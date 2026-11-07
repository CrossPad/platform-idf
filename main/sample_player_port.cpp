/**
 * @file sample_player_port.cpp
 * @brief The SD card port both sample engines read through, and the prebuilt
 *        streamer's library hooks.
 *
 * Shared by every build: the handle cache, the per-task file slots, the
 * internal-RAM floor for fopen(), the DMA stage, sample_port_pread() (the
 * native engine's reads), the note-latency accounting and the loaders'
 * pacing. Only with CONFIG_CROSSPAD_SAMPLER_MARCEL: the library's file
 * handles (SampleStreamPlayer_File*), its memory, ErrorMsg and NoteOn hooks
 * and its voice cap.
 *
 * @date 25.02.2026
 * @author Marcel Licence
 */


#include "sd_sector_io.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "engine_arena.hpp"
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include <ml_smpl_stream_ply.h>
#include "MarcelSampleNode.hpp"
#endif
#include "crosspad_s3.h"
#include "bsp_sdmmc.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "SamplerEnginePort.hpp"
#include "audio/audio_task.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <atomic>
#include <sys/stat.h>
#include <algorithm>


bool sample_player_any_sounding(void);
/* weight 1 = the kit loader; a cosmetic reader passes more and waits longer. */
void sample_player_port_yield_to_streamer(uint32_t weight);
extern std::atomic<uint32_t> g_bg_pass_last_us;   /* audio_module.cpp */


/* A path read through open_resolved() lands in "the open file" of its task's
 * slot. audio_bg streams through its own slot, the mutators (kit loader,
 * LVGL, engine teardown) share another, and the RT renderer, which only ever
 * closes, gets one that is always empty. With one slot for all of them the
 * streamer had to stand aside for every read a kit load made — 30..200 ms
 * each — and voices that kept sounding across the swap ran dry. */
enum { SLOT_MUTATOR = 0, SLOT_STREAM = 1, SLOT_RT = 2, SLOT_COUNT = 3 };
static FILE *s_slot[SLOT_COUNT];

static int slot_index(void)
{
    const char *n = pcTaskGetName(NULL);
    if (n && strcmp(n, "audio_bg") == 0) return SLOT_STREAM;
    if (n && strcmp(n, "audio_rt") == 0) return SLOT_RT;
    return SLOT_MUTATOR;
}
static FILE *&cur_file(void) { return s_slot[slot_index()]; }

/* Under s_lock. A handle another slot is reading from cannot be shared: two
 * tasks seeking and reading one FILE interleave their positions. */
static bool live_elsewhere_locked(FILE *f, int mySlot)
{
    for (int i = 0; i < SLOT_COUNT; i++) {
        if (i != mySlot && s_slot[i] == f) return true;
    }
    return false;
}

/* Open handles, kept. The library switches its file per refill, which meant an
 * fopen/fclose per chunk — hundreds of FAT opens a second across a kit, and
 * the source of "buffer underrun". Reopening an already-open path now costs a
 * rewind. The mount allows 20 descriptors; this keeps at most kFileCache.
 *
 * Three tasks reach this state (audio_bg, the kit loader/panel under the file
 * mutex, and the RT renderer, which closes from inside the library's render
 * call and is serialized by nothing), so every transition of a slot and the
 * cache is made under one spinlock, with fopen/fclose outside it.
 *
 * The RT task never closes: fclose() on FAT is a directory sync (50-110 ms on
 * the render task), and a recycled handle got closed twice. What the renderer
 * detaches is parked for the next open on a task that may do SD I/O. */
struct OpenFile { char path[256]; FILE *f; uint32_t used; int64_t streamedUs; uint8_t *stdioBuf; };
/* An entry the streamer refilled through this recently is a voice that is
 * probably still sounding; evicting it costs that voice a directory walk on
 * its next refill. Older than this and it is the previous kit's file, which
 * a swap wants out of the way before the new kit's files. */
static constexpr int64_t kStreamProtectUs = 1000000;
/* A FILE's stdio buffer is ~1 kB of internal RAM, allocated by newlib on the
 * first read; twelve cached handles were 12 kB of the ~25 kB the board has
 * left with the Sampler up. Each cache slot lends its handle a buffer in
 * PSRAM instead (setvbuf before any I/O). Every fread() passes through it,
 * whatever its length -- this newlib's fread() has no large-read bypass
 * (libc_a-fread.o calls only __srefill_r and memcpy) -- so a read comes off
 * the card 1 kB at a time; the loader's preload goes around it
 * (read_staged()). */
static constexpr size_t kStdioBuf = 1024;
/* Twelve. With eight, the loader's opens evicted the handles the refill
 * needed on a pad's first hit — and every evicted one is a FAT directory
 * walk (40..160 ms on a 1-bit card) inside the streamer's pass, which is a
 * late start of the voice, or a hole in whichever was nearest empty. Not
 * sixteen: every descriptor of the mount (bsp_sdmmc.c max_files, 20) is a
 * 4 kB FIL in PSRAM at mount time, and CONFIG_FATFS_FS_LOCK caps the open
 * files too — the streamer's own copies, kit.json and the waveform loader
 * need the rest, and at 16 the card refused opens. Each handle keeps a
 * stdio buffer, lent from PSRAM below: unbuffered, newlib refills one byte
 * at a time and a 4 kB fread became 4096 reads under the FAT lock — the
 * loader spun into the watchdog. */
/* Eighteen. Twelve could not hold a whole kit: with 16+ samples on the pads
 * every entry was "recently used", the kStreamProtectUs guard had nothing
 * cold to pick, and a 16-pad pattern evicted a sounding voice's handle on
 * nearly every note (measured: g_port_evict +204 over 240 notes at 20 hits/s).
 * The ceiling that forced twelve was the mount's max_files = 20 and
 * CONFIG_FATFS_FS_LOCK = 20 -- at sixteen the card refused opens -- and both
 * are 32 now, so a kit fits with room for the streamer's own copies,
 * kit.json and the waveform loader. */
static constexpr int kFileCache = 18;
/* In PSRAM. The array is ~288 B an entry and this board boots with a couple
 * of hundred bytes of internal RAM to spare (see the internal-RAM cliff in
 * CLAUDE.md); eighteen entries internal would be ~5 kB taken from the DMA
 * pool. It is touched only when a file is opened or evicted, never per
 * sample, so the extra access latency inside s_lock does not reach the
 * render. */
EXT_RAM_BSS_ATTR static OpenFile s_files[kFileCache];
static uint32_t s_fileUse = 0;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Internal RAM a fopen() must be able to draw on without aborting the board:
 * the FILE's recursive mutex (~100 B, pvPortMalloc, internal only), the VFS
 * fd slot, and the 512 B DMA buffer sdmmc allocates for the first read. */
#define kMinInternalForOpen 1536u
static uint32_t s_lowMemRefusals = 0;
static constexpr int kDeferred = 4;
struct Parked { FILE *f; uint8_t *buf; };   /* buf: its stdio buffer, freed after it, or NULL */
static Parked s_deferred[kDeferred];
uint32_t g_port_defer = 0;        /* handles the RT task handed off for closing */
uint32_t g_port_defer_lost = 0;   /* no room to park: descriptor leaked rather than closed on RT */
uint32_t g_port_evict = 0;        /* cache slots recycled */
std::atomic<uint32_t> g_port_stream_fopen{0};   /* real fopen() calls the streamer had to make */
std::atomic<uint32_t> g_port_fopen_max_us{0};   /* slowest fopen() since the swap log last read it */
std::atomic<uint32_t> g_port_stream_bytes{0};   /* bytes the streamer read off the card */
std::atomic<uint32_t> g_port_mutator_bytes{0};  /* bytes the loader (and other mutators) read */

static bool is_rt_task(void)
{
    const char *n = pcTaskGetName(NULL);
    return n && strcmp(n, "audio_rt") == 0;
}

/* Under s_lock. */
static FILE *cache_lookup_locked(const char *path, int mySlot)
{
    for (auto &e : s_files) {
        if (e.f && strcmp(e.path, path) == 0 && !live_elsewhere_locked(e.f, mySlot)) {
            e.used = ++s_fileUse;
            if (mySlot == SLOT_STREAM) e.streamedUs = esp_timer_get_time();
            return e.f;
        }
    }
    return NULL;
}

static bool cache_has_locked(FILE *f)
{
    for (auto &e : s_files) {
        if (e.f == f) return true;
    }
    return false;
}

/* Under s_lock. Returns the handle displaced from the cache (to be closed by
 * the caller, outside the lock), or NULL. */
static FILE *cache_insert_locked(const char *path, FILE *f, int mySlot)
{
    OpenFile *slot = NULL;
    FILE *evicted = NULL;
    for (auto &e : s_files) {
        if (!e.f) { slot = &e; break; }
    }
    if (!slot) {
        /* Least recently used — but a handle the streamer refilled through
         * within kStreamProtectUs goes last: evicting one costs that voice a
         * FAT directory walk on its next refill, and eight of those in one
         * pass were a hole in every voice. Protected for good, the previous
         * kit's files sat in the cache while the new kit's evicted each
         * other, and every first hit on a fresh pad started with a walk. */
        const int64_t now = esp_timer_get_time();
        for (int pass = 0; pass < 2 && !slot; pass++) {
            uint32_t oldest = UINT32_MAX;
            for (auto &e : s_files) {
                if (live_elsewhere_locked(e.f, -1)) continue;
                if (pass == 0 && now - e.streamedUs < kStreamProtectUs) continue;
                if (e.used < oldest) { oldest = e.used; slot = &e; }
            }
        }
        if (!slot) return NULL;         /* every entry is live in some slot: cannot happen with >3 */
        g_port_evict++;
        evicted = slot->f;
    }
    snprintf(slot->path, sizeof(slot->path), "%s", path);
    slot->f = f;
    slot->used = ++s_fileUse;
    slot->streamedUs = (mySlot == SLOT_STREAM) ? esp_timer_get_time() : 0;
    return evicted;
}

/* Outside s_lock, right after the insert and before any read on `f`. */
static void cache_lend_buffer(FILE *f)
{
    for (auto &e : s_files) {
        if (e.f != f) continue;
        if (!e.stdioBuf) {
            e.stdioBuf = (uint8_t *)heap_caps_malloc(kStdioBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        if (e.stdioBuf) setvbuf(f, (char *)e.stdioBuf, _IOFBF, kStdioBuf);
        return;
    }
}

/* Close what the RT task parked. Any task but audio_rt, outside s_lock. */
static void drain_deferred(void)
{
    Parked batch[kDeferred];
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < kDeferred; i++) { batch[i] = s_deferred[i]; s_deferred[i] = Parked{}; }
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < kDeferred; i++) {
        if (!batch[i].f) continue;
        fclose(batch[i].f);
        heap_caps_free(batch[i].buf);
    }
}

/* Who reaches into the file port, by task. Every open, seek and read is
 * meant to come from audio_bg (streaming) or a mutator holding the file
 * mutex; a caller outside those two is a race with the stream. */
struct PortCallers { uint32_t audio_bg; uint32_t other; char lastOther[16]; };
PortCallers g_port_opens = {};
PortCallers g_port_seeks = {};
PortCallers g_port_closes = {};
PortCallers g_port_reads = {};
static void note_caller(PortCallers &c)
{
    const char *n = pcTaskGetName(NULL);
    if (n && strcmp(n, "audio_bg") == 0) { c.audio_bg++; return; }
    c.other++;
    snprintf(c.lastOther, sizeof(c.lastOther), "%s", n ? n : "?");
}

void sample_player_port_flush_files(void)
{
    FILE *batch[kFileCache];
    int n = 0;
    portENTER_CRITICAL(&s_lock);
    for (auto &e : s_files) {
        if (e.f && !live_elsewhere_locked(e.f, -1)) batch[n++] = e.f;
        e.f = NULL; e.path[0] = '\0'; e.streamedUs = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < n; i++) fclose(batch[i]);
    drain_deferred();
}

/* One file's cached handle closed -- unless a slot is reading through it --
 * so the file can be removed (FATFS refuses an open one): the kit rate
 * conversion's tidy, for the originals it replaces. */
void sample_player_port_forget_file(const char *path)
{
    FILE *f = NULL;
    portENTER_CRITICAL(&s_lock);
    for (auto &e : s_files) {
        if (!e.f || strcmp(e.path, path) != 0 || live_elsewhere_locked(e.f, -1)) continue;
        f = e.f;
        e.f = NULL; e.path[0] = '\0'; e.streamedUs = 0;
        break;
    }
    portEXIT_CRITICAL(&s_lock);
    if (f) fclose(f);
}

/* sample_player_port_forget_dir()'s request to the streamer: let go of its
 * slot's file if it lies under s_letGoDir. That slot is audio_bg's alone --
 * swapped by any other task it could go under a read -- so audio_bg does it
 * itself, between passes. */
static char s_letGoDir[sizeof(OpenFile::path)];
static std::atomic<uint32_t> s_letGoAsked{0};
static std::atomic<uint32_t> s_letGoDone{0};

static bool path_under(const char *path, const char *dir, size_t n)
{
    return strncmp(path, dir, n) == 0 && path[n] == '/';
}

void sample_player_port_stream_let_go(void)
{
    const uint32_t asked = s_letGoAsked.load(std::memory_order_acquire);
    if (asked == s_letGoDone.load(std::memory_order_relaxed)) return;
    const size_t n = strlen(s_letGoDir);
    portENTER_CRITICAL(&s_lock);
    FILE *f = s_slot[SLOT_STREAM];
    for (auto &e : s_files) {
        if (f && e.f == f && path_under(e.path, s_letGoDir, n)) {
            s_slot[SLOT_STREAM] = NULL;   /* still cached: closed by the asker */
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    s_letGoDone.store(asked, std::memory_order_release);
}

/* Every cached handle on a file under `dir` closed, so the folder can be
 * removed (FATFS refuses an open file). A kit that has just stopped being
 * current keeps a kit's worth of them in the cache -- and its last streamed
 * one in the streamer's slot -- until other files push them out, and the
 * browser's delete failed until another kit had been played. The streamer
 * lets go of its slot first (sample_player_port_stream_let_go(), within a
 * pass or two). The mutators' slot is left alone: after a kit change it holds
 * the new kit's last file. A handle a slot opens again meanwhile -- a sample
 * of that kit still sounding -- stays open, and the removal fails as before.
 * The LVGL thread only (the kit browser and KIT_DELETE). */
void sample_player_port_forget_dir(const char *dir)
{
    static constexpr int kLetGoWaitMs = 50;
    snprintf(s_letGoDir, sizeof(s_letGoDir), "%s", dir);
    const uint32_t asked = s_letGoAsked.fetch_add(1, std::memory_order_acq_rel) + 1;
    for (int i = 0; i < kLetGoWaitMs && s_letGoDone.load(std::memory_order_acquire) != asked; i++) {
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    const size_t n = strlen(dir);
    FILE *batch[kFileCache];
    int k = 0;
    portENTER_CRITICAL(&s_lock);
    for (auto &e : s_files) {
        if (!e.f || !path_under(e.path, dir, n) || live_elsewhere_locked(e.f, -1)) continue;
        batch[k++] = e.f;
        e.f = NULL; e.path[0] = '\0'; e.streamedUs = 0;
    }
    portEXIT_CRITICAL(&s_lock);
    for (int i = 0; i < k; i++) fclose(batch[i]);
    drain_deferred();
}

/* Preloads served from a prefetch / missed, since boot: KIT_SWAP_LOG reports
 * them; nothing prefetches, so both stay 0. */
EXT_RAM_BSS_ATTR uint32_t g_port_pf_hits, g_port_pf_misses;

/* Called from the audio task -- the render itself, inside the library's
 * Process(). Once the WAV block pool runs dry the library reports on every
 * buffer, with an empty string — 220 000 lines in a 4-minute run. Drop the
 * empty ones; count the rest and keep the latest text. The line is printed
 * by sample_player_port_report_errors() off the render: printf on UART0,
 * which has no driver, is a busy wait, and it sat inside the very rt_max it
 * reported. */
std::atomic<uint32_t> g_engine_noblock{0};    /* "no free block" reports (one per starved buffer) */
std::atomic<uint32_t> g_engine_underrun{0};   /* "buffer underrun" reports */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL /* the library's error hook and its underrun line */
EXT_RAM_BSS_ATTR static char s_errMsg[40];    /* latest report; a torn copy is only cosmetic */
static std::atomic<uint32_t> s_errCount{0};
/* The line goes out up to a second after the report, from the worker, so it
 * says when the reports came (ms since boot, the clock of the "I (ms)" lines:
 * the line's first and latest). An underrun gets a line of its own, the first
 * one until that line is out: where the streamer was when it came (see
 * g_bg_pass_mark_us) -- in a pass and for how long, or idle since when;
 * within that pass the bounce ticks it slept and the pieces it read around
 * the stage; how long the read() it was in had been going; who held the
 * stage: s(treamer), b(ounce), m(utator) -- and then how the two passes that
 * end after it went: one of them owed the voice its refill (the one under way
 * if it had not reached the voice yet, else the next). That instant alone
 * cannot tell a streamer held up after it from one that was not: a pass a few
 * ms old at the underrun can still run for the whole hole. */
EXT_RAM_BSS_ATTR static uint32_t s_errFirstMs, s_errLastMs;
struct PassEnd { uint32_t us, readMaxUs; uint16_t yld, fb; };
struct UnderrunAt {
    uint32_t ms, bgUs, readUs;
    uint16_t yld, fb;
    char bg, stage;
    PassEnd end[2];                           /* filled by audio_bg as the passes end */
};
EXT_RAM_BSS_ATTR static UnderrunAt s_ur;
static volatile bool s_urTaken;               /* s_ur holds one not yet printed */
static volatile uint8_t s_urEnded;            /* how many of its s_ur.end[] are in */
#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */
extern std::atomic<uint32_t> g_bg_pass_mark_us;   /* audio_module.cpp */
/* The streamer's pass under way, written by audio_bg only. */
static uint16_t s_passYld, s_passFb;
static uint32_t s_passReadMaxUs;              /* its longest read() of a piece, FAT lock wait included */
static volatile uint32_t s_readUs;            /* the streamer's read() under way: its start | 1, else 0 */
static volatile char s_stageBy;               /* the stage's holder, 0 when free */

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
void sample_player_port_pass_begin(void)
{
    s_passYld = 0;
    s_passFb = 0;
    s_passReadMaxUs = 0;
}

void sample_player_port_pass_end(uint32_t passUs)
{
    if (!s_urTaken) return;           /* then the count: the render zeroes it before it raises this */
    const uint8_t k = s_urEnded;
    if (k >= 2) return;
    s_ur.end[k] = PassEnd{passUs, s_passReadMaxUs, s_passYld, s_passFb};
    s_urEnded = k + 1;
}

void SampleStreamPlayer_ErrorMsg(const char *msg)
{
    if (msg == NULL || msg[0] == '\0')
    {
        return;
    }
    const uint32_t nowMs = esp_log_timestamp();
    if (!s_errFirstMs) s_errFirstMs = nowMs;   /* the worker zeroes it as it takes the line */
    s_errLastMs = nowMs;
    if (strstr(msg, "no free block")) ++g_engine_noblock;
    else if (strstr(msg, "underrun")) {
        ++g_engine_underrun;
        if (!s_urTaken) {
            const uint32_t now = (uint32_t)esp_timer_get_time();
            const uint32_t mark = g_bg_pass_mark_us.load(std::memory_order_relaxed);
            const uint32_t rd = s_readUs;
            const char by = s_stageBy;
            s_ur = UnderrunAt{nowMs, now - mark, rd ? now - rd : 0, s_passYld, s_passFb,
                              (mark & 1u) ? 'p' : 'i', by ? by : '-', {}};
            s_urEnded = 0;
            s_urTaken = true;
        }
    }
    strlcpy(s_errMsg, msg, sizeof(s_errMsg));
    s_errCount.fetch_add(1, std::memory_order_release);
}

/* The underrun's own line, once both passes after it have ended -- or 2 s on,
 * with what came ("-" for a pass that did not end: the streamer is stuck).
 * p1/p2 = pass us/bounce ticks/pieces around the stage/longest piece us. */
static void report_underrun(void)
{
    if (!s_urTaken) return;
    const uint8_t ended = s_urEnded;
    const UnderrunAt u = s_ur;
    if (ended < 2 && esp_log_timestamp() - u.ms < 2000) return;
    s_urTaken = false;
    char p[2][40];
    for (int i = 0; i < 2; i++) {
        if (i < ended) {
            snprintf(p[i], sizeof(p[i]), "%" PRIu32 "us/%u/%u/%" PRIu32 "us", u.end[i].us,
                     (unsigned)u.end[i].yld, (unsigned)u.end[i].fb, u.end[i].readMaxUs);
        } else {
            strcpy(p[i], "-");
        }
    }
    /* One printf: a line in pieces interleaves with other tasks' logs. */
    printf("\033[31mengine underrun_at=%" PRIu32 "ms bg_%s=%" PRIu32 "us read=%" PRIu32
           "us yld=%u fb=%u stage=%c p1=%s p2=%s\033[0m\n",
           u.ms, u.bg == 'p' ? "pass" : "idle", u.bgUs, u.readUs, (unsigned)u.yld, (unsigned)u.fb,
           u.stage, p[0], p[1]);
}

/* At most once a second, from the worker (main.cpp pad_anim_job): the
 * engine's latest report and how many came since the previous line -- while
 * the engine is in trouble, the last words before a task watchdog takes the
 * board, when no CDC verb can still be asked. */
void sample_player_port_report_errors(void)
{
    static uint32_t s_errPrinted = 0;
    const uint32_t count = s_errCount.load(std::memory_order_acquire);
    if (count != s_errPrinted) {
        char msg[sizeof(s_errMsg)];
        memcpy(msg, s_errMsg, sizeof(msg));
        msg[sizeof(msg) - 1] = '\0';
        const uint32_t rtMax = crosspad::marcelSampleRtMaxLogUs().exchange(0);
        /* Taken before the print, a report that comes during it (UART0 is a
         * busy wait) opens the next line. */
        const uint32_t firstMs = s_errFirstMs, lastMs = s_errLastMs;
        s_errFirstMs = 0;
        /* The line can come after the engine was stopped, and the library
         * must not be asked anything then: -1. */
        const int freeWavs = crosspad::marcelEngineReady().load(std::memory_order_acquire)
                                 ? (int)SampleStreamPlayer_GetFreeWavCnt() : -1;
        printf("\033[31m%s (+%" PRIu32 " suppressed) at=%" PRIu32 "..%" PRIu32 "ms rt_max=%" PRIu32
               "us free=%d closes=%" PRIu32 "/%" PRIu32 "(%s) evict=%" PRIu32 " rt_busy_max=%" PRIu32 "us\033[0m\n",
               msg, count - s_errPrinted - 1, firstMs, lastMs, rtMax,
               freeWavs,
               g_port_closes.audio_bg, g_port_closes.other, g_port_closes.lastOther, g_port_evict,
               audio_rt_busy_max(AUDIO_RT_BUSY_ENGINE_LINE, NULL));
        s_errPrinted = count;
    }
    report_underrun();
}
#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */

void fileSeekTo(uint32_t pos)
{
    note_caller(g_port_seeks);
    FILE *f = cur_file();
    if (f)
    {
        /* Emptied first, the seek is a plain lseek(): newlib's fseek() would
         * refill 1 kB at the target, a card read before every refill, which
         * goes around stdio anyway (read_staged()). */
        fflush(f);
        fseek(f, pos, SEEK_SET);
    }
}

/* A preload (SetupSample(): a voice's share of the pool, ~96 kB a layer),
 * staged through internal RAM. Through stdio it came 1 kB at a time (see
 * kStdioBuf), and FATFS handed each sector of it to sdmmc with a PSRAM
 * destination, which the S3's SDMMC cannot DMA into: a CMD17 and a CMD13
 * through a 512 B bounce per sector, ~400 transactions a layer, each a wake
 * of the reader -- for kit_load, on core 1 under audio_rt. Read through the
 * descriptor into a DMA-capable buffer in pieces that end on 4 kB of the
 * file, FATFS hands each piece to sdmmc whole: one CMD18 (with clusters of
 * 4 kB or more a piece never spans two). Every mutator comes here: kit_load,
 * app_main's STARTUP.wav, the LVGL thread's previews.
 *
 * The buffer is the board's one stage (below), taken a piece at a time; when
 * there is none, or the descriptor cannot say where it is, nothing is read
 * and the caller goes through stdio as before -- counted, for KIT_SWAP_LOG.
 *
 * The streamer reads the same way (readBytes()): the library refills a voice
 * with one call of up to 96 kB, which through stdio was ~190 CMD17+CMD13
 * pairs, and pass after pass of those left the card no time for a bounce. */
static constexpr uint32_t kStage = 4096;
EXT_RAM_BSS_ATTR uint32_t g_port_staged;            /* preloads read around stdio */
EXT_RAM_BSS_ATTR uint32_t g_port_stage_fallback;    /* preloads that went through it */
EXT_RAM_BSS_ATTR uint32_t g_port_stream_staged;     /* refills read around stdio */
EXT_RAM_BSS_ATTR uint32_t g_port_stream_fb;         /* refills, or 4 kB pieces of one, read without the stage */
EXT_RAM_BSS_ATTR uint32_t g_port_stream_read_max_us; /* longest refill read (no fopen); SMPLLAST clears */

/* One 4 kB DMA-capable buffer for every bulk transfer that goes around stdio:
 * the streamer's refills, the mutators' preloads, a bounce's pieces
 * (SongEnginePort.cpp). Taken once, before BLE asks for its block, so it
 * never cuts the largest one later -- three buffers of their own were 12 kB
 * at the worst moment, at the 63 kB floor. Order: stage, then FAT -- nothing
 * takes the stage while holding a FILE's lock. A mutex, so a lower-priority
 * holder runs at its waiter's priority.
 *
 * Taken for one piece and given back after it -- but a FreeRTOS mutex is not
 * handed to whoever waits: the streamer (prio 18) gives and takes again before
 * a waiter on its core ever runs, so a waiter gets the stage only between two
 * of the streamer's refills, when it sleeps. Hence:
 *  - The streamer never waits for it. A piece it finds taken goes straight
 *    into the voice's buffer (PSRAM: a CMD17 per sector through sdmmc's
 *    bounce, the old cost; counted in stream_fb). Waiting, it would wait at
 *    the holder's pace -- a kit load on core 1 gets no CPU under a busy
 *    render -- and the inheritance would stop at the holder: a holder that
 *    waits for the FAT lock does not pass priority 18 on to whoever holds
 *    that. Reading directly, the streamer waits on the FAT lock itself.
 *  - A bounce piece has a deadline (the take's ring) and says it is waiting
 *    (urgent): the streamer then steps aside for a tick after its piece --
 *    while its pass is young (kYieldToBounceUs). Past that the pass is
 *    voices waiting for their refill, and a step aside can cost them more
 *    than the tick: the bounce's piece is a card write, during which the
 *    streamer's next piece reads around the stage and waits on the FAT lock
 *    for the write to end. The take's ring holds up to 743 ms (186 at the
 *    least a bounce starts with); waiting out the rest of a pass (158 ms the
 *    longest seen, under a kit load) costs it a part of that, and a ring
 *    that runs out fails the take as "card too slow" rather than leaving a
 *    hole. A kit load is not urgent: it paces itself on the streamer
 *    instead (sample_player_port_yield_to_streamer). */
static uint8_t *s_stage;
static SemaphoreHandle_t s_stageMutex;
static std::atomic<uint32_t> s_stageUrgent{0};   /* urgent takers waiting for the stage */
/* The loader's kPassBusyUs: a pass this old is refilling voices that wait. A
 * new voice went silent for 57 ms from its first block, mid-bounce, 12 hits/s
 * on one pad (live_set 06.10 01:41, ea83f96). */
static constexpr uint32_t kYieldToBounceUs = 20000;

void sample_player_port_stage_init(void)
{
    s_stageMutex = xSemaphoreCreateMutex();
    /* The sector layer bounces every transfer to or from PSRAM a multi-sector
     * command at a time (sd_sector_io.cpp): with it there is nothing for a stage
     * to do, and its 4 kB of internal RAM stay free. Users then read and
     * write their own buffers directly (sample_player_port_direct_io()). */
    if (sd_bounce_ready()) return;
    if (s_stageMutex) s_stage = (uint8_t *)heap_caps_malloc(kStage, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!s_stage) ESP_LOGE("sample_port", "no DMA stage: bulk card I/O goes through stdio");
}

/* NULL without a stage; otherwise held until _give(). @p urgent: the streamer
 * steps aside while this waits (a bounce piece). */
uint8_t *sample_player_port_stage_take(bool urgent)
{
    if (!s_stage) return NULL;
    if (urgent) s_stageUrgent.fetch_add(1, std::memory_order_relaxed);
    xSemaphoreTake(s_stageMutex, portMAX_DELAY);
    if (urgent) s_stageUrgent.fetch_sub(1, std::memory_order_relaxed);
    s_stageBy = urgent ? 'b' : 'm';
    return s_stage;
}

bool sample_player_port_direct_io(void)
{
    return !s_stage && sd_bounce_ready();
}

void sample_player_port_stage_give(void)
{
    s_stageBy = 0;
    xSemaphoreGive(s_stageMutex);
}

/* A read in one read(), straight into its buffer in PSRAM: the card's sector
 * layer bounces it through internal RAM a multi-sector command at a time
 * (sd_sector_io.cpp), so FATFS walks the file once for the whole read
 * instead of once a 4 kB piece. */

/* @p stream: the streamer's refill, read whole. A mutator's preload (a kit
 * load's 64 kB heads) goes in pieces with the card given away between them:
 * whole, a head held the card from the song's ring, which ran dry -- 75 blocks
 * of silence over five kit loads under an eight-track song (2026-10-27). */
static bool read_direct(FILE *f, uint8_t *buffer, uint32_t len, uint32_t &got, bool stream)
{
    constexpr uint32_t kPiece = 16384;
    got = 0;
    const int fd = __sfileno(f);
    const off_t start = (fd >= 0 && fflush(f) == 0) ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
    if (start < 0) return false;
    while (got < len) {
        const uint32_t want = stream ? len - got : std::min(kPiece, len - got);
        s_readUs = (uint32_t)esp_timer_get_time() | 1u;
        const ssize_t n = read(fd, buffer + got, want);
        const uint32_t us = (uint32_t)esp_timer_get_time() - s_readUs;
        if (us > s_passReadMaxUs) s_passReadMaxUs = us;
        s_readUs = 0;
        if (n <= 0) break;
        got += (uint32_t)n;
        if ((uint32_t)n < want || got == len) break;
        sample_player_port_yield_to_streamer(1);
    }
    fflush(f);
    fseek(f, start + (off_t)got, SEEK_SET);
    return true;
}

/* @p stream: the streamer's refill -- the stage only if it is free, no pacing
 * on itself; else a mutator's preload -- waits for the stage, paces itself on
 * the streamer. */
static bool read_staged(FILE *f, uint8_t *buffer, uint32_t len, uint32_t &got, bool stream)
{
    got = 0;
    /* No stage: the card's sector layer bounces (sd_bounce), so the read
     * goes straight into its PSRAM buffer, whole. */
    if (!s_stage) return sd_bounce_ready() && read_direct(f, buffer, len, got, stream);
    /* On a read stream fflush() moves the descriptor to the stream's position
     * and empties the buffer (newlib __sflush_r). */
    const int fd = __sfileno(f);    /* the macro: fileno() would link 94 B more */
    const off_t start = (fd >= 0 && fflush(f) == 0) ? lseek(fd, 0, SEEK_CUR) : (off_t)-1;
    if (start < 0) return false;
    off_t pos = start;
    while (got < len) {
        uint32_t want = kStage - (uint32_t)(pos % kStage);
        if (want > len - got) want = len - got;
        ssize_t n;
        uint8_t *stage = stream ? (xSemaphoreTake(s_stageMutex, 0) == pdTRUE ? s_stage : NULL)
                                : sample_player_port_stage_take(false);
        if (stream) {
            if (stage) s_stageBy = 's';
            else ++s_passFb;
            s_readUs = (uint32_t)esp_timer_get_time() | 1u;
        }
        if (stage) {
            n = read(fd, stage, want);
            if (n > 0) memcpy(buffer + got, stage, (size_t)n);
            sample_player_port_stage_give();
        } else {
            n = read(fd, buffer + got, want);
            ++g_port_stream_fb;
        }
        if (stream) {
            const uint32_t us = (uint32_t)esp_timer_get_time() - s_readUs;
            if (us > s_passReadMaxUs) s_passReadMaxUs = us;
            s_readUs = 0;
        }
        if (n <= 0) break;
        got += (uint32_t)n;
        pos += n;
        if ((uint32_t)n < want) break;
        if (got == len) break;
        if (!stream) sample_player_port_yield_to_streamer(1);
        else if (s_stageUrgent.load(std::memory_order_relaxed) &&
                 (uint32_t)esp_timer_get_time() - g_bg_pass_mark_us.load(std::memory_order_relaxed) <
                     kYieldToBounceUs) {
            ++s_passYld;
            vTaskDelay(1);
        }
    }
    /* The stream still holds the offset of before the reads. Flushed with an
     * empty buffer it marks the seek unoptimised (__SNPT): a plain lseek(),
     * not one that refills 1 kB from that stale offset. */
    fflush(f);
    fseek(f, pos, SEEK_SET);
    return true;
}

/* One read through @p f, on the terms of the task asking: the streamer's
 * unpaced, a mutator's in paced pieces. */
static uint32_t read_from(FILE *f, uint8_t *buffer, uint32_t len)
{
    const int slot = slot_index();
    if (slot == SLOT_STREAM)
    {
        /* Unpaced: this is the reader everyone else yields to. */
        const int64_t t0 = esp_timer_get_time();
        uint32_t n = 0;
        if (len >= 2 * kStage && read_staged(f, buffer, len, n, true)) ++g_port_stream_staged;
        else
        {
            if (len >= 2 * kStage) ++g_port_stream_fb;
            n = fread(buffer, 1, len, f);
        }
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        if (dt > g_port_stream_read_max_us) g_port_stream_read_max_us = dt;
        g_port_stream_bytes.fetch_add(n, std::memory_order_relaxed);
        return n;
    }
    if (slot != SLOT_MUTATOR)
    {
        return fread(buffer, 1, len, f);
    }
    /* The loader reads a whole sample in one library call — SetupSample()
     * preloads the file up to the voice's share of the pool — and one fread
     * of that size is one FAT read holding the mount lock for its duration:
     * 30..200 ms on a 1-bit card, during which the streamer's refill waits
     * and a sounding voice runs dry. In pieces, the lock is free between
     * them. That alone is not enough while voices sound: the card's bandwidth
     * is the limit (measured: a refill pass of 100..210 ms with eight voices
     * streaming beside a loader reading at ~2 MB/s, and a fixed tick per
     * piece did not bring it down), so the loader paces itself on the
     * streamer's last pass — sample_player_port_yield_to_streamer(). */
    constexpr uint32_t kChunk = 4096;
    if (len >= 2 * kStage)
    {
        uint32_t n = 0;
        if (read_staged(f, buffer, len, n, false))
        {
            ++g_port_staged;
            g_port_mutator_bytes.fetch_add(n, std::memory_order_relaxed);
            return n;
        }
        ++g_port_stage_fallback;
    }
    uint32_t got = 0;
    while (got < len)
    {
        const uint32_t want = (len - got) < kChunk ? (len - got) : kChunk;
        const uint32_t n = fread(buffer + got, 1, want, f);
        got += n;
        if (n < want) break;
        if (got < len) sample_player_port_yield_to_streamer(1);
    }
    g_port_mutator_bytes.fetch_add(got, std::memory_order_relaxed);
    return got;
}

uint32_t readBytes(uint8_t *buffer, uint32_t len)
{
    note_caller(g_port_reads);
    FILE *f = cur_file();
    if (!f)
    {
        return 0;
    }
    return read_from(f, buffer, len);
}



/* fopen() is not allowed to fail gracefully here: newlib's __sfp() creates
 * the FILE's recursive mutex with pvPortMalloc (internal RAM only) and
 * lock_init_generic() calls abort() when that returns NULL
 * (newlib/src/locks.c). Seen on hardware with BLE up: sdmmc could no longer
 * allocate its DMA buffer, one sample failed to open, and the next fopen took
 * the whole board down mid kit-load. An open that is refused reads as "unable
 * to open file" instead, which every caller already tolerates. */
static bool room_to_open(void)
{
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= kMinInternalForOpen)
    {
        return true;
    }
    s_lowMemRefusals++;
    return false;
}

/* An open walks the directory under the volume's lock -- 63 ms on one kit's
 * first sample -- and the streamer waits behind it; a reader that is not the
 * streamer opens between its passes. */
static FILE *timed_fopen(const char *path, const char *mode, int mySlot)
{
    if (mySlot == SLOT_MUTATOR) sample_player_port_yield_to_streamer(1);
    const int64_t t0 = esp_timer_get_time();
    FILE *f = fopen(path, mode);
    const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
    if (dt > g_port_fopen_max_us.load(std::memory_order_relaxed)) {
        g_port_fopen_max_us.store(dt, std::memory_order_relaxed);
    }
    if (mySlot == SLOT_STREAM) g_port_stream_fopen.fetch_add(1, std::memory_order_relaxed);
    return f;
}

/* Rate-limited: printf on the audio task blocks on the console UART, which
 * turns a recoverable error into dropped audio. */
static void report_open_failure(const char *path)
{
    static uint32_t fails = 0;
    if ((fails++ % 100) == 0)
    {
        printf("Error opening file: %s (%" PRIu32 " failures, %" PRIu32 " refused on low internal RAM)\n",
               path, fails, s_lowMemRefusals);
    }
}

/* A full path opened into the calling task's slot: the native sampler engine
 * addresses samples by full path and frame, and gets the handle cache, the
 * per-task slots, the internal-RAM floor and the deferred closes. */
static bool open_resolved(const char *path, const char *mode)
{
    /* Open first, then swap it in, and close whatever comes back out. Clearing
     * the slot first leaks: two concurrent opens both find it empty and the
     * later assignment drops the earlier file. The mount only has 32
     * descriptors (bsp_sdmmc.c max_files), and once they are gone fatfs logs
     * every failure with a blocking vprintf from the audio task. */
    note_caller(g_port_opens);
    if (!is_rt_task()) drain_deferred();
    const bool readOnly = (mode && strcmp(mode, "rb") == 0);
    FILE *opened = NULL;
    /* The slot is assigned inside the same critical section that finds or
     * inserts the handle: a handle that is in the cache but in no slot yet is
     * fair game for another task's eviction, and the streamer evicting the
     * loader's fresh handle between the insert and the assignment closed it
     * twice -- a heap-corruption assert in fclose on the loader. */
    const int mySlot = slot_index();
    FILE *stale = NULL;
    bool staleCached = false;
    if (readOnly)
    {
        portENTER_CRITICAL(&s_lock);
        opened = cache_lookup_locked(path, mySlot);
        if (opened)
        {
            stale = s_slot[mySlot];
            s_slot[mySlot] = opened;
            staleCached = stale && cache_has_locked(stale);
        }
        portEXIT_CRITICAL(&s_lock);
    }
    if (opened)
    {
        fseek(opened, 0, SEEK_SET); /* a fresh open starts at 0; so does a reuse */
    }
    else if (room_to_open())
    {
        opened = timed_fopen(path, mode, mySlot);
        FILE *evicted = NULL;
        portENTER_CRITICAL(&s_lock);
        if (opened && readOnly)
        {
            evicted = cache_insert_locked(path, opened, mySlot);
        }
        stale = s_slot[mySlot];
        s_slot[mySlot] = opened;
        staleCached = stale && cache_has_locked(stale);
        portEXIT_CRITICAL(&s_lock);
        if (evicted) fclose(evicted);
        if (opened && readOnly) cache_lend_buffer(opened);
    }
    if (stale && stale != opened && !staleCached)
    {
        fclose(stale);
    }

    if (opened == NULL)
    {
        report_open_failure(path);
        return false;
    }
    return true;
}

/* Bytes [offset, offset + bytes) of a full path, in one call.
 *
 * The engine this serves issues its reads out of order -- one voice is three
 * seconds into its sample while another is starting -- so a seek and a read
 * as two calls would only be atomic under a lock held across both. One call
 * that carries its own offset needs no such lock, and lands on the same
 * cached handle, the same per-task slot and the same pacing against the
 * loader that every other read here gets. */
/* The size of a full path's file, through the same cached handle: the WAV
 * parser clamps a data chunk to it, as a read past the end would. 0 when it
 * cannot be opened. */
extern "C" uint32_t sample_port_size(const char *fullPath)
{
    if (!fullPath || !open_resolved(fullPath, "rb")) return 0;
    FILE *f = cur_file();
    struct stat st;
    return f && fstat(__sfileno(f), &st) == 0 && st.st_size > 0 ? (uint32_t)st.st_size : 0;
}

extern "C" uint32_t sample_port_pread(const char *fullPath, uint32_t offset, void *dst, uint32_t bytes)
{
    if (!fullPath || !dst || bytes == 0) return 0;
    if (!open_resolved(fullPath, "rb")) return 0;
    fileSeekTo(offset);
    return readBytes(static_cast<uint8_t *>(dst), bytes);
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/* The library's files (SampleStreamPlayer_File*). It opens a file, keeps the
 * handle for as long as it needs it and passes it to every seek and read. A
 * handle is the 1-based index into s_handles; the library's "no file" is
 * kNoFile. Nothing is cached: an open is an fopen(), a close an fclose(), and
 * the library decides how long a file stays open. The handles share the
 * mount's 32 descriptors with the cache above.
 *
 * Each handle remembers where its next read starts, so the seek the library
 * issues before a refill is skipped when it asks for the place the last read
 * ended at: a voice streaming along its file never seeks. A read-only handle
 * gets a kStdioBuf stdio buffer from PSRAM, and every read goes through
 * read_from(), paced by the task that asks like any other.
 *
 * Three tasks open, read and close them: the streamer (audio_bg) for the
 * voices it refills, the mutators for the samples they set up, and the render,
 * which hands a finished voice's file back from audio_rt. So a seek or a read
 * holds its entry for the call: a close that arrives meanwhile only marks it,
 * the last user lets go of the FILE, and the entry is not reused under a
 * reader. A FILE the render lets go of is parked
 * for the next open on a task that may do SD I/O (fclose() on FAT is a
 * directory sync, 50-110 ms). Nobody waits on anybody. */
static constexpr uint32_t kNoFile = 0xFFFFFFFFu;
static constexpr uint32_t kHandles = 32;
struct Handle
{
    FILE *f;         /* NULL: a free entry */
    uint8_t *buf;    /* its stdio buffer, lent from PSRAM */
    uint32_t pos;    /* where the next read starts */
    uint8_t users;   /* seeks and reads under way on it */
    bool closing;    /* closed while in use: the last user lets go of it */
};
EXT_RAM_BSS_ATTR static Handle s_handles[kHandles];
static std::atomic<uint32_t> s_lastHandle{0};   /* opened, seeked or read last: where writeBytes() goes */

/* "kits/a.wav", "/kits/a.wav" and "/sdcard/kits/a.wav" are the same file. */
static bool resolve_path(const char *name, char *out, size_t n)
{
    const size_t m = strlen(BSP_SD_MOUNT_POINT);
    if (strncmp(name, BSP_SD_MOUNT_POINT, m) == 0 && (name[m] == '/' || name[m] == '\0'))
    {
        return snprintf(out, n, "%s", name) < (int)n;
    }
    while (*name == '/') ++name;
    return snprintf(out, n, "%s/%s", BSP_SD_MOUNT_POINT, name) < (int)n;
}

static Handle *handle_acquire(uint32_t h)
{
    Handle *hd = nullptr;
    portENTER_CRITICAL(&s_lock);
    if (h >= 1 && h <= kHandles && s_handles[h - 1].f && !s_handles[h - 1].closing)
    {
        hd = &s_handles[h - 1];
        ++hd->users;
    }
    portEXIT_CRITICAL(&s_lock);
    return hd;
}

/* A FILE nobody uses any more: closed here, or parked when this is the render
 * (descriptor leaked, counted, when there is no room to park). */
static void handle_retire(FILE *f, uint8_t *buf)
{
    if (!is_rt_task())
    {
        fclose(f);
        heap_caps_free(buf);
        return;
    }
    bool parked = false;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < kDeferred; i++)
    {
        if (!s_deferred[i].f) { s_deferred[i] = Parked{f, buf}; parked = true; break; }
    }
    portEXIT_CRITICAL(&s_lock);
    if (parked) g_port_defer++; else g_port_defer_lost++;
}

static void handle_release(Handle *hd)
{
    FILE *f = nullptr;
    uint8_t *buf = nullptr;
    portENTER_CRITICAL(&s_lock);
    if (--hd->users == 0 && hd->closing)
    {
        f = hd->f;
        buf = hd->buf;
        hd->f = NULL;
        hd->buf = NULL;
        hd->closing = false;
    }
    portEXIT_CRITICAL(&s_lock);
    if (f) handle_retire(f, buf);
}

static void handle_close(uint32_t h)
{
    note_caller(g_port_closes);
    FILE *f = nullptr;
    uint8_t *buf = nullptr;
    portENTER_CRITICAL(&s_lock);
    if (h >= 1 && h <= kHandles && s_handles[h - 1].f && !s_handles[h - 1].closing)
    {
        Handle &e = s_handles[h - 1];
        if (e.users)
        {
            e.closing = true;   /* a seek or read is inside: it lets go of the file */
        }
        else
        {
            f = e.f;
            buf = e.buf;
            e.f = NULL;
            e.buf = NULL;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    if (f) handle_retire(f, buf);
}

uint32_t SampleStreamPlayer_FileOpen(const char *filename, const char *mode)
{
    note_caller(g_port_opens);
    char path[sizeof(OpenFile::path)];
    if (!filename || !mode || !resolve_path(filename, path, sizeof(path))) return kNoFile;
    if (!is_rt_task()) drain_deferred();
    FILE *f = room_to_open() ? timed_fopen(path, mode, slot_index()) : NULL;
    uint32_t h = 0;
    if (f)
    {
        const bool readOnly = mode[0] == 'r' && !strchr(mode, '+');
        uint8_t *buf = readOnly ? (uint8_t *)heap_caps_malloc(kStdioBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
        if (buf) setvbuf(f, (char *)buf, _IOFBF, kStdioBuf);
        portENTER_CRITICAL(&s_lock);
        for (uint32_t i = 0; i < kHandles; i++)
        {
            if (s_handles[i].f) continue;
            s_handles[i] = Handle{f, buf, 0, 0, false};
            h = i + 1;
            break;
        }
        portEXIT_CRITICAL(&s_lock);
        if (!h)   /* every entry is taken: the descriptor goes back */
        {
            fclose(f);
            heap_caps_free(buf);
        }
    }
    if (!h)
    {
        report_open_failure(path);
        return kNoFile;
    }
    s_lastHandle.store(h, std::memory_order_relaxed);
    return h;
}

void SampleStreamPlayer_FileClose(uint32_t fileIdx)
{
    uint32_t last = fileIdx;
    s_lastHandle.compare_exchange_strong(last, 0, std::memory_order_relaxed);
    handle_close(fileIdx);
}

void SampleStreamPlayer_FileSeek(uint32_t fileIdx, uint32_t pos)
{
    note_caller(g_port_seeks);
    Handle *hd = handle_acquire(fileIdx);
    if (!hd) return;
    s_lastHandle.store(fileIdx, std::memory_order_relaxed);
    if (hd->pos != pos)   /* else the last read ended right here */
    {
        /* Emptied first, the seek is a plain lseek(), as in fileSeekTo(). */
        fflush(hd->f);
        fseek(hd->f, pos, SEEK_SET);
        hd->pos = pos;
    }
    handle_release(hd);
}

uint32_t SampleStreamPlayer_FileRead(uint32_t fileIdx, uint8_t *buffer, uint32_t len)
{
    note_caller(g_port_reads);
    Handle *hd = buffer ? handle_acquire(fileIdx) : nullptr;
    if (!hd) return 0;
    s_lastHandle.store(fileIdx, std::memory_order_relaxed);
    const uint32_t got = read_from(hd->f, buffer, len);
    hd->pos += got;
    handle_release(hd);
    return got;
}

/* The library's recorder writes to the file it opened, seeked or read last. */
uint32_t writeBytes(uint8_t *buffer, uint32_t len)
{
    const uint32_t h = s_lastHandle.load(std::memory_order_relaxed);
    Handle *hd = buffer ? handle_acquire(h) : nullptr;
    if (!hd) return 0;
    const uint32_t n = (uint32_t)fwrite(buffer, 1, len, hd->f);
    hd->pos += n;
    handle_release(hd);
    return n;
}

/* Every library handle still open. After WipeOut() and DeInit() the library
 * holds none; anything left is a descriptor the mount would not get back.
 * The engine teardown only, with the library stopped. */
void sample_player_port_close_library_files(void)
{
    uint32_t left = 0;
    for (uint32_t h = 1; h <= kHandles; h++)
    {
        portENTER_CRITICAL(&s_lock);
        const bool open = s_handles[h - 1].f && !s_handles[h - 1].closing;
        portEXIT_CRITICAL(&s_lock);
        if (open) { handle_close(h); ++left; }
    }
    s_lastHandle.store(0, std::memory_order_relaxed);
    if (left) ESP_LOGW("sample_port", "the library left %" PRIu32 " file(s) open", left);
}

/* The library's memory. MallocSlowMemory is the sample and stream blocks
 * Init() was given the sizes of: from the shared arena (engine_arena.hpp),
 * which the pitched instrument gets while the prebuilt engine is stopped and
 * engine_arena_reset() reclaims wholesale. MallocFastMemory is its small, hot
 * state: internal RAM while kFastInternalReserve stays free beside it (the
 * fopen() floor, BLE and WiFi live there), else PSRAM.
 *
 * Free() is handed a bare pointer; engine_arena_free() takes either kind. The
 * blocks are remembered here with their size and kind -- the tallies are
 * evidence EngineHandoff logs, and a fast block DeInit() left behind is not
 * arena memory, so the reset would not take it back
 * (sample_player_port_forget_blocks()). An unbalanced DeInit shows up as a
 * full table (g_marcel_blocks_lost) rather than as silently wrong numbers. */
struct EngineBlock { void *ptr; uint32_t size; bool fast; };
static constexpr int kEngineBlocks = 128;
static EngineBlock s_engineBlocks[kEngineBlocks];
static constexpr size_t kFastInternalReserve = 48 * 1024;
uint32_t g_marcel_malloc_bytes = 0, g_marcel_malloc_count = 0;
uint32_t g_marcel_fast_bytes = 0, g_marcel_fast_internal_bytes = 0;
uint32_t g_marcel_free_bytes = 0, g_marcel_free_count = 0;
uint32_t g_marcel_free_untracked = 0;   /* freed without a recorded size */
uint32_t g_marcel_blocks_lost = 0;      /* table full: size not remembered */

static void remember_block(void *ptr, uint32_t size, bool fast)
{
    g_marcel_malloc_bytes += size;
    g_marcel_malloc_count++;
    for (auto &b : s_engineBlocks)
    {
        if (b.ptr == NULL) { b = EngineBlock{ptr, size, fast}; return; }
    }
    g_marcel_blocks_lost++;
}

void *SampleStreamPlayer_MallocSlowMemory(uint32_t size)
{
    void *ptr = engine_arena_alloc(size);
    if (!ptr)
    {
        printf("PSRAM allocation failed (%" PRIu32 " bytes for streamPlayer)\n", size);
        return NULL;
    }
    remember_block(ptr, size, false);
    return ptr;
}

void *SampleStreamPlayer_MallocFastMemory(uint32_t size)
{
    void *ptr = NULL;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > size + kFastInternalReserve)
    {
        ptr = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    const bool internal = ptr != NULL;
    if (!ptr) ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr)
    {
        printf("allocation failed (%" PRIu32 " bytes of fast memory for streamPlayer)\n", size);
        return NULL;
    }
    g_marcel_fast_bytes += size;
    if (internal) g_marcel_fast_internal_bytes += size;
    remember_block(ptr, size, true);
    return ptr;
}

void SampleStreamPlayer_Free(void *ptr)
{
    if (!ptr) return;

    bool known = false;
    for (auto &b : s_engineBlocks)
    {
        if (b.ptr == ptr)
        {
            g_marcel_free_bytes += b.size;
            g_marcel_free_count++;
            b = EngineBlock{};
            known = true;
            break;
        }
    }
    if (!known) g_marcel_free_untracked++;

    engine_arena_free(ptr);
}

/* Called by the handoff after engine_arena_reset(): the arena is empty, so
 * every slow block is gone whether DeInit freed it or not; a fast block it
 * left is freed here. */
void sample_player_port_forget_blocks(void)
{
    uint32_t leaked = 0;
    for (auto &b : s_engineBlocks)
    {
        if (b.ptr && b.fast)
        {
            heap_caps_free(b.ptr);
            leaked += b.size;
        }
        b = EngineBlock{};
    }
    if (leaked) ESP_LOGW("sample_port", "DeInit left %" PRIu32 " B of fast memory; freed here", leaked);
}

/* The voice cap, SampleStreamPlayer_SetMaxPolyphony(), in the library's units
 * (channels: a stereo voice takes two): 32 after every Init(), sixteen stereo
 * voices. HIL's SMPL_POLY overrides it (s_polyCap) until SMPL_POLY 0. */
static constexpr uint8_t kMaxPolyphony = 32;
static uint8_t s_polyCap;
static uint8_t s_polyApplied;   /* what SetMaxPolyphony was last given: SMPL_PEAK poly= */

uint8_t sample_player_port_poly(void) { return s_polyApplied; }

static void apply_poly(void)
{
    s_polyApplied = s_polyCap ? s_polyCap : kMaxPolyphony;
    SampleStreamPlayer_SetMaxPolyphony(s_polyApplied);
}

void sample_player_port_set_poly_cap(uint8_t cap)
{
    s_polyCap = cap;
    apply_poly();
}

/* Right after SampleStreamPlayer_Init(), before the engine is published. The
 * cache's idle handles go first: they are descriptors the library needs now,
 * one per sample it streams, out of the mount's 32. */
void sample_player_port_engine_started(void)
{
    sample_player_port_flush_files();
    apply_poly();
}
#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */

#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/ui/sampler_panel.hpp"

/* Last eight events the engine played, for SMPL_LAST over CDC — listening
 * cannot tell "the pad below sounds" from "the wrong note arrived". */
#include <atomic>
#include "esp_timer.h"
struct EngineNoteEvent { uint8_t note; uint8_t wavIdx; uint32_t ms; };
EngineNoteEvent g_engine_last[8];
std::atomic<uint32_t> g_engine_last_count{0};

/* Pad → engine latency: the request stamps the note on hand-off, NoteOnCb
 * fires when the voice starts. Nothing else on the chain measures it.
 * 32-bit throughout -- the voice start that reads them is in the render, and a
 * 64-bit atomic is not lock-free on this core. The stamp is esp_timer's low
 * 32 bits (0 = none; a stamp that lands on 0 is written as 1), and the
 * difference of two is right across the 71-minute wrap. */
std::atomic<uint32_t> g_note_req_us[128];
std::atomic<uint32_t> g_note_lat_last_us{0}, g_note_lat_max_us{0}, g_note_lat_count{0};
std::atomic<uint32_t> g_note_lat_sum_us{0};

/* Which layers of which pad the engine is rendering, one bit per (pad, wav):
 * set when a voice starts, cleared when it ends — the engine reports both.
 * A pad with no bit set has no voice the renderer could be inside, which is
 * what lets the kit loader restructure it without gating the renderer. */
static std::atomic<uint32_t> s_padVoices[16];
/* NoteOn has been issued but the engine has not yet started the voice (the
 * callback fires from the render pass, a block later). Without this a loader
 * that looked in that gap saw an idle pad, rebuilt it under the voice that
 * was just starting, and the tone changed kit mid-way. Cleared by the first
 * NoteOnCb, by any NoteOffCb, and by a wipe. */
static constexpr uint32_t kVoiceStarting = 1u << 31;

bool sample_player_pad_sounding(uint8_t pad)
{
    return pad < 16 && s_padVoices[pad].load(std::memory_order_acquire) != 0;
}

void sample_player_pad_starting(uint8_t pad)
{
    if (pad < 16) s_padVoices[pad].fetch_or(kVoiceStarting, std::memory_order_acq_rel);
}

/* The longest a reader waits for a native streaming pass to end, per unit of
 * its weight (SMPL_PACE ms; 0 = no giving way, the A/B against before). */
std::atomic<uint32_t> g_port_pace_ms{20};

bool sample_player_any_sounding(void)
{
    for (auto &v : s_padVoices) {
        if (v.load(std::memory_order_acquire) != 0) return true;
    }
    /* The voices above are the prebuilt engine's. The native one reports its
     * own, and only those that read the card count: a resident voice needs
     * nothing from it. Without this nothing ever gave way to the native
     * streamer -- a kit load under play read its heads at full speed past it,
     * and every underrun of a live session fell in a kit change. */
    if (!g_port_pace_ms.load(std::memory_order_relaxed)) return false;
    uint8_t streaming = 0;
    native_sampler_voice_meter(nullptr, &streaming, nullptr, nullptr);
    return streaming > 0;
}

/* For every bulk reader of the card that is not the streamer — the kit
 * loader through the port, the waveform loader on its own handle: after a
 * piece, give the bus back for as long as the streamer's last refill pass
 * says it needs it. Nothing sounding, nothing given. Bounded — at 8 ms a
 * piece a kit under heavy play took 17 s to land, and a swap that never
 * lands is its own kind of dropout. */
std::atomic<bool> g_port_loader_hurry{false};
extern std::atomic<uint32_t> g_bg_pass_mark_us;   /* audio_module.cpp */

/* A song streaming from the card mixes every track into one ring ahead of
 * playback (core SongEngine, kSongRingFrames): below 16384 frames, a bulk reader
 * waits, bounded, for the song's worker to top it up. Not the worker itself,
 * which gives way through here too. */
static void yield_to_song(uint32_t weight)
{
    constexpr uint32_t kSongLowFrames = 16384;
    constexpr int64_t kWaitUs = 40000;
    const crosspad::ISongEngine *se = crosspad::getPlatformServices().songEngine;
    if (!se || se->streamAheadFrames() >= kSongLowFrames) return;
    const char *me = pcTaskGetName(NULL);
    if (me && strcmp(me, "song_io") == 0) return;
    const int64_t until = esp_timer_get_time() + kWaitUs * weight;
    while (se->streamAheadFrames() < kSongLowFrames && esp_timer_get_time() < until) vTaskDelay(1);
}

void sample_player_port_yield_to_streamer(uint32_t weight)
{
    constexpr uint32_t kPassBusyUs = 20000;    // a pass this long: the streamer is keeping up
    constexpr uint32_t kPassLateUs = 40000;    // this long: voices are close to running dry
    yield_to_song(weight);
    if (!sample_player_any_sounding()) return;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    /* The streamer is held out for this very change: no pass is coming. */
    if (crosspad::samplePlayerStreamHeld()) return;
#endif
    /* A hit is waiting for the pad this read rebuilds: it plays the moment
     * the read ends, so the read no longer gives the bus away. Late by the
     * raw read instead of the paced one. */
    if (weight == 1 && g_port_loader_hurry.load(std::memory_order_relaxed)) return;
    /* The native streamer refills every voice in one pass, a 16-48 kB read
     * each. A piece started inside it takes the stage, and every piece of the
     * pass after that goes to the card sector by sector (stream_fb), and an
     * open is a directory walk the whole pass waits behind. So the next piece
     * waits for the pass to end, bounded: a pass that never ends must not
     * hold a kit load. 20 ms, measured with the six-block rings, 24 swaps at
     * 20 hits/s: no underrun, against 3 without giving way at all; 100 ms
     * gave no fewer and a swap 10 % longer. The mark is odd while a pass
     * runs. */
    const uint32_t paceMs = g_port_pace_ms.load(std::memory_order_relaxed);
    if (paceMs) {
        const int64_t until = esp_timer_get_time() + (int64_t)paceMs * 1000 * weight;
        while ((g_bg_pass_mark_us.load(std::memory_order_relaxed) & 1u) && esp_timer_get_time() < until) {
            vTaskDelay(1);
        }
    }
    const uint32_t pass = g_bg_pass_last_us.load(std::memory_order_relaxed);
    if (pass > kPassLateUs)      vTaskDelay(pdMS_TO_TICKS(3 * weight));
    else if (pass > kPassBusyUs) vTaskDelay(pdMS_TO_TICKS(1 * weight));
    else                         taskYIELD();
}

void sample_player_pad_forget_voices(uint8_t pad)
{
    if (pad < 16) s_padVoices[pad].store(0, std::memory_order_release);
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
void SampleStreamPlayer_NoteOnCb(uint8_t note, uint8_t wavIdx)
{
    if (note < 16) {
        s_padVoices[note].fetch_or(1u << (wavIdx < 31 ? wavIdx : 30), std::memory_order_acq_rel);
        s_padVoices[note].fetch_and(~kVoiceStarting, std::memory_order_acq_rel);
    }
    const uint32_t n = g_engine_last_count.fetch_add(1);
    const int64_t t = esp_timer_get_time();
    g_engine_last[n % 8] = EngineNoteEvent{note, wavIdx, (uint32_t)(t / 1000)};
    const uint32_t req = g_note_req_us[note & 0x7F].exchange(0);
    if (req) {
        const uint32_t lat = (uint32_t)t - req;
        g_note_lat_last_us = lat;
        if (lat > g_note_lat_max_us.load()) g_note_lat_max_us = lat;
        g_note_lat_sum_us += lat;
        ++g_note_lat_count;
    }
    auto cb = crosspad_sampler::sampler_get_engine_callbacks();
    if (cb.onNoteOn) cb.onNoteOn(note, wavIdx);
    crosspad_sampler::sampler_on_engine_note_on(note, wavIdx);
}

void SampleStreamPlayer_NoteOffCb(uint8_t note, uint8_t wavIdx)
{
    if (note < 16) {
        s_padVoices[note].fetch_and(~(1u << (wavIdx < 31 ? wavIdx : 30)) & ~kVoiceStarting,
                                    std::memory_order_acq_rel);
    }
    auto cb = crosspad_sampler::sampler_get_engine_callbacks();
    if (cb.onNoteOff) cb.onNoteOff(note, wavIdx);
    crosspad_sampler::sampler_on_engine_note_off(note, wavIdx);
}
#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */
