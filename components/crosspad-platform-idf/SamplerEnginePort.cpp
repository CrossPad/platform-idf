// SPDX-License-Identifier: GPL-3.0-or-later
#include "SamplerEnginePort.hpp"
#include "crosspad/dsp/AudioKernels.hpp"
#include "lat_trace.h"

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/WavLayout.hpp"
#include "crosspad/audio/ScopeTap.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/sampletools/SampleFx.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampler/KitTake.hpp"
#include "crosspad/sampler/SamplerEngine.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/ui/sampler_panel.hpp"
#include "engine_arena.hpp"
#include "PitchedKitPort.hpp"

#include "SongEnginePort.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include <sys/stat.h>

#include <atomic>
#include <new>
#include <string>
#include <cstdio>
#include <cstring>

/* main/sample_player_port.cpp. The accounting every diagnostic already reads
 * -- SMPL_LAST's last eight events and the pad-to-sound latency -- belongs to
 * whichever engine is playing, not to the prebuilt one. Without this the
 * native engine's note path is invisible to every verb on the board, and the
 * one claim this design rests on (a resident head makes note-on a pointer
 * assignment) cannot be checked at all. */
struct EngineNoteEvent { uint8_t note; uint8_t wavIdx; uint32_t ms; };
extern EngineNoteEvent g_engine_last[8];
extern std::atomic<uint32_t> g_engine_last_count;
/* 32-bit, all of them: on_voice_start() runs in the render, and a 64-bit
 * atomic is not lock-free on this core (libatomic takes a lock for it). The
 * request stamp is esp_timer's low 32 bits, 0 meaning none; differences of
 * them are right across the wrap. */
extern std::atomic<uint32_t> g_note_req_us[128];
extern std::atomic<uint32_t> g_note_lat_last_us, g_note_lat_max_us, g_note_lat_count;
extern std::atomic<uint32_t> g_note_lat_sum_us;

extern "C" {
/* main/sample_player_port.cpp -- the handle cache, the per-task file slots,
 * the internal-RAM floor and the pacing against the kit loader, all of which
 * this engine wants and none of which it should re-invent. */
uint32_t sample_port_pread(const char *fullPath, uint32_t offset, void *dst, uint32_t bytes);
uint32_t sample_port_size(const char *fullPath);
/* main/audio/audio_task.cpp: the streaming task's stack high-water mark,
 * for SMPLENG -- the audition's header parse runs on it too. */
uint32_t audio_bg_stack_free(void);
/* ...and its handle, for the note-on stall record. */
TaskHandle_t audio_bg_task(void);
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/* main/audio/engine_handoff.cpp */
extern "C" bool marcel_engine_running(void);
extern "C" void marcel_engine_stop(void);
extern "C" bool marcel_engine_start(void);
#endif

/* The take, the pad rebuild, the release and the verbs' reports run on a
 * worker or the CDC task, never per block; this file is -O2 for the node's
 * render. GCC's `cold` builds them for size and changes nothing about what
 * they do -- the image is PSRAM too (CLAUDE.md). */
#define CP_COLD __attribute__((cold))

/* The clock, defined in CrosspadPlatform.cpp. */
crosspad::MetronomeNode& getMetronome();

namespace crosspad {
namespace {

const char* TAG = "NativeSampler";

/* Deliberately out here rather than a member: the source object lives in
 * PSRAM (below), and an atomic read-modify-write on external memory is not
 * something to rely on -- the S3's S32C1I is an internal-RAM instruction. */
std::atomic<uint32_t> s_bytesRead{0};

/* The render's rate, as the mixer prepared the node: what a sample's own
 * rate is compared with (native_sampler_rate_report). */
std::atomic<uint32_t> s_engineRate{0};

/* The take's hand-over and where it is: 0 idle, 1 loading, 2 ready, 3
 * failed. The state is derived from the worker's gate (core LatestWork):
 * busy from a request to the worker's last pass, then the last outcome --
 * stored by hand on two threads it once stuck at 1 for good. */
using TakeWork = LatestWork<TakeSpec>;
TakeWork s_take;

/* Why the last take failed, in words.
 *
 * `state=3` on its own cost three build-and-flash cycles of guessing on this
 * bench, which is exactly the diagnostic gap this engine's own counters exist
 * to avoid. A verb that can say "it failed" and not "at what" is half a verb. */
char s_why[56] = "";

/* Which engine a kit load should land in. On by default in this build: the
 * switch was reachable only over CDC and did not survive a reboot, so nobody
 * holding the instrument could get to it. `SMPL_ENGINE marcel` still turns it
 * off for a side-by-side -- in an image built with the prebuilt engine
 * (CONFIG_CROSSPAD_SAMPLER_MARCEL); without it there is nothing to prefer. */
std::atomic<bool> s_prefer{true};
void why(const char* w) { std::snprintf(s_why, sizeof(s_why), "%s", w); }

/// Whether a sample loaded in @p store reads @p path.
bool store_holds(const SampleStore& store, const char* path);

/* ── Where bytes come from ─────────────────────────────────────────────────
 *
 * A WAV is parsed once per path, when the store loads its head, and what the
 * parse found is kept here: a read only gets a path and a frame number, and
 * it needs the data chunk's offset and the frame size to turn those into a
 * byte range. The table is the store's slot count, because the store never
 * asks about a path it has not described. */
/* The table below has two sides: the loader's (describe() writes an entry,
 * forget() empties it -- the take, the pad rebuild, the editor's fallback in
 * native_sampler_describe(), all under LoaderLock) and the streaming pass's
 * (read() looks one up, on audio_bg, for every refill). Nothing ordered them,
 * so a refill could copy an entry half rewritten -- a data offset from one
 * parse with a frame count from another. A mutex, not a critical section:
 * a lookup is up to 64 path compares, and audio_bg is not the render. Taken
 * for the copy only, never across a card read. Created with the loader's
 * (ensure_reclaim_hook()); before that nothing reads the table. */
SemaphoreHandle_t s_sourceMux = nullptr;

class SourceLock {
public:
    SourceLock() { if (s_sourceMux) xSemaphoreTake(s_sourceMux, portMAX_DELAY); }
    ~SourceLock() { if (s_sourceMux) xSemaphoreGive(s_sourceMux); }
    SourceLock(const SourceLock&) = delete;
    SourceLock& operator=(const SourceLock&) = delete;
};

class IdfSampleSource : public ISampleSource {
    static uint32_t preadAt(void* path, uint32_t offset, void* dst, uint32_t bytes)
    {
        return sample_port_pread(static_cast<const char*>(path), offset, dst, bytes);
    }

public:
    struct Entry;
    /// @p table: kMaxSlots entries, in PSRAM; @p store: the store it describes
    /// samples for, which says which entries are still read.
    constexpr IdfSampleSource(Entry* table, const SampleStore* store) : table_(table), store_(store) {}

    bool describe(const char* path, SampleDesc& out) override
    {
        Entry e;
        if (!parse(path, e)) return false;
        remember(path, e);
        out.frames = e.frames;
        out.sampleRate = e.rate;
        out.channels = e.channels;
        out.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t frames, int16_t* dst) override
    {
        Entry e;
        {
            SourceLock lk;
            const Entry* found = find(path);
            if (!found) return 0;
            e = *found;
        }
        return readEntry(e, first, frames, dst);
    }

    void forget()
    {
        SourceLock lk;
        count_ = 0;
    }

    struct Entry {
        char     path[SampleStore::kMaxPath] = {};
        uint32_t dataOffset = 0;
        uint32_t frames = 0;
        uint32_t rate = 0;
        uint8_t  channels = 0;
    };

    /// Frames of a parsed file, through the port's handle cache.
    static uint32_t readEntry(const Entry& e, uint32_t first, uint32_t frames, int16_t* dst)
    {
        if (frames == 0 || first >= e.frames) return 0;
        if (first + frames > e.frames) frames = e.frames - first;
        const uint32_t frameBytes = e.channels * sizeof(int16_t);
        const uint32_t got = sample_port_pread(e.path, e.dataOffset + first * frameBytes,
                                               dst, frames * frameBytes);
        s_bytesRead.fetch_add(got, std::memory_order_relaxed);
        return got / frameBytes;
    }

    /**
     * @brief A WAV's frames as the converter sees them too.
     *
     * crosspad::WavLayout is the one parser: the kit rate converter accepts
     * exactly the files this engine plays, with the same frames at the same
     * offset (a data chunk stated past the end of the file ends where the
     * file does). Only 16-bit samples, mono or stereo -- a stereo sample
     * played as mono is half a kit's stereo image thrown away. No chunk list:
     * this runs on audio_bg's 4 kB stack for an audition.
     */
public:
    static bool parse(const char* path, Entry& e)
    {
        WavLayout lay;
        if (!lay.parse(&preadAt, const_cast<char*>(path), sample_port_size(path)) || !lay.pcm16()) return false;
        e.channels = static_cast<uint8_t>(lay.channels);
        e.rate = lay.rate;
        e.dataOffset = lay.dataOffset;
        e.frames = lay.frames;
        return true;
    }

private:
    const Entry* find(const char* path) const
    {
        for (uint16_t i = 0; i < count_; ++i) {
            if (std::strcmp(table()[i].path, path) == 0) return &table()[i];
        }
        return nullptr;
    }

    void remember(const char* path, Entry& e)
    {
        /* The name goes in before the lookup, not after.
         *
         * It used to be filled only on the append path, so updating an entry
         * that was already there overwrote it with a struct whose path was
         * still empty -- and the entry stopped being findable. read() then
         * returned 0 for a file whose header had just been parsed
         * successfully, the store counted a read failure, and a whole kit came
         * back as `no_sample_readable`.
         *
         * A kit of sixteen distinct files survived it by accident: the wipe
         * and the re-append happened to leave a valid entry at the moment
         * read() looked. A kit that puts one file on every pad did not, which
         * is how it was found. */
        std::snprintf(e.path, sizeof(e.path), "%s", path);
        SourceLock lk;
        for (uint16_t i = 0; i < count_; ++i) {
            if (std::strcmp(table()[i].path, path) == 0) { table()[i] = e; return; }
        }
        if (count_ < SampleStore::kMaxSlots) {
            table()[count_] = e;        /* whole before it is counted */
            ++count_;
            return;
        }
        /* Full. A kit swapped under a performance has no whole-kit release
         * any more to clear this, so an entry no loaded sample reads -- a
         * header described for a plan or a preview, a sample the last kit
         * gave back -- makes room. The streamer only looks up loaded ones. */
        for (uint16_t i = 0; i < count_; ++i) {
            if (!store_holds(*store_, table()[i].path)) { table()[i] = e; return; }
        }
    }

    Entry* table() const { return table_; }
    Entry* table_;
    const SampleStore* store_;
    uint16_t count_ = 0;
};

/* The table, not the object.
 *
 * `IdfSampleSource` is polymorphic, and a polymorphic object may not carry
 * EXT_RAM_BSS_ATTR: its vtable pointer is a non-zero *static* initialiser, so
 * the compiler puts the object in .data -- and the attribute then puts that
 * .data into `.ext_ram.bss`, which the startup code zeroes. The vptr is wiped
 * before main(), and the first call through the interface reads the vtable at
 * address 0. Measured here as `task=smpl_take cause=28 vaddr=0x00000008` --
 * offset 8 being describe()'s slot. GCC does not warn.
 *
 * So the object stays in internal RAM, small, and only its table -- 11 kB of
 * paths and WAV headers, plain data, no vptr -- goes to PSRAM. */
EXT_RAM_BSS_ATTR IdfSampleSource::Entry s_sourceTable[SampleStore::kMaxSlots];

/* The audition's source: one file, not a row of the kit's table -- a file
 * picked in a browser is not a sample of the kit, and sixty of them browsed
 * past would fill that table. describe() and read() both run on audio_bg (the
 * engine's streaming pass), so the one entry needs no lock. Its entry is in
 * PSRAM for the same reason the kit's table is. */
EXT_RAM_BSS_ATTR IdfSampleSource::Entry s_previewEntry;

class IdfPreviewSource : public ISampleSource {
public:
    bool describe(const char* path, SampleDesc& out) override
    {
        IdfSampleSource::Entry e;
        if (!IdfSampleSource::parse(path, e)) { s_previewEntry.path[0] = '\0'; return false; }
        std::snprintf(e.path, sizeof(e.path), "%s", path);
        s_previewEntry = e;
        out.frames = e.frames;
        out.sampleRate = e.rate;
        out.channels = e.channels;
        out.bits = 16;
        return true;
    }

    uint32_t read(const char* path, uint32_t first, uint32_t frames, int16_t* dst) override
    {
        if (std::strcmp(s_previewEntry.path, path) != 0) return 0;
        return IdfSampleSource::readEntry(s_previewEntry, first, frames, dst);
    }
};

/* ── Where memory comes from ──────────────────────────────────────────────
 *
 * The arena, not the general PSRAM heap. With the prebuilt engine up the
 * arena holds 4.25 MB and the launcher has ~275 kB outside it -- there is no
 * megabyte out there to take, and this engine wants about 1.5. So it takes
 * the same bytes the engine it replaces was holding, and leaves the rest of
 * the arena free for the song rings and a pitched bank. */
void* arena_alloc(size_t bytes, void*) { return engine_arena_try_alloc(bytes); }
void  arena_free(void* p, void*)       { engine_arena_free(p); }

/* The pool's own two tables, which must outlive the arena's resets -- a few
 * kilobytes from the ordinary PSRAM heap. MALLOC_CAP_SPIRAM is explicit
 * because CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL puts anything this small in
 * internal RAM otherwise, and internal RAM is the thing this board runs out
 * of first. */
void* psram_alloc(size_t bytes, void*)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
void  psram_free(void* p, void*) { heap_caps_free(p); }

/* The pool's critical section. A plain busy-wait flag is a priority inversion
 * here: the note thread runs below the render thread on the same core and can
 * be preempted holding the lock, while the streaming thread spins on it from
 * the other core. portENTER_CRITICAL suspends preemption on the holding core
 * for the few instructions the free list needs, which is the same answer
 * sample_player_port.cpp gives for its own handle cache. */
portMUX_TYPE s_poolLock = portMUX_INITIALIZER_UNLOCKED;
void pool_lock(void*)   { portENTER_CRITICAL(&s_poolLock); }
void pool_unlock(void*) { portEXIT_CRITICAL(&s_poolLock); }

/* ── The node ─────────────────────────────────────────────────────────────
 *
 * A thin wrapper so the mixer's channel exists from boot whether or not this
 * engine is current, and so the render time is measured the same way
 * MarcelSampleNode measures it -- around the engine's call alone, which is
 * the number that has to come down. */
class NativeSamplerNode : public IAudioNode {
public:
    /* Its own channel beside the prebuilt engine's "Sampler" when both are
     * built; the only sampler, in that channel's name and slot, when not. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    const char* name() const override { return "Sampler2"; }
#else
    const char* name() const override { return "Sampler"; }
#endif
    bool isGenerator() const override { return true; }

    /* Off until the Sampler's scope asks: enabling is what takes the tap's
     * ring (native_sampler_scope_enable()), so an image that never shows it
     * never pays for it. */
    NativeSamplerNode() { scope.setEnabled(false); }

    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override
    {
        engine.onPrepare(maxFrames, sampleRate);
        scope.setSampleRate(sampleRate);
        s_engineRate.store(sampleRate, std::memory_order_relaxed);
    }

    void onSampleRateChange(uint32_t sampleRate) override
    {
        scope.setSampleRate(sampleRate);
        s_engineRate.store(sampleRate, std::memory_order_relaxed);
    }

    /* The live scope gets what this channel carries, mono, in pieces of
     * kScopeChunk off the stack: no buffer to size, nothing allocated or
     * locked on the render thread (ScopeTap::write is a copy into a ring
     * taken when it was enabled). A stopped engine writes silence, so the
     * trace runs on rather than freezing on the last block. */
    static constexpr uint32_t kScopeChunk = 32;

    void scopeSilence(uint32_t frames)
    {
        float mono[kScopeChunk] = {};
        for (uint32_t done = 0; done < frames; done += kScopeChunk) {
            scope.write(mono, frames - done < kScopeChunk ? frames - done : kScopeChunk);
        }
    }

    void mix(float* out, uint32_t frames) override
    {
        const bool tap = scope.enabled();
        silent = true;
        if (!active.load(std::memory_order_acquire)) {
            if (tap) scopeSilence(frames);
            return;
        }
        /* The cycle counter, not esp_timer: one RSR against ~235 cycles of a
         * SYSTIMER read and its wait, twice a block. */
        const uint32_t c0 = esp_cpu_get_cycle_count();
        engine.mix(out, frames);
        const uint32_t dt = (esp_cpu_get_cycle_count() - c0) / static_cast<uint32_t>(esp_rom_get_cpu_ticks_per_us());
        if (dt > rtMaxUs.load(std::memory_order_relaxed)) {
            rtMaxUs.store(dt, std::memory_order_relaxed);
        }
        if (dt > rtMaxPeakUs.load(std::memory_order_relaxed)) {
            rtMaxPeakUs.store(dt, std::memory_order_relaxed);
        }
        /* No voice rendered: the block is the zeros the mixer cleared it to,
         * and nothing below needs a pass over it -- nor does the mixer's own
         * meter (lastMixSilent()). */
        silent = engine.lastMixSilent();
        if (silent) {
            if (tap) scopeSilence(frames);
            return;
        }
        /* What this engine put on its channel, int16 scale, for SMPL_PEAK --
         * "the engine played nothing" against "the bus carried nothing out".
         * The mixer hands every channel a cleared scratch buffer, so the
         * buffer is this engine's contribution alone. Outside the timed span:
         * rt_max_us stays the engine's own number. One pass for it and the
         * song's meter of the track both sides of it. */
        float l = 0.0f, r = 0.0f;
        crosspad::dsp::peakStereo(out, frames, l, r);
        const int32_t q = static_cast<int32_t>(crosspad::dsp::maxOf(l, r) * 32767.0f);
        if (q > peak.load(std::memory_order_relaxed)) peak.store(q, std::memory_order_relaxed);
        notePeaks(l, r, meter);

        if (tap) {
            float mono[kScopeChunk];
            uint32_t n = 0;
            for (uint32_t i = 0; i < frames; ++i) {
                mono[n++] = (out[i * 2] + out[i * 2 + 1]) * 0.5f;
                if (n == kScopeChunk) { scope.write(mono, n); n = 0; }
            }
            if (n) scope.write(mono, n);
        }
    }

    bool lastMixSilent() const override { return silent; }

    /* A block's loudest sample a side into @p into, kept until read
     * (native_sampler_level_take()): the song's meter of the track it plays. */
    static void notePeaks(float l, float r, std::atomic<float>* into)
    {
        if (l > into[0].load(std::memory_order_relaxed)) into[0].store(l, std::memory_order_relaxed);
        if (r > into[1].load(std::memory_order_relaxed)) into[1].store(r, std::memory_order_relaxed);
    }

    SamplerEngine engine;
    ScopeTap scope;
    std::atomic<float> meter[2] = {};
    std::atomic<bool> active{false};
    std::atomic<uint32_t> rtMaxUs{0};       /* SMPL_ENGINE's, read and cleared there */
    std::atomic<uint32_t> rtMaxPeakUs{0};   /* SMPL_PEAK's, so neither clears the other */
    std::atomic<int32_t>  peak{0};          /* SMPL_PEAK's, read and cleared there */
    bool silent = true;                     /* render thread: the last mix() added nothing */
};

/* The node and the source stay in internal RAM -- the node holds the atomics
 * the render thread touches every block, the source has a vtable (see above).
 * The two big tables do not: a path and a WAV header each, read by the loader
 * and the streaming thread and never by the render, and between them 46 kB of
 * the ~90 kB of internal RAM this board has left with the Sampler up. That is
 * the cliff CLAUDE.md records, and this is the same answer
 * sample_player_port.cpp gives for its own handle cache. */
NativeSamplerNode   s_node;
EXT_RAM_BSS_ATTR SampleStore     s_store;
IdfSampleSource     s_source(s_sourceTable, &s_store);
IdfPreviewSource    s_previewSource;
BlockPool           s_pool;
SamplerKit          s_kit;
bool                s_poolReady = false;
uint32_t            s_headBytes = 0;

bool store_holds(const SampleStore& store, const char* path)
{
    for (uint16_t i = 0; i < SampleStore::kMaxSlots; ++i) {
        const SampleStore::Slot* sl = store.slot(i);
        if (sl && std::strcmp(sl->path, path) == 0) return true;
    }
    return false;
}

/* 4 kB blocks out of 64 kB pages. Sixteen voices at eight blocks is 512 kB,
 * and the ceiling is set so a kit's heads and every voice's ring together
 * stay well inside the arena the prebuilt engine used to hold whole. */
/* 16 kB, not 4.
 *
 * The wall on streaming polyphony is the overhead of a read, not the card's
 * throughput: at sixteen voices a 4 kB read averaged 1784 us and peaked at
 * 12704 (SMPL_LAST's lat avg/max), which is 2.2 MB/s from a 4-bit card at
 * 40 MHz. One streamer task issuing those in series could not keep sixteen
 * voices fed -- three of them sounded and thirteen underran.
 *
 * Four times the block with a quarter of the ring (StreamWindow's
 * kDefaultBlocks) is the same 64 kB a voice and the same ~360 ms of
 * lookahead, for a quarter of the reads. */
constexpr size_t kBlockBytes = 16384;
/* Six blocks a ring, not four: 465 ms of lookahead at 44.1 kHz stereo rather
 * than 279. The card is the kit loader's and the UI's too, and right as a kit
 * lands under play the streamer's opens waited 180-200 ms behind theirs --
 * with four blocks 10 underruns in 18 swaps at 20 hits/s, with six 1 in 30.
 * A page is one ring, so the ring stays one span (one read refills
 * several blocks); sixteen of them keep the 1.5 MB ceiling. */
constexpr uint8_t  kStreamBlocks = 6;
constexpr size_t kPageBytes  = kStreamBlocks * kBlockBytes;
constexpr uint32_t kMaxPages = 16;            // 1.5 MB of streaming at most
/* The audition's ring (native_sampler_preview()): four blocks of a page of
 * the arena, reserved with every kit so a browser's audition never takes a
 * ring a pad's voice was counted on. */
constexpr uint32_t kPreviewBlocks = StreamWindow::kDefaultBlocks;
/* What a borrower leaves the owner beyond the pool's ceiling: heads for a pad
 * edit that adds layers -- two, at the size this kit's take gave each (the
 * most a take asks for, before the first take). */
constexpr size_t kLendKeepHead = HeadPlan{}.headBytes;
constexpr uint32_t kLendKeepHeads = 2;

/* The pool follows the voices (pool_follow_voices()): a kit that streams
 * reserves the audition's ring and this many voice rings, and the streaming
 * pass adds one for every voice that waits -- a voice waits while its head
 * plays, ~270 ms at 64 kB -- up to maxStreamingVoices, and gives the pages
 * back after kPoolQuietMs with nothing streaming. It used to reserve every
 * ring the budget allowed at the take: 1 MB for a kit that played none. */
constexpr uint8_t  kBaseRings   = 1;
constexpr uint32_t kPoolQuietMs = 4000;

uint32_t pool_blocks_for(uint8_t rings) { return static_cast<uint32_t>(rings) * kStreamBlocks + kPreviewBlocks; }

/// Slots whose sample did not fit its head -- the ones that will stream.
uint16_t ss_slots_incomplete();

CP_COLD bool ensure_pool()
{
    if (s_poolReady) return true;
    BlockPool::Config c;
    c.blockBytes = kBlockBytes;
    c.pageBytes  = kPageBytes;
    c.maxPages   = kMaxPages;
    c.alloc = &arena_alloc;
    c.free  = &arena_free;
    c.tableAlloc = &psram_alloc;
    c.tableFree  = &psram_free;
    c.lock   = &pool_lock;
    c.unlock = &pool_unlock;
    if (!s_pool.configure(c)) return false;
    s_poolReady = true;
    return true;
}

/* Which file a pad's layer actually plays, and what the engine still has to do
 * to it.
 *
 * `pitch` this engine does itself, as a phase increment. `hp`, `lp` and
 * `reverse` it does not: those are filters and a direction, and the firmware
 * already renders a copy of the sample with them applied
 * (crosspad::renderedSamplePath, written by the kit loader). Reaching for the
 * original file instead would silently drop them -- a pad with a filter would
 * play unfiltered, a reversed pad forwards -- so when a copy is called for and
 * is there, that copy is what plays.
 *
 * The copy has the pitch baked in too, because it is keyed on the whole
 * SampleFx. So a layer that uses one gets `semitones = 0`: applying the step
 * again would transpose it twice. Realtime pitch is therefore what a pad gets
 * whenever it has no filter and no reverse, which is every pad the kit JSON
 * does not say otherwise about -- the serialiser writes hp, lp and reverse
 * only when they are set. */
struct LayerFile {
    std::string path;
    uint32_t    start = 0;
    uint32_t    end   = 0;      ///< 0 = to the end
    bool        pitchInFile = false;
};

CP_COLD static LayerFile layer_file(IdfSampleSource& src, const char* kitPath, const TakePad& pad,
                                    const TakeLayer& layer)
{
    LayerFile f;
    const std::string logical = takeLayerLogical(kitPath, layer);
    f.path  = kitFsPath(logical);
    f.start = layer.start;
    f.end   = layer.end;

    /* Pitch alone is this engine's job, so no copy is wanted for it. */
    const std::string copyLogical = takeLayerCopy(pad, logical);
    if (copyLogical.empty()) return f;

    const std::string copy = kitFsPath(copyLogical);
    struct stat st;
    if (stat(copy.c_str(), &st) != 0) {
        /* The loader has not rendered it (yet). The original is the only thing
         * there is; the filter is missing either way, and a silent pad would
         * be worse than an unfiltered one. */
        return f;
    }

    if (pad.reverse) {
        /* The copy runs backwards, so the trim's end is where it starts. */
        SampleDesc d;
        if (!src.describe(f.path.c_str(), d) || d.frames == 0) return f;
        const uint32_t n = d.frames;
        const uint32_t e = (f.end && f.end < n) ? f.end : n;
        const uint32_t b = f.start < e ? f.start : e;
        f.start = n - e;
        f.end   = b ? n - b : 0;
    }
    f.path = copy;
    f.pitchInFile = true;
    return f;
}

/* One pad of a kit, from a copy of the editor's model (a TakeSpec or a
 * TakePadSpec, made by whoever asked -- never the KitInfo itself, which the UI
 * frees when a newer kit retires it), into @p kp. Split out because a pad edit
 * has to be able to do exactly this and nothing else: turning the pitch knob
 * is the whole point of a realtime step, and it would be a poor joke if
 * changing it handed the kit back to the engine that cannot do it. Into a pad
 * of the caller's rather than the published kit's, so a swap can read every
 * sample in first and point the pad at them in one step.
 *
 * @return layers that found their file.
 */
CP_COLD static uint16_t build_pad(IdfSampleSource& src, SampleStore& store, const char* kitPath,
                                  const TakePad& tp, const TakeLayer* layers, uint8_t p, uint32_t headBytes,
                                  KitPad& kp)
{
    kp = KitPad{};
    kp.mode  = static_cast<PadMode>(tp.mode <= 2 ? tp.mode : 0);
    kp.choke = static_cast<uint8_t>(tp.choke & 0x07);
    kp.gain  = SamplerKit::volumeToGain(tp.volume);
    /* The kit stores pan as a signed offset from centre; the engine wants
     * -1..+1, and the 0..127 MIDI convention is only the prebuilt API's. */
    kp.pan   = SamplerKit::panToBalance(tp.pan + 64);
    /* What this engine exists for: a step, not a copy of the file on the card. */
    kp.semitones = tp.pitch;
    kp.roundRobin = tp.cycle;
    if (tp.attack) kp.attackMs = static_cast<float>(tp.attack);

    uint16_t loaded = 0;
    for (uint8_t i = 0; i < tp.layerCount && i < KitPad::kMaxLayers; ++i) {
        const LayerFile lf = layer_file(src, kitPath, tp, layers[tp.firstLayer + i]);
        SampleDesc d;
        if (!src.describe(lf.path.c_str(), d)) continue;
        const uint32_t frameBytes = d.channels * sizeof(int16_t);
        uint32_t headFrames = headBytes / frameBytes;
        /* From the trim's start: a head that began at the file's first frame
         * left a START past it with nothing to play. */
        uint16_t slot = store.load(lf.path.c_str(), headFrames, lf.start, lf.end);
        /* The header read, so what failed is the head: the arena is short of
         * it. During a swap the outgoing kit's samples are still in there, and
         * a shorter head -- streamed sooner -- beats a pad without its sample.
         * Halved to HeadPlan's floor, as the plan itself would be. */
        const uint32_t floorFrames = HeadPlan{}.headBytesMin / frameBytes;
        while (slot == SampleStore::kNoSlot && headFrames / 2 >= floorFrames) {
            headFrames /= 2;
            slot = store.load(lf.path.c_str(), headFrames, lf.start, lf.end);
        }
        if (slot == SampleStore::kNoSlot) {
            /* Said, not skipped quietly: a pad with a layer missing sounds
             * like a wrong kit, and the arena running short of one block for
             * a head (a borrower, or fragmentation) is otherwise invisible. */
            const auto ss = store.stats();
            ESP_LOGW(TAG, "pad %u layer %u: no head for %s (%u kB asked; slots %u/%u, arena "
                          "%u kB free, largest %u kB)",
                     (unsigned)p, (unsigned)i, lf.path.c_str(),
                     (unsigned)(headFrames * frameBytes / 1024),
                     (unsigned)ss.slots, (unsigned)SampleStore::kMaxSlots,
                     (unsigned)((engine_arena_capacity() - engine_arena_used()) / 1024),
                     (unsigned)(engine_arena_largest_free() / 1024));
            continue;
        }
        kp.layers[kp.layerCount].slot  = slot;
        kp.layers[kp.layerCount].start = lf.start;
        kp.layers[kp.layerCount].end   = lf.end;
        ++kp.layerCount;
        ++loaded;
        /* The copy already carries the step; stepping again would double it. */
        if (lf.pitchInFile) kp.semitones = 0;
    }
    return loaded;
}

uint16_t ss_slots_incomplete()
{
    const auto st = s_store.stats();
    return static_cast<uint16_t>(st.slots - st.completeSlots);
}

/* How big a head each sample of a kit gets, from its copy.
 *
 * HeadPlan's 64 kB: 0.37 s of stereo before the streamer has to deliver,
 * and every sample shorter than that -- most drum hits -- resident whole.
 * The rest streams. PSRAM is what the board runs out of, and the engine now
 * takes it from the general heap as it needs it (engine_arena.cpp), so a
 * kit holds no more of it than that.
 *
 * It was 256 kB a sample (0.74 s, 1.2-1.5 MB a kit): every resident byte is
 * one the streamer does not fetch while the pads are played, and card
 * traffic is what was left in the render (rt_max 1464 us at eleven voices
 * with 3.4 MB/s off the card). Traded back for the memory on 2026-10-18;
 * SMPL_PEAK's rt_busy_max_us and SMPL_ENGINE underrun= are where it shows.
 *
 * Reads every sample's header: a worker's job. */
uint32_t plan_heads(IdfSampleSource& src, const char* kitPath, const TakePad* pads, const TakeLayer* layers,
                    uint32_t budgetBytes)
{
    HeadPlan plan;
    plan.budgetBytes = budgetBytes;

    uint32_t sizes[SampleStore::kMaxSlots];
    uint16_t n = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads && n < SampleStore::kMaxSlots; ++p) {
        const TakePad& tp = pads[p];
        for (uint8_t i = 0; i < tp.layerCount && n < SampleStore::kMaxSlots; ++i) {
            SampleDesc d;
            const LayerFile lf = layer_file(src, kitPath, tp, layers[tp.firstLayer + i]);
            if (!src.describe(lf.path.c_str(), d)) continue;
            /* A head begins at the layer's start, so what it can hold is the
             * file from there on. */
            const uint32_t from = SampleStore::trimStart(lf.start, lf.end, d.frames);
            sizes[n++] = (d.frames - from) * d.channels * sizeof(int16_t);
        }
    }
    return plan.resolve(sizes, n);
}

/// The kit's own cap on voices (TakeSpec::voiceCap: a drum kit's 12), 0 none.
/// Only the total: how many voices stream is the pool's business.
std::atomic<uint8_t> s_kitVoiceCap{0};

/* The heads' share of the arena: everything past the pool's ceiling and a
 * margin the song rings and a waveform read can still borrow. */
uint32_t heads_budget()
{
    const size_t cap = engine_arena_capacity();
    const size_t margin = 512u * 1024;
    const size_t reserved = static_cast<size_t>(kMaxPages) * kPageBytes + margin;
    return cap > reserved ? static_cast<uint32_t>(cap - reserved) : 1024u * 1024;
}

/// What the kit plays with: the engine's defaults, under the kit's own cap.
SamplerEngine::Params kit_params()
{
    SamplerEngine::Params p;
    p.streamBlocks = kStreamBlocks;
    const uint8_t cap = s_kitVoiceCap.load(std::memory_order_acquire);
    if (cap && cap < p.maxVoices) p.maxVoices = cap;
    return p;
}

} // namespace
} // namespace crosspad

using namespace crosspad;

static void on_voice_start(uint8_t pad, uint8_t layer, void*);
static void on_voice_end(uint8_t pad, uint8_t layer, void*);
static bool bar_clock(void*, uint32_t& framesToBar);

/* One lock over every loader-side change to the store, the pool and the kit.
 *
 * There are two worker tasks here -- `smpl_take` (a whole kit) and `smpl_pad`
 * (one pad, from the editor) -- and nothing serialised them. `kit_churn`
 * found the consequence on round one: `smpl_pad` was inside
 * `s_store.load()`, allocating a head out of the arena, while `smpl_take` ran
 * `release()` and then `engine_arena_reset()` under it. The head that came
 * back pointed into memory the heap had just re-registered, and the next
 * `unload()` handing it over asserted inside multi_heap_free -- a panic on
 * `smpl_take` with an eleven-deep backtrace that said nothing about the race.
 *
 * **Recursive**, deliberately: `take_kit_now()` holds it and calls
 * `engine_arena_reset()`, whose reclaim hook is ours and takes it again. The
 * other direction -- another task's reset waiting for a take to finish -- is
 * a wait, not a cycle.
 *
 * Both holders are priority-4 workers and the render never comes near this,
 * so a mutex is right where the pool needed a critical section. */
/* What the engine was last given. The cap is changed at runtime by
 * SMPL_POLY, and setParams() takes the whole struct. */
static SamplerEngine::Params s_params{};

static SemaphoreHandle_t s_loaderMux = nullptr;

class LoaderLock {
public:
    /// @p quiet: a caller that expects to be turned away during a take (the
    /// UI's describe fallback) and has its own answer for it.
    explicit LoaderLock(uint32_t waitMs = 10000, bool quiet = false)
    {
        if (!s_loaderMux) return;
        held_ = xSemaphoreTakeRecursive(s_loaderMux, pdMS_TO_TICKS(waitMs)) == pdTRUE;
        if (!held_ && !quiet) ESP_LOGE(TAG, "loader lock timed out after %u ms", (unsigned)waitMs);
    }
    ~LoaderLock() { if (held_) xSemaphoreGiveRecursive(s_loaderMux); }
    LoaderLock(const LoaderLock&) = delete;
    LoaderLock& operator=(const LoaderLock&) = delete;
    bool ok() const { return held_ || s_loaderMux == nullptr; }
private:
    bool held_ = false;
};

/* Every bound below is a wait for another thread, never a spin: audio_bg may
 * be the one holding the engine's pass, on this core, below a worker. */
static bool before_us(int64_t until) { return esp_timer_get_time() < until; }

/* The audition's ring is pool blocks, so it has to be back before the pool's
 * pages go -- by release, reset or forget. Its fade is the render's, on the
 * Preview channel: the same 100 ms barrier the voices get, then, if the
 * render never ran it (the USB audio profile parks the mixer), it is ended
 * from here, which is safe exactly because nothing is rendering it -- and
 * only once no streaming pass is in it (ioIdle()), which previewAbandon()
 * would otherwise refuse twenty times over a slow card read. Bounded either
 * way. Loader-side only, like the callers. */
static void preview_stop_wait(SamplerEngineBase& e)
{
    if (!e.previewBusy()) return;
    e.previewStop();
    for (int i = 0; i < 20 && e.previewBusy(); ++i) {
        e.serviceIo();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    if (!e.previewBusy()) return;
    const int64_t until = esp_timer_get_time() + 200 * 1000;
    while (!e.ioIdle() && before_us(until)) vTaskDelay(pdMS_TO_TICKS(1));
    for (int i = 0; i < 20 && e.previewBusy(); ++i) {
        if (e.previewAbandon()) {
            ESP_LOGW(TAG, "audition abandoned: the render never faded it");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/**
 * Every streaming pass out of the engine and every voice ended, before the
 * store's heads or the pool's pages go.
 *
 * After `active` is false and the render barrier is waited out, so nothing
 * renders a voice any more. Then:
 *  1. a streaming pass of our own (serviceIo() true): the Finished voices
 *     give their rings back, and audio_bg is not in the voices while we are;
 *  2. whatever is still sounding is abandoned -- it never ends by itself now
 *     (the render is parked, or this engine is off it) and its window would
 *     stay open on a head about to be freed;
 *  3. the audition, after no pass is in it;
 *  4. no pass in either half when this returns.
 * ~500 ms at most; a bound that runs out is logged, never waited past.
 *
 * @return false when a streaming pass still held the voices at the bound:
 *         it may still be filling a block of the pool, so the caller must not
 *         hand the pool's pages back (forget them; the next arena reset takes
 *         them).
 */
static bool io_quiesce(SamplerEngineBase& e)
{
    const int64_t until = esp_timer_get_time() + 500 * 1000;
    while (!e.serviceIo() && before_us(until)) vTaskDelay(pdMS_TO_TICKS(1));
    const bool stuck = e.anySounding();
    bool done = e.abandonVoices();
    while (!done && before_us(until)) {
        vTaskDelay(pdMS_TO_TICKS(1));
        done = e.abandonVoices();
    }
    if (!done) {
        ESP_LOGE(TAG, "io quiesce: a streaming pass held the voices for 500 ms; going on");
    } else if (stuck) {
        ESP_LOGW(TAG, "voices abandoned: the render never ended them");
    }
    preview_stop_wait(e);
    while (!e.ioIdle() && before_us(until)) vTaskDelay(pdMS_TO_TICKS(1));
    return done && e.ioIdle();
}

static bool io_quiesce() { return io_quiesce(s_node.engine); }

/* The note path's mutation window, counted: a take closes it from its
 * release to the kit published, and the arena reset in the middle of the
 * take runs arena_reclaim(), which closes and opens it again -- without the
 * count that reopened the note path halfway through the take. Under
 * LoaderLock (recursive, so the nested holder is the same task). */
static std::atomic<int> s_notesClosed{0};

static void close_notes()
{
    if (s_notesClosed.fetch_add(1, std::memory_order_acq_rel) == 0) s_node.engine.beginMutation();
}

static void open_notes()
{
    int n = s_notesClosed.load(std::memory_order_acquire);
    while (n > 0 && !s_notesClosed.compare_exchange_weak(n, n - 1, std::memory_order_acq_rel)) { }
    if (n == 1) s_node.engine.endMutation();
}

/* Take the engine off the render with its voices ended: the fades played
 * while the render still mixes this engine (active), then active cleared and
 * the render barrier waited out, then io_quiesce(). Leaves the note path
 * closed (close_notes()); the caller opens it again. */
static bool quiesce_engine()
{
    SamplerEngine& e = s_node.engine;
    close_notes();
    if (s_node.active.load(std::memory_order_acquire)) {
        /* Silence first, then wait out the render pass that may be inside a
         * voice. 10 ms is the barrier every other teardown here uses against
         * the RT thread. */
        e.allNotesOff();
        for (int i = 0; i < 20 && e.anySounding(); ++i) {
            e.serviceIo();
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        s_node.active.store(false, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return io_quiesce();
}

/* The kit the engine holds, by the kit manager's id: SMPLENG's kit=, which a
 * HIL run compares with the kit it settled on. -1 with none. */
static std::atomic<int> s_kitId{-1};

/* ── A kit swapped under a performance ────────────────────────────────────
 *
 * The engine never stops for a kit change. Each pad gets its new samples read
 * in beside the old ones and is then pointed at them in one step; a voice
 * already sounding keeps reading the head it started on and plays to its
 * end. What the outgoing kit no longer plays is **retired**: no pad names
 * it, and it goes back to the arena once nothing reads it either
 * (SamplerEngine::slotInUse()). Everything here is under LoaderLock. */

/// Store slots no pad names any more, each waiting for its last voice.
static uint64_t s_retired = 0;
/// The kit the last take could not read (the kit manager's id); see
/// native_sampler_swap_pad().
static int s_takeFailed = -1;

static bool kit_names(uint16_t slot)
{
    for (const auto& kp : s_kit.pad) {
        for (uint8_t i = 0; i < kp.layerCount; ++i) {
            if (kp.layers[i].slot == slot) return true;
        }
    }
    return false;
}

static void sweep_retired()
{
    uint64_t left = s_retired;
    while (left) {
        const uint16_t i = static_cast<uint16_t>(__builtin_ctzll(left));
        left &= left - 1;
        if (s_node.engine.slotInUse(i)) continue;
        s_store.unloadSlot(i);
        s_retired &= ~(1ull << i);
    }
}

/* Sweep, and once nothing is left waiting, give back the pool's pages if the
 * kit no longer streams anything -- what a whole-kit take does by never
 * reserving them. @return true while some old sample is still sounding. */
static bool settle_retired()
{
    sweep_retired();
    if (s_retired) return true;
    if (ss_slots_incomplete() == 0) {
        /* trim() takes the audition's page with the rest: it comes back. */
        s_pool.trim();
        s_pool.reserve(kPreviewBlocks);
    }
    return false;
}

/// The current kit, when it is one this engine plays.
static KitInfo* current_sampler_kit()
{
    KitInfo* kit = getKitManager() ? getKitManager()->getCurrentKit() : nullptr;
    if (!kit || !kit->parsed || kit->engine == KitEngine::Pitched) return nullptr;
    return kit;
}

/* Pad @p p from its copy @p ps. @p plan, the whole kit's copy, for the first
 * pad of another kit: its heads are sized for it, over all of its files, while
 * the outgoing kit is still what plays -- and from here on it is the kit the
 * engine holds. Under EngineArenaOwner and LoaderLock. */
static void swap_pad(const TakePadSpec& ps, uint8_t p, const TakeSpec* plan)
{
    if (plan) {
        s_headBytes = plan_heads(s_source, plan->kitPath, plan->pads, plan->layers, heads_budget());
        s_kitId.store(plan->kitId, std::memory_order_release);
        /* The incoming kit's voice cap, from its first pad on: no take
         * follows a swap to set it. The total only, so the pool stays. */
        s_kitVoiceCap.store(plan->voiceCap, std::memory_order_release);
        const uint8_t voices = kit_params().maxVoices;
        if (s_params.maxVoices != voices) {
            s_params.maxVoices = voices;
            s_node.engine.setParams(s_params);
        }
    }
    /* Room first: whatever the pads swapped before this one have let go of. */
    sweep_retired();

    KitPad next;
    const uint16_t layers = build_pad(s_source, s_store, ps.kitPath, ps.pad, ps.layers, p, s_headBytes, next);
    const KitPad old = s_kit.pad[p];
    /* The swap itself, one copy. A note that reads the pad halfway through
     * may mix the two and play a sample of either; both are loaded, and no
     * old one goes back while a note is in flight. */
    s_kit.pad[p] = next;

    for (uint8_t i = 0; i < next.layerCount; ++i) {
        if (next.layers[i].slot < SampleStore::kMaxSlots) s_retired &= ~(1ull << next.layers[i].slot);
    }
    for (uint8_t i = 0; i < old.layerCount; ++i) {
        const uint16_t s = old.layers[i].slot;
        if (s < SampleStore::kMaxSlots && !kit_names(s)) s_retired |= 1ull << s;
    }

    /* Rings, as a take reserves them, once anything in the kit streams --
     * an all-resident kit going out may have held none. */
    if (ss_slots_incomplete() > 0) s_pool.reserve(pool_blocks_for(kBaseRings));
    sweep_retired();

    ESP_LOGI(TAG, "pad %u swapped: %u layers, pitch %d, %u old samples still sounding",
             (unsigned)p, (unsigned)layers, (int)next.semitones,
             (unsigned)__builtin_popcountll(s_retired));
}

/* The arena is about to be re-registered by somebody else.
 *
 * Every head this engine holds, and every page its pool holds, points into
 * that block. After the reset those pointers are not the heap's any more, and
 * handing one back is an assert inside multi_heap_free -- which is how this
 * was found: a pitched kit loaded between two sample kits (the pitched branch
 * of take_kit_now() returns early, by design, because that kit has its own
 * engine), and the *next* take's unload() freed stale heads. `kit_churn`
 * caught it on round one; reading the code for weeks had not.
 *
 * So: go silent the way a release does (the fades while the render still
 * plays this engine, then off it, then every pass out and every voice ended),
 * and *forget* rather than release. Called from inside engine_arena_reset(),
 * so nothing here may allocate, free, or reset the arena again.
 *
 * Not the same as native_sampler_release(), which gives the memory back one
 * piece at a time and is right when this engine is the one letting go. */
static void arena_reclaim(void*)
{
    /* Forgetting is always safe -- it frees nothing -- so this proceeds even
     * if the lock could not be taken. The lock is here to order against a
     * `build_pad()` that is mid-`load()`, not to make the forget conditional. */
    LoaderLock lk(5000);
    const auto st0 = s_store.stats();
    const uint16_t slots = st0.slots;
    const uint32_t heads = (uint32_t)st0.headBytes;
    const bool wasActive = s_node.active.load(std::memory_order_acquire);
    /* Legal here: engine_arena_reset() runs its borrowers *before* taking the
     * arena lock, and every caller of it is a worker task, never the render. */
    quiesce_engine();
    s_node.engine.setKit(nullptr);
    s_store.forget();
    s_source.forget();
    s_pool.forgetPages();
    s_headBytes = 0;
    s_retired = 0;
    s_kit = SamplerKit{};
    s_kitId.store(-1, std::memory_order_release);
    open_notes();
    s_take.settle(TakeWork::kIdle);   /* a take in flight reports its own outcome */
    ESP_LOGW(TAG, "arena reclaimed: active=%d slots=%u heads=%u kB", wasActive ? 1 : 0,
             (unsigned)slots, (unsigned)(heads / 1024));
}

static void start_pad_worker();

/// Registered the first time anything here is used, not in a constructor:
/// static constructors do not run in a useful order on this platform.
CP_COLD static void ensure_reclaim_hook()
{
    static bool done = false;
    if (done) return;
    done = true;
    s_loaderMux = xSemaphoreCreateRecursiveMutex();
    if (!s_loaderMux) ESP_LOGE(TAG, "no loader mutex: the workers will race");
    s_sourceMux = xSemaphoreCreateMutex();
    if (!s_sourceMux) ESP_LOGE(TAG, "no source mutex: a refill may read a half-written entry");
    engine_arena_set_reclaim(&arena_reclaim, nullptr);
    ESP_LOGI(TAG, "arena reclaim hook registered");
    start_pad_worker();
}

extern "C" crosspad::IAudioNode* native_sampler_node(void)
{
    ensure_reclaim_hook();
    s_node.engine.setPreviewSource(&s_previewSource);
    return &s_node;
}

extern "C" bool native_sampler_preferred(void)
{
    return s_prefer.load(std::memory_order_acquire);
}

extern "C" void native_sampler_set_preferred(bool on)
{
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    s_prefer.store(on, std::memory_order_release);
#else
    (void)on;   /* the only engine there is */
#endif
}

extern "C" void native_sampler_peak_take(int32_t* peak, uint32_t* rtMaxUs, uint32_t* underruns)
{
    const int32_t p = s_node.peak.exchange(0, std::memory_order_relaxed);
    const uint32_t us = s_node.rtMaxPeakUs.exchange(0, std::memory_order_relaxed);
    if (peak) *peak = p;
    if (rtMaxUs) *rtMaxUs = us;
    if (underruns) *underruns = s_node.engine.counters().underruns;
}

extern "C" uint32_t native_sampler_underruns(void)
{
    return s_node.engine.counters().underruns;
}

extern "C" bool native_sampler_active(void)
{
    return s_node.active.load(std::memory_order_acquire);
}

/* native_sampler_release() less its last step: the note path stays closed,
 * for a take that goes straight on to fill the engine again. */
CP_COLD static void release_engine(void)
{
    LoaderLock lk;   /* against smpl_pad: see LoaderLock */
    /* Two halves, and only the first needs the engine to be current.
     *
     * Silencing voices and waiting out the render pass is pointless when
     * nothing is playing -- but *giving the memory back* is not, and it used
     * to be skipped with it. The whole function returned early on
     * `!active`, so a release that followed a kit load -- which deactivates
     * the engine on its way past -- left every head of the previous kit in
     * the arena. The next take then added its own on top: measured on the
     * board as `slots=28 complete=16 heads_kb=4509` for a sixteen-pad kit,
     * which is nearly the whole arena, and a third kit would not have fitted
     * at all.
     *
     * And the voices are ended either way (quiesce_engine()): with the render
     * parked, or with this engine already off it, they never end by
     * themselves, and unload() below would free the heads under them. */
    const bool idle = quiesce_engine();
    s_node.engine.setKit(nullptr);
    {
        const auto st = s_store.stats();
        ESP_LOGI(TAG, "release: unloading %u slots, %u kB of heads",
                 (unsigned)st.slots, (unsigned)(st.headBytes / 1024));
    }
    s_store.unload();
    s_source.forget();
    if (idle) {
        s_pool.releaseAll();
    } else {
        /* A pass may still be writing into a block: its page stays the
         * arena's until the next reset, never freed under the write. */
        ESP_LOGE(TAG, "release: pool pages forgotten, not freed (a streaming pass never left)");
        s_pool.forgetPages();
    }
    s_retired = 0;
    s_kitId.store(-1, std::memory_order_release);
    /* A release that is the first half of a take leaves the state alone --
     * the take sets it when it knows. */
    s_take.settle(TakeWork::kIdle);   /* a take in flight reports its own outcome */
    ESP_LOGI(TAG, "released");
}

extern "C" CP_COLD void native_sampler_release(void)
{
    LoaderLock lk;
    release_engine();
    open_notes();
}

/* The note path stays closed for the whole of a take -- from the release
 * (which closed it) to the kit published -- and opens on every way out. */
class NotesClosedUntilReturn {
public:
    NotesClosedUntilReturn() = default;
    ~NotesClosedUntilReturn() { open_notes(); }
    NotesClosedUntilReturn(const NotesClosedUntilReturn&) = delete;
    NotesClosedUntilReturn& operator=(const NotesClosedUntilReturn&) = delete;
};

enum class TakeResult : uint8_t { Taken, Failed, Dropped };

CP_COLD static TakeResult take_kit_now(const TakeSpec& spec)
{
    /* The arena's owner for the whole take, before LoaderLock (the lock
     * order, engine_arena.hpp). A pitched bank's handoff and build hold the
     * same lock, so a take and a pitched load never interleave: whichever
     * started first ends first, and the other then checks what is current. */
    EngineArenaOwner owner;
    LoaderLock lk;   /* the whole take, release and arena reset included */
    /* A copy that waited (behind a take, or for this lock) is only worth
     * taking if its kit is still the current one. If not, a newer load is
     * under way (its end asks again) or a pitched bank took over -- and taking
     * a sampler kit now would reset the arena under that bank. Checked here,
     * holding the lock every other loader-side change takes. An id read; the
     * model itself is not touched. */
    IKitManager* km = getKitManager();
    if (km && km->getCurrentKitId() != spec.kitId) {
        ESP_LOGW(TAG, "take of kit %d dropped: kit %d is current now", spec.kitId,
                 km->getCurrentKitId());
        return TakeResult::Dropped;
    }
    /* A pitched bank installed under a sampler kit belongs to whoever put it
     * there and is still playing it -- an app holding the engine
     * (instrument_host: a kit load under it ends here too) -- and the reset
     * below would wipe it under that owner. A kit load that is really
     * leaving the pitched kit unloads the bank on its way (reload_pad_impl),
     * so this never drops a take that was meant to happen. */
    if (crosspad::pitchedBankInArena()) {
        ESP_LOGW(TAG, "take of kit %d dropped: a pitched bank holds the arena", spec.kitId);
        return TakeResult::Dropped;
    }
    ensure_reclaim_hook();
    release_engine();
    NotesClosedUntilReturn closed;

    /* Every take starts from the engine's defaults -- its half of two kits
     * while a song holds a second: a SMPL_POLY raise is for the kit it was
     * made on, and the pool below is sized from these. */
    s_kitVoiceCap.store(spec.voiceCap, std::memory_order_release);
    s_params = kit_params();
    s_node.engine.setParams(s_params);

    /* The arena is the prebuilt engine's until it is stopped, and the song
     * rings borrow its slack, so both are asked to let go before anything is
     * carved out of it. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (marcel_engine_running()) marcel_engine_stop();
#endif
    crosspad::song_engine_release_arena();
    engine_arena_reset();
    /* The reset took every page the pool held with it, whether or not
     * releaseAll() got them one at a time. Said so explicitly: a pool that
     * still thinks it has pages hands out blocks that point into memory the
     * heap has re-registered. */
    s_pool.forgetPages();

    if (!ensure_pool()) {
        ESP_LOGE(TAG, "pool refused");
        why("pool_refused");
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        marcel_engine_start();
#endif
        return TakeResult::Failed;
    }

    s_store.bind(&s_source, &arena_alloc, &arena_free, nullptr);
    s_node.engine.bind(&s_pool, &s_store);
    s_node.engine.setNoteCallbacks(&on_voice_start, &on_voice_end, nullptr);
    s_node.engine.setBarClock(&bar_clock, nullptr);
    s_node.engine.setClock([]() -> uint32_t { return (uint32_t)esp_timer_get_time(); });

    const uint32_t headBytes = plan_heads(s_source, spec.kitPath, spec.pads, spec.layers, heads_budget());

    s_kit = SamplerKit{};
    uint16_t loaded = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
        loaded += build_pad(s_source, s_store, spec.kitPath, spec.pads[p], spec.layers, p, headBytes, s_kit.pad[p]);
    }

    if (loaded == 0) {
        ESP_LOGE(TAG, "no sample of this kit could be read");
        why("no_sample_readable");
        s_store.unload();
        s_pool.releaseAll();
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        marcel_engine_start();
#endif
        return TakeResult::Failed;
    }

    /* Blocks for the voices, now that the store knows which samples actually
     * stream. A kit whose every sample fits in its head -- a kit of short
     * hits, which is most of a drum kit -- reserves nothing at all and never
     * touches the card again. The rest reserve a ring per voice that could be
     * streaming at once, and no more. */
    const uint16_t streaming = static_cast<uint16_t>(ss_slots_incomplete());
    if (streaming > 0) {
        /* A ring belongs to a **voice**, not to a sample.
         *
         * This reserved one ring per streaming *slot*, which is only right if
         * a sample is never played twice at once. Hit one streaming pad four
         * times and that is four voices of the same slot, each at its own
         * place in the file and each needing its own ring. A kit with three
         * streaming samples therefore got three rings, and every voice past
         * the third was refused one: it played its resident head and stopped.
         * What that sounds like is the pad restarting from the top instead of
         * sounding through -- which is how it was reported. Measured on INTRO
         * (17 slots, 14 of them resident whole): norings=21, underrun=21.
         *
         * So: one ring per voice that could be sounding. `reserve()` is still
         * bounded by the pool's own ceiling (kMaxPages, 1.5 MB), and
         * HeadPlan already sets that much of the arena aside before it sizes
         * the heads, so this takes nothing away from residency. */
        /* One ring per voice that may be **streaming**, not per voice.
         *
         * A resident voice reads nothing and needs no ring, so sizing this by
         * maxVoices bought memory nothing would use -- and it is real memory:
         * raising the cap to 28 for a measurement took the pool from 512 kB
         * to 1 MB and the next VU meter the UI built could not allocate,
         * which aborted on the LVGL task. */
        /* ...and since 2026-10-18 not even that up front: the voices that
         * stream are the ones that need a ring, and the streaming pass grows
         * the pool to them as they start (pool_follow_voices()). */
        s_pool.reserve(pool_blocks_for(kBaseRings));
    } else {
        s_pool.reserve(kPreviewBlocks);
    }

    s_headBytes = headBytes;
    s_takeFailed = -1;
    why(spec.layersDropped ? "layers_dropped" : "");
    s_node.engine.setKit(&s_kit);
    s_node.engine.resetCounters();
    s_node.rtMaxUs.store(0, std::memory_order_relaxed);
    s_kitId.store(spec.kitId, std::memory_order_release);
    s_node.active.store(true, std::memory_order_release);

    const auto ps = s_pool.stats();
    const auto ss = s_store.stats();
    ESP_LOGI(TAG, "kit %d '%s': %u samples, head %u kB each, heads %u kB, pool %u kB, arena %u/%u kB%s",
             spec.kitId, spec.name, (unsigned)ss.slots, (unsigned)(headBytes / 1024),
             (unsigned)(ss.headBytes / 1024), (unsigned)(ps.bytesHeld / 1024),
             (unsigned)(engine_arena_used() / 1024), (unsigned)(engine_arena_capacity() / 1024),
             spec.layersDropped ? " (layers past the store's 64 left out)" : "");
    return TakeResult::Taken;
}

/* Copies of the kit model for the two workers, made by whoever asks and handed
 * over through one pointer each: newest wins, the one it replaces is freed by
 * the replacer. PSRAM (~11 kB a kit, ~1.5 kB a pad), plain data, held only
 * from the request to the worker's pass. */
static void* spec_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}


/* The take reads every sample's header and its resident head off the card --
 * seconds of work. It ran on whichever thread asked for it, which was the CDC
 * main task with its 4 kB stack, and took the board down with it. Same answer
 * the kit loader gives: a task of its own, its stack in PSRAM, and the caller
 * gets a state to poll.
 *
 * It serves requests until there are none (WorkerGate): a kit that lands while
 * one is being taken is taken right after, not refused -- the engine used to
 * keep the old kit under the new one's name. */
CP_COLD static void take_kit_task(void*)
{
    while (s_take.next()) {
        TakeSpec* spec = s_take.take();
        if (!spec) continue;                 /* folded into the pass before */
        const TakeResult r = take_kit_now(*spec);
        heap_caps_free(spec);
        s_take.done(r == TakeResult::Taken    ? TakeWork::kReady
                    : r == TakeResult::Failed ? TakeWork::kFailed
                    : native_sampler_active() ? TakeWork::kReady : TakeWork::kIdle);
    }
    vTaskDeleteWithCaps(nullptr);   /* created WithCaps: its stack and TCB go back */
}

extern "C" uint8_t native_sampler_state(void)
{
    return s_take.state();
}

extern "C" CP_COLD bool native_sampler_take_kit(void)
{
    IKitManager* km = getKitManager();
    KitInfo* kit = km ? km->getCurrentKit() : nullptr;
    if (!kit || !kit->parsed || kit->engine == KitEngine::Pitched) return false;

    /* Here, on the asking thread, while the kit is current: the worker never
     * reads the model (the UI frees a retired kit's pads). Strings and
     * numbers only, no card access. */
    void* mem = spec_alloc(sizeof(TakeSpec));
    if (!mem) {
        ESP_LOGE(TAG, "take: no %u B of PSRAM for the kit's copy", (unsigned)sizeof(TakeSpec));
        why("no_mem");
        s_take.settle(TakeWork::kFailed);    /* shown once no take is in flight */
        return false;
    }
    TakeSpec* spec = new (mem) TakeSpec();
    if (!buildTakeSpec(*kit, km->getCurrentKitId(), *spec)) {
        heap_caps_free(spec);
        why("kit_path");                     /* the kit's path does not fit a copy */
        s_take.settle(TakeWork::kFailed);
        return false;
    }
    if (spec->layersDropped) {
        ESP_LOGW(TAG, "take: %u layers of '%s' left out (64 samples at most, or a name too long)",
                 (unsigned)spec->layersDropped, spec->name);
    }

    /* One step from the requester's view: the copy in, the gate posted --
     * the state reads "loading" from the post on, whatever the worker of the
     * previous kit stores meanwhile. */
    TakeSpec* older = nullptr;
    const bool start = s_take.offer(spec, older);
    if (older) heap_caps_free(older);
    if (!start) return true;                 /* the running worker takes it next */

    /* 8 kB stack in PSRAM, for the take only: it reads the card (through the
     * port's DMA stage, never into this stack) and writes no flash or NVS. It
     * was a static 8 kB of internal RAM held for good, on a board whose
     * internal RAM is what BLE and WiFi run out of. Created WithCaps, so it
     * deletes itself WithCaps (take_kit_task). */
    TaskHandle_t task = nullptr;
    if (xTaskCreatePinnedToCoreWithCaps(take_kit_task, "smpl_take", 8192, nullptr, 4, &task, 1,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        why("no_task");
        if (TakeSpec* left = s_take.startFailed()) heap_caps_free(left);
        return false;
    }
    return true;
}

/* Fired from the render pass, when the voice is actually heard -- so the
 * latency this records is pad-to-sound and not pad-to-accepted. RT-safe:
 * 32-bit atomics and one array slot. */
static void on_voice_start(uint8_t pad, uint8_t layer, void*)
{
    const uint32_t n = g_engine_last_count.fetch_add(1);
    const int64_t t = esp_timer_get_time();
    g_engine_last[n % 8] = EngineNoteEvent{pad, layer, (uint32_t)(t / 1000)};
    const uint32_t req = g_note_req_us[pad & 0x7F].exchange(0);
    if (req) {
        const uint32_t lat = (uint32_t)t - req;
        g_note_lat_last_us = lat;
        if (lat > g_note_lat_max_us.load()) g_note_lat_max_us = lat;
        g_note_lat_sum_us += lat;
        ++g_note_lat_count;
    }
    lat_trace_voice_start(pad);
    crosspad::getPadLedController().notifyNoteDimmed(pad, false);
    crosspad::getPadLedController().notifyNotePlay(pad, true);
    crosspad_sampler::sampler_on_engine_note_on(pad, layer);
}

/* The pad stops playing with its last voice: the press set it playing, and
 * left set, every later colour of the pad waited for the next release. */
static void on_voice_end(uint8_t pad, uint8_t layer, void*)
{
    crosspad::getPadLedController().notifyNoteDimmed(pad, false);
    crosspad::getPadLedController().notifyNotePlay(pad, s_node.engine.padSounding(pad));
    crosspad_sampler::sampler_on_engine_note_off(pad, layer);
}

/* What a Loop pad launches on: the metronome, the sequencer's clock in
 * samples. Added to the mixer after the sampler, so in the render its
 * position is still the one at the start of the block the sampler mixes. */
static bool bar_clock(void*, uint32_t& framesToBar)
{
    const crosspad::MetronomeNode& m = getMetronome();
    if (!m.running()) return false;
    framesToBar = m.framesToBar();
    return true;
}

/* A Loop pad muted or waiting for its bar shows dimmed; the launch itself is
 * on_voice_start()'s to light. */
static void loop_led(uint8_t pad, uint8_t key)
{
    const auto st = s_node.engine.loopState(pad, key);
    crosspad::getPadLedController().notifyNoteDimmed(
        pad, st == SamplerEngine::LoopState::Queued || st == SamplerEngine::LoopState::Muted);
}

/* Rebuilding a pad reads headers and a resident head off the card, so it goes
 * on a task of its own -- the same conclusion the whole-kit take reached, and
 * the same one this missed on the first try: called straight from the CDC
 * verb it overflowed the 4 kB main task (`task=main cause=65`). Callers only
 * ever want "this pad, again"; none of them waits for it.
 *
 * Newest wins per pad (one copy each, replaced by a newer request), and a pad
 * asked for twice while the worker runs is rebuilt once.
 *
 * One task for the life of the board, parked on a notification. It used to be
 * created per request and to delete itself at the end, on one static stack --
 * and a request landing between `s_padTask = nullptr` and the delete started
 * a second task on the stack the first was still running on. It is also where
 * the outgoing kit's last voices are waited for after a swap. */
static std::atomic<TakePadSpec*> s_padPending[SamplerKit::kPads] = {};
static TaskHandle_t s_padTask = nullptr;

/* How often a retired sample is looked at again, and for how long. A loop
 * plays until something stops it; past the wait its sample goes back at the
 * next swap or pad edit instead. */
static constexpr uint32_t kRetirePollMs = 50;
static constexpr uint32_t kRetireWaitMs = 20000;

static void pad_worker(void*)
{
    uint32_t waitedMs = 0;
    bool waiting = false;
    for (;;) {
        const TickType_t wait = waiting ? pdMS_TO_TICKS(kRetirePollMs) : portMAX_DELAY;
        if (ulTaskNotifyTake(pdTRUE, wait) != 0) waitedMs = 0;
        else waitedMs += kRetirePollMs;

        for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
            TakePadSpec* ps = s_padPending[p].exchange(nullptr, std::memory_order_acq_rel);
            if (!ps) continue;
            {
                /* The arena's owner first (lock order, engine_arena.hpp): a
                 * head is carved out of the arena, never beside a pitched
                 * build or a reset. */
                EngineArenaOwner owner;
                LoaderLock lk;
                /* Under the lock, because a take may have run between the
                 * request and this pad's turn: the kit this would rebuild into
                 * is then gone (or another), and `build_pad()` would allocate a
                 * head out of an arena that is about to be reset. */
                if (lk.ok() && s_node.active.load(std::memory_order_acquire) &&
                    ps->kitId == s_kitId.load(std::memory_order_acquire)) {
                    swap_pad(*ps, p, nullptr);
                }
            }
            heap_caps_free(ps);
        }

        EngineArenaOwner owner;
        LoaderLock lk;
        waiting = lk.ok() && s_node.active.load(std::memory_order_acquire) &&
                  settle_retired() && waitedMs < kRetireWaitMs;
    }
}

static void start_pad_worker()
{
    /* PSRAM stack, like smpl_take's: card reads and the arena, never flash --
     * and its 6 kB of internal RAM pay for the render's buffers there. */
    if (xTaskCreatePinnedToCoreWithCaps(pad_worker, "smpl_pad", 6144, nullptr, 4, &s_padTask, 1,
                                        MALLOC_CAP_SPIRAM) != pdPASS)
        s_padTask = nullptr;
    if (!s_padTask) ESP_LOGE(TAG, "no pad worker: a pad edit hands the kit to the prebuilt engine");
}

extern "C" CP_COLD bool native_sampler_reload_pad(uint8_t pad)
{
    if (!native_sampler_active() || pad >= KIT_PADS || pad >= SamplerKit::kPads || !s_padTask) return false;
    IKitManager* km = getKitManager();
    KitInfo* kit = current_sampler_kit();
    if (!kit) return false;
    /* Only a pad of the kit this engine holds: another kit's pads come in
     * through native_sampler_swap_pad(), which sizes the heads for it first.
     * False hands the pad back to the caller. */
    if (km->getCurrentKitId() != s_kitId.load(std::memory_order_acquire)) return false;

    /* The copy is made here, while the kit is current: the worker never reads
     * the model (KitTake.hpp). */
    void* mem = spec_alloc(sizeof(TakePadSpec));
    if (!mem) return false;
    TakePadSpec* ps = new (mem) TakePadSpec();
    if (!buildTakePadSpec(*kit, km->getCurrentKitId(), pad, *ps)) {
        heap_caps_free(ps);
        return false;
    }
    if (TakePadSpec* older = s_padPending[pad].exchange(ps, std::memory_order_acq_rel)) {
        heap_caps_free(older);
    }
    xTaskNotifyGive(s_padTask);
    return true;
}

/* A take that ran long, on a 1-bit card and a big kit, is seconds; this only
 * bounds a take that never ends. */
static constexpr uint32_t kTakeWaitMs = 60000;

extern "C" bool native_sampler_swap_pad(uint8_t pad, bool mayTake)
{
    if (pad >= KIT_PADS || pad >= SamplerKit::kPads) return false;
    IKitManager* km = getKitManager();
    KitInfo* want = current_sampler_kit();
    if (!want) return false;
    const int wantId = km->getCurrentKitId();
    if (!native_sampler_active()) {
        /* Meant to hold the kit and not holding it yet: the first kit after
         * boot. It used to go into the prebuilt engine and be taken from it
         * once the load was done -- the kit read twice with the take's silence
         * in between, and a kit change landing during that take went the
         * prebuilt engine's way again (pads held back, forced, 7.8 s). Taken
         * here instead, whole, and the pads then swap into it like any other.
         * A kit that a take could not read is not tried again pad after pad. */
        if (!mayTake || !native_sampler_preferred() || s_takeFailed == wantId) return false;
        if (s_take.state() != TakeWork::kBusy && !native_sampler_take_kit()) return false;
        for (uint32_t waited = 0; s_take.state() == TakeWork::kBusy && waited < kTakeWaitMs;
             waited += 10) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (!native_sampler_active()) {
            s_takeFailed = wantId;
            return false;
        }
    }

    /* Copies, made here on the loader's thread while the kit is current, as a
     * take makes them (KitTake.hpp): the swap never reads the model. The whole
     * kit's too for its first pad, whose heads are sized over all of its files. */
    void* padMem = spec_alloc(sizeof(TakePadSpec));
    TakePadSpec* ps = padMem ? new (padMem) TakePadSpec() : nullptr;
    TakeSpec* plan = nullptr;
    if (s_kitId.load(std::memory_order_acquire) != wantId) {
        void* planMem = spec_alloc(sizeof(TakeSpec));
        plan = planMem ? new (planMem) TakeSpec() : nullptr;
        if (plan && !buildTakeSpec(*want, wantId, *plan)) {
            heap_caps_free(plan);
            plan = nullptr;
        }
    }
    bool mine = true;
    if (!ps || !buildTakePadSpec(*want, wantId, pad, *ps)) {
        /* The pad keeps what it plays. Saying "not mine" would hand it to the
         * prebuilt engine, and with it the whole kit. */
        ESP_LOGE(TAG, "pad %u: no copy of it for the swap", (unsigned)pad);
    } else {
        /* The arena's owner first, then the loader's lock (engine_arena.hpp). */
        EngineArenaOwner owner;
        LoaderLock lk;
        /* Under the lock, again: a take or an arena reclaim may have run
         * while this waited for it. */
        if (!s_node.active.load(std::memory_order_acquire)) {
            mine = false;
        } else if (s_kitId.load(std::memory_order_acquire) != wantId && !plan) {
            ESP_LOGE(TAG, "pad %u: kit %d not planned, left as it is", (unsigned)pad, wantId);
        } else if (lk.ok()) {   /* the lock logs its own timeout */
            swap_pad(*ps, pad, s_kitId.load(std::memory_order_acquire) != wantId ? plan : nullptr);
        }
    }
    if (ps) heap_caps_free(ps);
    if (plan) heap_caps_free(plan);
    if (!mine) return false;
    /* The outgoing kit's last voices are waited for on the worker, not here:
     * the loader has the next pad to read. */
    if (s_retired && s_padTask) xTaskNotifyGive(s_padTask);
    return true;
}

extern "C" bool native_sampler_pad_ready(uint8_t pad)
{
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return false;
    IKitManager* km = getKitManager();
    if (!km || km->getCurrentKitId() != s_kitId.load(std::memory_order_acquire)) return false;
    /* One byte of the pad swap_pad() copies whole: mid-copy it reads as the
     * old pad's count or the new one's, never torn. */
    return s_kit.pad[pad].layerCount > 0;
}

extern "C" bool native_sampler_set_pad_pitch(uint8_t pad, int semitones)
{
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return false;
    if (semitones < -24) semitones = -24;
    if (semitones >  24) semitones =  24;
    /* One number. No file is read, no header re-parsed, no pad rebuilt -- a
     * step is what the voice multiplies its phase increment by, and the next
     * note picks it up. Voices already sounding keep theirs, which is what a
     * knob turned over a ringing note should do.
     *
     * Safe from the LVGL thread: a single int8_t the render thread only reads
     * when it starts a voice. */
    s_kit.pad[pad].semitones = static_cast<int8_t>(semitones);
    return true;
}

/* The slowest note-on since SMPL_STALL ON, and where its time went -- and the
 * slowest pad path, the STM's note to the end of its handling (UART drv).
 *
 * The engine's noteon_max_us is wall time around a body that takes no lock
 * and reads no card, so a 46 ms one is the caller not running. Beside it:
 * the caller's own CPU time over the same span, audio_bg's -- the task above
 * the pad path on core 0 -- and the caller's core's idle time, so one record
 * says whether it was the note itself, the streamer, another task, or
 * nothing at all running (a flash write holding both cores). Off by
 * default: four vTaskGetInfo() calls a span are not for every board. */
struct NoteStall {
    uint32_t us, selfUs, bgUs, idleUs, atMs;
    uint8_t core;
    char task[configMAX_TASK_NAME_LEN];
};
EXT_RAM_BSS_ATTR static NoteStall s_stall[NativeStallClock::kRecords];
static std::atomic<uint32_t> s_stallMaxUs[NativeStallClock::kRecords];
static std::atomic<bool> s_stallOn{false};

static uint32_t task_run_us(TaskHandle_t h)
{
    if (!h) return 0;
    TaskStatus_t st;
    vTaskGetInfo(h, &st, pdFALSE, eInvalid);
    return static_cast<uint32_t>(st.ulRunTimeCounter);
}

NativeStallClock::NativeStallClock(uint8_t record)
    : record_(record < kRecords ? record : 0), on_(s_stallOn.load(std::memory_order_relaxed))
{
    if (!on_) return;
    me_ = xTaskGetCurrentTaskHandle();
    core_ = static_cast<uint8_t>(xPortGetCoreID());
    bg_ = audio_bg_task();
    self0_ = task_run_us(static_cast<TaskHandle_t>(me_));
    bg0_ = task_run_us(static_cast<TaskHandle_t>(bg_));
    idle0_ = static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(core_));
    t0_ = esp_timer_get_time();
}

NativeStallClock::~NativeStallClock()
{
    if (!on_) return;
    const uint32_t dt = static_cast<uint32_t>(esp_timer_get_time() - t0_);
    if (dt <= s_stallMaxUs[record_].load(std::memory_order_relaxed)) return;
    s_stallMaxUs[record_].store(dt, std::memory_order_relaxed);
    NoteStall& r = s_stall[record_];
    r.us = dt;
    r.selfUs = task_run_us(static_cast<TaskHandle_t>(me_)) - self0_;
    r.bgUs = task_run_us(static_cast<TaskHandle_t>(bg_)) - bg0_;
    r.idleUs = static_cast<uint32_t>(ulTaskGetIdleRunTimeCounterForCore(core_)) - idle0_;
    r.atMs = static_cast<uint32_t>(t0_ / 1000);
    r.core = core_;
    std::snprintf(r.task, sizeof(r.task), "%s", pcTaskGetName(static_cast<TaskHandle_t>(me_)));
}

extern "C" void native_sampler_note_on(uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    if (!native_sampler_active()) return;
    NativeStallClock clock(NativeStallClock::kNote);
    s_node.engine.noteOn(pad, velocity, transpose, key);
    loop_led(pad, key);
}

extern "C" bool native_sampler_arm(uint8_t touch, uint8_t pad, int8_t transpose, uint8_t key)
{
    if (!native_sampler_active()) return false;
    if (!s_node.engine.arm(touch, pad, transpose, key)) return false;
    lat_trace_note_arm(touch);
    return true;
}

extern "C" void native_sampler_play(uint8_t touch, uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    if (!native_sampler_active()) return;
    NativeStallClock clock(NativeStallClock::kNote);
    if (!s_node.engine.fire(touch, pad, velocity, transpose, key)) {
        s_node.engine.noteOn(pad, velocity, transpose, key);
    }
    loop_led(pad, key);
    lat_trace_note_fire(pad);
}

extern "C" void native_sampler_disarm(uint8_t touch)
{
    s_node.engine.disarm(touch);
}

extern "C" void native_sampler_note_off(uint8_t pad, uint8_t key)
{
    if (!native_sampler_active()) return;
    s_node.engine.noteOff(pad, key);
}

extern "C" void native_sampler_all_notes_off(void)
{
    if (!native_sampler_active()) return;
    s_node.engine.allNotesOff();
}

extern "C" bool native_sampler_pad_sounding(uint8_t pad)
{
    return native_sampler_active() && s_node.engine.padSounding(pad);
}

/* The kit holds what a new voice starts with; the engine is told so the
 * voices already sounding move too -- a fader turned over a playing loop has
 * to be heard on that loop, not on the next hit. */
static void push_pad_mix(uint8_t pad)
{
    const KitPad& kp = s_kit.pad[pad];
    s_node.engine.setPadMixGains(pad, kp.gain, kp.pan);
}

extern "C" void native_sampler_set_pad_volume(uint8_t pad, int volume)
{
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return;
    s_kit.pad[pad].gain = SamplerKit::volumeToGain(volume);
    push_pad_mix(pad);
}

extern "C" void native_sampler_set_pad_pan(uint8_t pad, int pan)
{
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return;
    /* The kit stores pan as a signed offset from centre. */
    s_kit.pad[pad].pan = SamplerKit::panToBalance(pan + 64);
    push_pad_mix(pad);
}

extern "C" void native_sampler_set_pad_metering(bool on)
{
    s_node.engine.padMeters().setEnabled(on);
}

extern "C" float native_sampler_take_pad_peak(uint8_t pad)
{
    return s_node.engine.padMeters().take(pad);
}

/* ── What the pad editor needs from whichever engine holds the kit ─────────
 *
 * These five exist because making this engine the default silenced parts of
 * the editor that went through the prebuilt one and had no other door. The
 * worst was the free-slot count: it came from `marcel_engine_free_slots()`,
 * which answers -1 while that engine is stopped, and the editor refuses to
 * add a sample to a pad when the count is not positive -- so on this engine
 * a pad could not be given a sample at all. */

extern "C" void native_sampler_stop_pad(uint8_t pad)
{
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return;
    s_node.engine.stopPad(pad);
}

extern "C" bool native_sampler_sync_pad_mode(uint8_t pad)
{
    if (!native_sampler_active() || pad >= SamplerKit::kPads) return false;
    IKitManager* km = crosspad::getKitManager();
    const KitInfo* kit = km ? km->getCurrentKit() : nullptr;
    if (!kit || !kit->parsed || pad >= KIT_PADS) return false;
    /* Only into the kit it came from: during a load the current kit is the
     * next one while the engine still holds the last. */
    if (km->getCurrentKitId() != s_kitId.load(std::memory_order_acquire)) return false;
    const uint8_t m = kit->pads[pad].playbackMode;
    /* One byte the render thread reads when it starts a voice, like the step.
     * A voice already looping keeps looping until it is stopped: the editor's
     * own button is next to a pad the player can hit again, and retuning or
     * unlooping a ringing note was never what turning it meant. */
    s_kit.pad[pad].mode = static_cast<PadMode>(m <= 2 ? m : 0);
    return true;
}

extern "C" int native_sampler_free_slots(void)
{
    if (!native_sampler_active()) return -1;
    const int used = static_cast<int>(s_store.stats().slots);
    const int free = static_cast<int>(SampleStore::kMaxSlots) - used;
    return free > 0 ? free : 0;
}

extern "C" CP_COLD bool native_sampler_describe(const char* path, uint32_t* outFrames,
                                        uint8_t* outChannels, uint32_t* outRate)
{
    if (!native_sampler_active() || !path || !path[0]) return false;

    /* The store already read this header when it loaded the sample, so answer
     * from it. The panel asks for every pad as it builds, and going to the
     * card each time put the Sampler's open past the three seconds `app_churn`
     * allows -- on a card running in 1-bit, sixteen header reads are seconds.
     * The path the caller passes is the one build_pad() loaded, so this hits
     * for every sample of the current kit; only a file outside the kit (the
     * browser's preview) falls through to the card. */
    for (uint16_t i = 0; i < SampleStore::kMaxSlots; ++i) {
        const SampleStore::Slot* sl = s_store.slot(i);
        if (!sl || std::strcmp(sl->path, path) != 0) continue;
        if (sl->desc.frames == 0) break;
        if (outFrames)   *outFrames   = sl->desc.frames;
        if (outChannels) *outChannels = sl->desc.channels;
        if (outRate)     *outRate     = sl->desc.sampleRate;
        return true;
    }

    /* The card, through the kit's source table -- which the take and the pad
     * rebuild write to under LoaderLock, with the same shared file slot.
     * This is the LVGL thread: 30 ms for the lock, and during a take (the
     * lock held for seconds) no answer rather than a frozen UI. */
    LoaderLock lk(30, true);
    if (!lk.ok()) return false;
    SampleDesc d;
    if (!s_source.describe(path, d) || d.frames == 0) return false;
    if (outFrames)   *outFrames   = d.frames;
    if (outChannels) *outChannels = d.channels;
    if (outRate)     *outRate     = d.sampleRate;
    return true;
}

extern "C" CP_COLD bool native_sampler_rate_report(uint16_t* samples, uint16_t* offRate, uint32_t* rate,
                                                   uint32_t* engineRate)
{
    if (!native_sampler_active() || native_sampler_state() == 1) return false;
    const uint32_t er = s_engineRate.load(std::memory_order_relaxed);
    if (!er) return false;
    LoaderLock lk(30, true);
    if (!lk.ok()) return false;

    /* Distinct files: a slot is a file and a trim start, and a sliced loop is
     * one file on sixteen pads. */
    const SampleStore::Slot* seen[SampleStore::kMaxSlots];
    uint16_t n = 0, off = 0;
    for (const KitPad& kp : s_kit.pad) {
        for (uint8_t l = 0; l < kp.layerCount; ++l) {
            const SampleStore::Slot* sl = s_store.slot(kp.layers[l].slot);
            if (!sl || sl->desc.sampleRate == 0) continue;
            bool dup = false;
            for (uint16_t i = 0; i < n && !dup; ++i) dup = std::strcmp(seen[i]->path, sl->path) == 0;
            if (dup || n >= SampleStore::kMaxSlots) continue;
            seen[n++] = sl;
            if (sl->desc.sampleRate != er) ++off;
        }
    }
    uint32_t best = 0, bestCount = 0;
    for (uint16_t i = 0; i < n; ++i) {
        const uint32_t r = seen[i]->desc.sampleRate;
        if (r == er) continue;
        uint32_t c = 0;
        for (uint16_t j = 0; j < n; ++j) c += seen[j]->desc.sampleRate == r;
        if (c > bestCount) { best = r; bestCount = c; }
    }
    if (samples)    *samples = n;
    if (offRate)    *offRate = off;
    if (rate)       *rate = best;
    if (engineRate) *engineRate = er;
    return true;
}

extern "C" CP_COLD bool native_sampler_reads(const char* fsPath)
{
    LoaderLock lk(30, true);
    if (!lk.ok()) return true;
    return store_holds(s_store, fsPath);
}

extern "C" CP_COLD uint8_t native_sampler_set_max_voices(uint8_t n)
{
    if (!native_sampler_active()) return 0;
    /* 0: back to the engine's default, as SMPL_POLY 0 has always meant. */
    if (n == 0) n = kit_params().maxVoices;
    if (n > SamplerEngine::kMaxSounding) n = SamplerEngine::kMaxSounding;
    SamplerEngine::Params p = s_params;
    p.maxVoices = n;
    s_params = p;
    s_node.engine.setParams(p);
    /* The ring pool was sized for the cap that was in force when the kit was
     * taken; a higher cap needs more rings or the extra voices play their
     * resident head and stop. */
    /* The total cap does not change how many rings are needed -- that is the
     * streaming budget's business -- so the pool is left alone here. */
    ESP_LOGI(TAG, "voice cap -> %u", (unsigned)n);
    return n;
}

/// How long a CDC verb tries for a lock another worker may hold for seconds.
constexpr uint32_t kCdcTryMs = 20;

extern "C" CP_COLD uint8_t native_sampler_set_max_streaming(uint8_t n)
{
    /* reserve() takes pages out of the arena: not beside a take, a pad
     * rebuild or a pitched build doing the same, nor across a reset -- so the
     * arena's owner lock first, then the loader's (the lock order). A CDC
     * verb, and the CDC task waits for nobody: a short try of each, and
     * "busy" rather than a hang (it was a 2 s wait behind a take) -- or a 0
     * that reads as a cap. The host retries. */
    if (!engine_arena_owner_lock(kCdcTryMs)) return NATIVE_SAMPLER_BUSY;
    struct OwnerRelease { ~OwnerRelease() { engine_arena_owner_unlock(); } } ownerRelease;
    LoaderLock lk(kCdcTryMs, true);
    if (!lk.ok()) return NATIVE_SAMPLER_BUSY;
    if (!native_sampler_active()) return 0;
    if (n == 0) n = kit_params().maxStreamingVoices;
    if (n > SamplerEngine::kMaxSounding) n = SamplerEngine::kMaxSounding;
    SamplerEngine::Params p = s_params;
    p.maxStreamingVoices = n;
    s_params = p;
    s_node.engine.setParams(p);
    ESP_LOGI(TAG, "streaming budget -> %u (the pool grows to it as voices stream)", (unsigned)n);
    return n;
}

extern "C" uint8_t native_sampler_max_streaming(void)
{
    return native_sampler_active() ? s_params.maxStreamingVoices : 0;
}

extern "C" uint8_t native_sampler_max_voices(void)
{
    return native_sampler_active() ? s_params.maxVoices : 0;
}

extern "C" void native_sampler_set_overloaded(bool on)
{
    s_node.engine.setOverloaded(on);
}

extern "C" void native_sampler_voice_meter(uint8_t* active, uint8_t* streaming,
                                           uint8_t* limit, bool* full)
{
    if (!native_sampler_active()) {
        if (active)    *active = 0;
        if (streaming) *streaming = 0;
        if (limit)     *limit = 0;
        if (full)      *full = false;
        return;
    }
    const SamplerEngine::Meter m = s_node.engine.meter();
    if (active)    *active = m.active;
    if (streaming) *streaming = m.streaming;
    if (limit)     *limit = m.limit;
    if (full)      *full = m.limit && m.active >= m.limit;
}

/* The pool to what the voices need, on the streaming pass: a ring for every
 * voice that waits for one (SamplerEngine::ringsWaiting()), up to the
 * streaming budget, and the pages back once nothing has streamed for
 * kPoolQuietMs. reserve() and trim() take pages from and give them to the
 * arena, so the arena's owner lock and then the loader's, as every other
 * caller takes them -- tried for a tick each: a take or a pad rebuild holds
 * them for long, and the next pass is a millisecond away. */
static void pool_follow_voices()
{
    static int64_t quietSince = 0;
    if (!s_poolReady || !s_node.active.load(std::memory_order_acquire)) return;
    const uint8_t waiting = s_node.engine.ringsWaiting();
    const BlockPool::Stats st = s_pool.stats();
    uint32_t want = 0;
    bool trim = false;
    if (waiting) {
        quietSince = 0;
        want = std::min(st.blocksTotal + static_cast<uint32_t>(waiting) * kStreamBlocks,
                        pool_blocks_for(s_params.maxStreamingVoices));
        if (want <= st.blocksTotal) return;
    } else {
        const uint32_t base = ss_slots_incomplete() > 0 ? pool_blocks_for(kBaseRings) : kPreviewBlocks;
        if (st.blocksTotal <= base || s_node.engine.streamingVoices() > 0) {
            quietSince = 0;
            return;
        }
        const int64_t now = esp_timer_get_time();
        if (quietSince == 0) quietSince = now;
        if (now - quietSince < static_cast<int64_t>(kPoolQuietMs) * 1000) return;
        quietSince = 0;
        want = base;
        trim = true;
    }
    if (!engine_arena_owner_lock(1)) return;
    {
        LoaderLock lk(1, true);
        if (lk.ok() && native_sampler_active()) {
            if (trim) s_pool.trim();
            s_pool.reserve(want);
        }
    }
    engine_arena_owner_unlock();
}

extern "C" void native_sampler_service_io(void)
{
    /* An audition streams with no kit too (the start-up sound). */
    if (s_node.active.load(std::memory_order_acquire) || s_node.engine.previewBusy()) {
        s_node.engine.serviceIo();
        pool_follow_voices();
    }
}

extern "C" bool native_sampler_level_take(float* left, float* right)
{
    const float l = s_node.meter[0].exchange(0.0f, std::memory_order_relaxed);
    const float r = s_node.meter[1].exchange(0.0f, std::memory_order_relaxed);
    if (left) *left = l;
    if (right) *right = r;
    return native_sampler_active();
}

extern "C" bool native_sampler_preview(const char* fsPath, uint32_t start, uint32_t end,
                                       int volume, int pan)
{
    /* The pool's pages are this engine's only while it holds a kit; under a
     * pitched bank the arena is the bank's. */
    if (!s_node.active.load(std::memory_order_acquire) || !fsPath || !fsPath[0]) return false;
    /* The kit's convention: volume 100 is unity, pan a signed offset from
     * centre. "To the end" arrives as 0 or 0xFFFFFFFF. */
    if (end == 0xFFFFFFFFu) end = 0;
    return s_node.engine.previewStart(fsPath, start, end, SamplerKit::volumeToGain(volume > 0 ? volume : 100),
                                      SamplerKit::panToBalance(pan + 64));
}

extern "C" void native_sampler_preview_stop(void)
{
    s_node.engine.previewStop();
}

extern "C" CP_COLD void native_sampler_boot_sound(const char* fsPath)
{
    LoaderLock lk;
    ensure_reclaim_hook();
    /* Only into an empty arena that nobody owns yet: app_main calls this
     * right after engine_arena_init(), before the kit scan, as the prebuilt
     * streamer played its preview there. */
    if (!lk.ok() || s_node.active.load(std::memory_order_acquire) || engine_arena_used() != 0) return;
    if (!ensure_pool()) return;
    s_store.bind(&s_source, &arena_alloc, &arena_free, nullptr);
    s_node.engine.bind(&s_pool, &s_store);
    if (s_pool.reserve(kPreviewBlocks) < kPreviewBlocks) {
        ESP_LOGW(TAG, "start-up sound: no ring");
        return;
    }
    s_node.engine.previewStart(fsPath, 0, 0, 1.0f, 0.0f);
}

/* The audition's own peak (int16 scale), read and cleared by SMPL_ENGINE's
 * prev_pk=: what the Preview channel carries from this engine, measurable in
 * the USB audio profile too -- SMPLENG is on the companion's list, MIX_LVL's
 * whole listing is too long for SysEx. The channel's buffer is cleared before
 * this runs (SampleToolsPort::mix calls it first), so it is the audition alone. */
static std::atomic<int32_t> s_previewPeak{0};

extern "C" void native_sampler_mix_preview(float* out, uint32_t frames)
{
    if (s_node.engine.previewState() == 0) return;
    s_node.engine.mixPreview(out, frames);
    float m = 0.0f;
    for (uint32_t i = 0; i < frames * 2; ++i) {
        const float a = out[i] < 0.0f ? -out[i] : out[i];
        if (a > m) m = a;
    }
    const int32_t q = static_cast<int32_t>(m * 32767.0f);
    if (q > s_previewPeak.load(std::memory_order_relaxed)) s_previewPeak.store(q, std::memory_order_relaxed);
}

/* The tap's ring is 32 kB, taken the first time the scope is shown and kept:
 * a float vector, so a failed allocation would be an abort on the LVGL task.
 * Refused instead while PSRAM has no block with room to spare -- the scope
 * stays blank, the Sampler stays up. */
static bool s_scopeReserved = false;
constexpr size_t kScopeBytes = ScopeTap::kCapacity * sizeof(float);

extern "C" CP_COLD void native_sampler_scope_enable(bool on)
{
    if (!on) { s_node.scope.setEnabled(false); return; }
    if (s_node.scope.enabled()) return;
    if (!s_scopeReserved) {
        if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < kScopeBytes + 64 * 1024) {
            ESP_LOGW(TAG, "live scope refused: no %u kB PSRAM block to spare",
                     (unsigned)(kScopeBytes / 1024));
            return;
        }
        s_scopeReserved = true;
    }
    s_node.scope.clear();
    s_node.scope.setEnabled(true);   /* reserves before the render can see it on */
}

extern "C" uint32_t native_sampler_scope_snapshot(float* out, uint32_t maxSamples, uint32_t* sampleRate)
{
    if (sampleRate) *sampleRate = s_node.scope.sampleRate();
    return s_node.scope.snapshot(out, maxSamples);
}

extern "C" size_t native_sampler_arena_keep(void)
{
    const size_t ceiling = static_cast<size_t>(kMaxPages) * kPageBytes;
    const size_t held = s_poolReady ? s_pool.stats().bytesHeld : 0;
    return arenaLendKeep(ceiling, held, s_headBytes ? s_headBytes : kLendKeepHead, kLendKeepHeads);
}

static uint16_t ready_pads()
{
    uint16_t m = 0;
    for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
        if (native_sampler_pad_ready(p)) m |= static_cast<uint16_t>(1u << p);
    }
    return m;
}

extern "C" CP_COLD void native_sampler_status(char* out, uint32_t cap)
{
    if (!out || cap == 0) return;
    const auto c = s_node.engine.counters();
    const auto ps = s_pool.stats();
    const auto ss = s_store.stats();
    std::snprintf(out, cap,
                  "SMPLENG: engine=%s state=%u why=%s kit=%d pitch=%d,%d,%d slots=%u complete=%u retired=%u head_kb=%u heads_kb=%u "
                  "pool_kb=%u pool_blocks=%u/%u peak=%u voices=%u/%u lim=%u/%u notes=%u drop=%u steal=%u shed=%u "
                  "armed=%u fired=%u disarmed=%u expired=%u "
                  "norings=%u underrun=%u quiet=%u rt_max_us=%u noteon_max_us=%u read_kb=%u bg_stack_free=%u "
                  "preview=%u prev_pk=%d pads=0x%04x engines=%s\r\n",
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
                  native_sampler_active() ? "native" : "marcel",
#else
                  native_sampler_active() ? "native" : "none",
#endif
                  (unsigned)s_take.state(),
                  s_why[0] ? s_why : "-",
                  /* The kit the engine holds (the kit manager's id), so a run
                   * can tell "took the kit it settled on" from "kept the old
                   * one under the new one's name". */
                  s_kitId.load(std::memory_order_acquire),
                  /* What three pads' steps came out as -- the one thing a
                   * realtime step cannot be checked by ear over a serial
                   * cable, and the kit field it comes from is easy to lose. */
                  (int)s_kit.pad[0].semitones, (int)s_kit.pad[1].semitones,
                  (int)s_kit.pad[7].semitones,
                  (unsigned)ss.slots, (unsigned)ss.completeSlots,
                  /* The outgoing kit's samples still sounding after a swap. */
                  (unsigned)__builtin_popcountll(s_retired),
                  (unsigned)(s_headBytes / 1024), (unsigned)(ss.headBytes / 1024),
                  (unsigned)(ps.bytesHeld / 1024), (unsigned)ps.blocksUsed,
                  (unsigned)ps.blocksTotal, (unsigned)ps.blocksPeak,
                  (unsigned)s_node.engine.voicesActive(), (unsigned)c.voicesPeak,
                  (unsigned)s_node.engine.meter().streaming,
                  (unsigned)s_node.engine.meter().limit,
                  (unsigned)c.notesStarted, (unsigned)c.notesDropped, (unsigned)c.steals, (unsigned)c.sheds,
                  (unsigned)c.armed, (unsigned)c.armFired, (unsigned)c.armCancelled, (unsigned)c.armExpired,
                  (unsigned)c.streamRefused, (unsigned)c.underruns, (unsigned)c.silenced,
                  (unsigned)s_node.rtMaxUs.exchange(0, std::memory_order_relaxed),
                  (unsigned)c.noteOnMaxUs,
                  (unsigned)(s_bytesRead.load(std::memory_order_relaxed) / 1024),
                  /* audio_bg's stack high-water mark (bytes never used, since
                   * boot): the audition parses a header and streams on it. */
                  (unsigned)audio_bg_stack_free(),
                  /* The audition: 0 idle 1 priming 2 playing 3 finished, and
                   * its peak since the last read -- a voice that never ends
                   * shows as 2 with a peak on every read. */
                  (unsigned)s_node.engine.previewState(),
                  (int)s_previewPeak.exchange(0, std::memory_order_relaxed),
                  /* Pads with a sample in the engine, against the LEDs. */
                  (unsigned)ready_pads(),
                  /* Which engines this image was built with: SMPL_ENGINE marcel
                   * is refused unless the second one is here. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
                  "native,marcel"
#else
                  "native"
#endif
                  );
}

extern "C" CP_COLD void native_sampler_loops(char* out, uint32_t cap)
{
    if (!out || cap == 0) return;
    static constexpr char kGlyph[] = {'.', 'q', 'P', 'm'};   // LoopState order
    char pads[SamplerKit::kPads + 1];
    for (uint8_t p = 0; p < SamplerKit::kPads; ++p) {
        pads[p] = kGlyph[static_cast<uint8_t>(s_node.engine.loopState(p))];
    }
    pads[SamplerKit::kPads] = '\0';
    uint32_t toBar = 0;
    const bool running = bar_clock(nullptr, toBar);
    const auto c = s_node.engine.counters();
    std::snprintf(out, cap, "SMPLLOOPS: pads=%s queued=%u launched=%u clock=%u to_bar=%u\r\n", pads,
                  (unsigned)c.loopsQueued, (unsigned)c.loopsLaunched, running ? 1u : 0u, (unsigned)toBar);
}

extern "C" CP_COLD void native_sampler_stall_report(char* out, uint32_t cap, int on)
{
    if (!out || cap == 0) return;
    if (on >= 0) {
        for (uint8_t i = 0; i < NativeStallClock::kRecords; ++i) {
            s_stallMaxUs[i].store(0, std::memory_order_relaxed);
            s_stall[i] = NoteStall{};
        }
        s_stallOn.store(on != 0, std::memory_order_relaxed);
    }
    static const char* const kName[NativeStallClock::kRecords] = {"note", "pad"};
    uint32_t len = 0;
    for (uint8_t i = 0; i < NativeStallClock::kRecords && len < cap; ++i) {
        const NoteStall& r = s_stall[i];
        const int n = std::snprintf(out + len, cap - len,
                                    "SMPLSTALL: on=%d what=%s us=%u task=%s core=%u self_us=%u bg_us=%u idle_us=%u at_ms=%u\r\n",
                                    s_stallOn.load(std::memory_order_relaxed) ? 1 : 0, kName[i], (unsigned)r.us,
                                    r.task[0] ? r.task : "-", (unsigned)r.core, (unsigned)r.selfUs,
                                    (unsigned)r.bgUs, (unsigned)r.idleUs, (unsigned)r.atMs);
        if (n > 0) len += static_cast<uint32_t>(n);
    }
}

extern "C" bool native_sampler_shed_voice(void)
{
    return s_node.engine.shedOne();
}

/* RT, for an offline pass: the fewest frames any voice of the kit's engine
 * has streamed ahead of where it plays (SamplerEngine::streamLead()). */
extern "C" uint32_t native_sampler_stream_lead(void)
{
    return s_node.engine.streamLead();
}
