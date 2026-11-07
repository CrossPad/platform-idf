/**
 * @file sd_sector_io.cpp
 * @brief The card's sector layer: a bounce for PSRAM buffers, a cache of
 *        single sectors, and SD_TRACE.
 *
 * FATFS hands sdmmc_read_sectors()/sdmmc_write_sectors() whatever buffer it
 * was given or keeps itself -- on this board mostly PSRAM, which the S3's
 * SDMMC cannot DMA to or from. Both are wrapped here (-Wl,--wrap in
 * main/CMakeLists.txt): such a transfer goes through one internal buffer
 * taken at boot, kBounceSectors at a time, and every transaction is counted
 * by the task that asked for it. A read of one sector -- what FATFS reads a
 * directory and the FAT with -- is kept (sector_cache, below).
 */
#include <string.h>
#include <algorithm>
#include <atomic>

#include "sd_sector_io.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

/* SD_TRACE: calls, sectors, time and the PSRAM buffers among them -- the
 * cost a streamer read actually pays, not the one its caller assumes. */
extern "C" esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start, size_t count);
extern "C" esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start, size_t count);
extern "C" esp_err_t __real_sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card);

namespace {
struct TraceRow {
    uint32_t calls, sectors, ext, one, us, maxUs;
};
enum { kBg, kSong, kLoad, kLvgl, kOther, kWho };
const char *const kWhoName[kWho] = {"audio_bg", "song_io", "kit_load", "lvgl", "other"};
TraceRow s_rd[kWho], s_wr[kWho];
/* The slowest transaction each way since the last SD_TRACE, by task name:
 * "other" hides who wrote just before a card stall. */
struct Slowest {
    uint32_t us;
    char task[16];
};
Slowest s_slowRd, s_slowWr;
uint32_t s_cacheHits, s_cacheMisses;
portMUX_TYPE s_traceMux = portMUX_INITIALIZER_UNLOCKED;

int who()
{
    const char *n = pcTaskGetName(nullptr);
    if (!strcmp(n, "audio_bg")) return kBg;
    if (!strcmp(n, "song_io")) return kSong;
    if (!strcmp(n, "kit_load") || !strncmp(n, "smpl_", 5)) return kLoad;
    if (!strncmp(n, "taskLVGL", 8) || !strncmp(n, "LVGL", 4)) return kLvgl;
    return kOther;
}

void note(TraceRow *rows, const void *buf, size_t count, uint32_t us)
{
    const int w = who();
    const char *name = pcTaskGetName(nullptr);
    Slowest &slow = rows == s_wr ? s_slowWr : s_slowRd;
    portENTER_CRITICAL(&s_traceMux);
    if (us > slow.us) {
        slow.us = us;
        strncpy(slow.task, name ? name : "?", sizeof(slow.task) - 1);
        slow.task[sizeof(slow.task) - 1] = '\0';
    }
    TraceRow &r = rows[w];
    r.calls++;
    r.sectors += count;
    if (esp_ptr_external_ram(buf)) r.ext++;
    if (count == 1) r.one++;
    r.us += us;
    if (us > r.maxUs) r.maxUs = us;
    portEXIT_CRITICAL(&s_traceMux);
}
} // namespace

/* A cache of single sectors. Opening a kit's sample walks /sdcard/crosspad/kits,
 * a directory of ~190 kits whose entries span dozens of sectors, and FATFS
 * reads a directory -- and the FAT -- a sector a command, through one window
 * per volume that every task's file shares: a kit's load took ~700 such
 * reads, ~350 ms of a ~450 ms load with the card to itself (SD_TRACE kit_load
 * single=, 2026-10-27), and every other reader paid the same walk. Two ways
 * of 32 sets in PSRAM (32 kB). A write takes the sectors it covers out, a
 * card init takes everything out (another card, or the same one written by
 * a computer), and a read that raced a write is not kept (s_gen). SD_CACHE
 * 0 turns it off for an A/B. */
namespace sector_cache {
constexpr uint32_t kSets = 32, kWays = 2;
struct Line {
    const sdmmc_card_t *card;
    uint32_t sector;
    bool valid;
};
/* PSRAM: 800 B of internal RAM was what a boot's kit scan could not spare. */
EXT_RAM_BSS_ATTR Line s_line[kSets][kWays];
EXT_RAM_BSS_ATTR uint8_t s_lru[kSets];      // the way to fill next
uint8_t *s_data;                            // kSets * kWays * 512, PSRAM
uint32_t s_gen;                             // bumped by every write and flush
std::atomic<uint8_t> s_on{1};
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

uint8_t *data(uint32_t set, uint32_t way) { return s_data + (set * kWays + way) * 512; }

bool lookup(const sdmmc_card_t *card, uint32_t sector, void *dst)
{
    if (!s_data || !s_on.load(std::memory_order_relaxed)) return false;
    const uint32_t set = sector % kSets;
    bool hit = false;
    portENTER_CRITICAL(&s_mux);
    for (uint32_t w = 0; w < kWays; ++w) {
        const Line &l = s_line[set][w];
        if (l.valid && l.card == card && l.sector == sector) {
            memcpy(dst, data(set, w), 512);
            s_lru[set] = (uint8_t)(w ^ 1u);
            hit = true;
            break;
        }
    }
    if (hit) ++s_cacheHits;
    else     ++s_cacheMisses;
    portEXIT_CRITICAL(&s_mux);
    return hit;
}

uint32_t gen()
{
    portENTER_CRITICAL(&s_mux);
    const uint32_t g = s_gen;
    portEXIT_CRITICAL(&s_mux);
    return g;
}

void keep(const sdmmc_card_t *card, uint32_t sector, const void *src, uint32_t genBefore)
{
    if (!s_data || !s_on.load(std::memory_order_relaxed)) return;
    const uint32_t set = sector % kSets;
    portENTER_CRITICAL(&s_mux);
    if (s_gen == genBefore) {
        const uint32_t w = s_lru[set];
        memcpy(data(set, w), src, 512);
        s_line[set][w] = Line{card, sector, true};
        s_lru[set] = (uint8_t)(w ^ 1u);
    }
    portEXIT_CRITICAL(&s_mux);
}

void forget(const sdmmc_card_t *card, uint32_t start, uint32_t count)
{
    portENTER_CRITICAL(&s_mux);
    ++s_gen;
    for (uint32_t set = 0; set < kSets; ++set)
        for (uint32_t w = 0; w < kWays; ++w) {
            Line &l = s_line[set][w];
            if (l.valid && (!card || (l.card == card && l.sector - start < count))) l.valid = false;
        }
    portEXIT_CRITICAL(&s_mux);
}
} // namespace sector_cache

extern "C" void sd_cache_enable(bool on)
{
    sector_cache::s_on.store(on ? 1 : 0);
    sector_cache::forget(nullptr, 0, 0);
}

extern "C" bool sd_cache_on(void) { return sector_cache::s_on.load(std::memory_order_relaxed) != 0; }

extern "C" esp_err_t __wrap_sdmmc_card_init(const sdmmc_host_t *host, sdmmc_card_t *card)
{
    sector_cache::forget(nullptr, 0, 0);
    return __real_sdmmc_card_init(host, card);
}

/* Sector-level bounce. The S3's SDMMC cannot DMA to or from PSRAM, and IDF
 * then splits a transfer into one CMD17/CMD24 a sector through a buffer it
 * mallocs from the DMA heap on every call -- 4x slower than a multi-sector
 * command, and an error ("not enough mem") the moment WiFi has that heap.
 * Here a buffer taken once carries kBounceSectors at a time: 16, not the 8 it
 * was -- half the commands for every reader at once, and the song's card time
 * under eight tracks and pads went from 6--11 s to 3--5 s a 40 s run
 * (hil_track_transitions.py, 2026-10-27), for 4 kB more internal RAM. */
constexpr size_t kBounceSectors = 16;
static uint8_t *s_bounce;
static SemaphoreHandle_t s_bounceMux;
static std::atomic<uint8_t> s_bounceOn{1};

extern "C" void sd_bounce_init(void)
{
    if (s_bounce) return;
    s_bounceMux = xSemaphoreCreateMutex();
    s_bounce = (uint8_t *)heap_caps_aligned_alloc(4, kBounceSectors * 512, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    sector_cache::s_data = (uint8_t *)heap_caps_malloc(sector_cache::kSets * sector_cache::kWays * 512,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

extern "C" void sd_bounce_enable(bool on) { s_bounceOn.store(on ? 1 : 0); }

/* Whether a transfer to or from PSRAM may simply be handed to the card. */
extern "C" bool sd_bounce_ready(void) { return s_bounce && s_bounceOn.load(std::memory_order_relaxed); }

static bool needs_bounce(const void *p)
{
    return esp_ptr_external_ram(p) || !esp_ptr_dma_capable(p) || ((uintptr_t)p & 3u);
}

extern "C" esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t *card, void *dst, size_t start, size_t count)
{
    const int64_t t = esp_timer_get_time();
    const bool one = count == 1;
    if (one && sector_cache::lookup(card, (uint32_t)start, dst)) return ESP_OK;
    const uint32_t gen = one ? sector_cache::gen() : 0;
    esp_err_t e;
    if (s_bounce && s_bounceOn.load(std::memory_order_relaxed) && needs_bounce(dst)) {
        xSemaphoreTake(s_bounceMux, portMAX_DELAY);
        e = ESP_OK;
        for (size_t done = 0; done < count && e == ESP_OK;) {
            const size_t n = std::min(count - done, kBounceSectors);
            e = __real_sdmmc_read_sectors(card, s_bounce, start + done, n);
            if (e == ESP_OK) memcpy((uint8_t *)dst + done * 512, s_bounce, n * 512);
            done += n;
        }
        xSemaphoreGive(s_bounceMux);
    } else {
        e = __real_sdmmc_read_sectors(card, dst, start, count);
    }
    if (one && e == ESP_OK) sector_cache::keep(card, (uint32_t)start, dst, gen);
    note(s_rd, dst, count, (uint32_t)(esp_timer_get_time() - t));
    return e;
}

extern "C" esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t *card, const void *src, size_t start, size_t count)
{
    const int64_t t = esp_timer_get_time();
    sector_cache::forget(card, (uint32_t)start, (uint32_t)count);
    esp_err_t e;
    if (s_bounce && s_bounceOn.load(std::memory_order_relaxed) && needs_bounce(src)) {
        xSemaphoreTake(s_bounceMux, portMAX_DELAY);
        e = ESP_OK;
        for (size_t done = 0; done < count && e == ESP_OK;) {
            const size_t n = std::min(count - done, kBounceSectors);
            memcpy(s_bounce, (const uint8_t *)src + done * 512, n * 512);
            e = __real_sdmmc_write_sectors(card, s_bounce, start + done, n);
            done += n;
        }
        xSemaphoreGive(s_bounceMux);
    } else {
        e = __real_sdmmc_write_sectors(card, src, start, count);
    }
    note(s_wr, src, count, (uint32_t)(esp_timer_get_time() - t));
    return e;
}

/* One line per caller with traffic, then clears. */
extern "C" void sd_trace_cmd(char *out, size_t outLen)
{
    TraceRow rd[kWho], wr[kWho];
    portENTER_CRITICAL(&s_traceMux);
    memcpy(rd, s_rd, sizeof rd);
    memcpy(wr, s_wr, sizeof wr);
    memset(s_rd, 0, sizeof s_rd);
    memset(s_wr, 0, sizeof s_wr);
    const Slowest slowRd = s_slowRd, slowWr = s_slowWr;
    s_slowRd = Slowest{};
    s_slowWr = Slowest{};
    portEXIT_CRITICAL(&s_traceMux);
    portENTER_CRITICAL(&sector_cache::s_mux);
    const uint32_t hits = s_cacheHits, misses = s_cacheMisses;
    s_cacheHits = s_cacheMisses = 0;
    portEXIT_CRITICAL(&sector_cache::s_mux);
    size_t o = snprintf(out, outLen, "SDTRACE: cache=%d hits=%u misses=%u slow_rd=%s:%u slow_wr=%s:%u",
                        sd_cache_on() ? 1 : 0, (unsigned)hits, (unsigned)misses, slowRd.us ? slowRd.task : "-",
                        (unsigned)slowRd.us, slowWr.us ? slowWr.task : "-", (unsigned)slowWr.us);
    for (int dir = 0; dir < 2; ++dir) {
        const TraceRow *rows = dir ? wr : rd;
        for (int w = 0; w < kWho; ++w) {
            const TraceRow &r = rows[w];
            if (!r.calls || o >= outLen) continue;
            o += snprintf(out + o, outLen - o,
                          " | %s %s calls=%u sectors=%u ext=%u single=%u busy_us=%u avg_us=%u max_us=%u",
                          dir ? "W" : "R", kWhoName[w], (unsigned)r.calls, (unsigned)r.sectors, (unsigned)r.ext,
                          (unsigned)r.one, (unsigned)r.us, (unsigned)(r.us / r.calls), (unsigned)r.maxUs);
        }
    }
    if (o < outLen) snprintf(out + o, outLen - o, "\r\n");
}
