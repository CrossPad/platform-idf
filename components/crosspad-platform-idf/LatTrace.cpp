// SPDX-License-Identifier: GPL-3.0-or-later
//
// LAT_TRACE: lat_trace.h says what it is. The render-side calls only store
// into atomics and PSRAM arrays the render alone writes; the arrays are taken
// by the first LAT_TRACE RESET, from the CDC task, never in the render -- the
// UI lives in what PSRAM is left, so a board nobody traces pays nothing.

#include "lat_trace.h"

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace {

constexpr int kPads = 16;

enum Stage : uint8_t { kRx, kParse, kPosted, kHandler, kEngine, kPass, kVoice, kWrite, kDac, kArm, kFire, kStages };

struct Segment { const char *name; Stage from, to; };
constexpr Segment kSegments[] = {
    {"uart_to_parse",     kRx,      kParse},
    {"parse_to_posted",   kParse,   kPosted},
    {"parse_to_handler",  kParse,   kHandler},
    {"handler_to_engine", kHandler, kEngine},
    {"engine_to_pass",    kEngine,  kPass},
    {"pass_to_voice",     kPass,    kVoice},
    {"pass_to_write",     kPass,    kWrite},
    {"pass_to_dac",       kPass,    kDac},
    {"uart_to_pass",      kRx,      kPass},
    {"uart_to_dac",       kRx,      kDac},
    {"arm_to_uart",       kArm,     kRx},
    {"parse_to_fire",     kParse,   kFire},
    {"fire_to_pass",      kFire,    kPass},
};
constexpr int kSegmentCount = sizeof(kSegments) / sizeof(kSegments[0]);

constexpr uint32_t kSamples = 256;   // per segment, the most recent
constexpr uint32_t kRecords = 128;   // complete hits kept whole for LATREC

/* One hit in flight on a pad. `armed` is published last by the note path and
 * cleared by the render once the hit is complete. */
struct Slot {
    std::atomic<uint32_t> t[kStages];
    std::atomic<bool> armed{false};
};

struct Record { uint8_t pad; uint32_t t[kStages]; };

struct Store {
    int32_t  samples[kSegmentCount][kSamples];
    Record   records[kRecords];
};

EXT_RAM_BSS_ATTR Slot s_slots[kPads];
/* When a touch armed each pad's voice: it comes before the note that starts
 * a hit's slot, so it waits here until then. */
EXT_RAM_BSS_ATTR std::atomic<uint32_t> s_armUs[kPads];
/* An arm older than this belongs to an earlier touch, not to this note. */
constexpr uint32_t kArmMaxAgeUs = 300 * 1000;
std::atomic<Store *> s_store{nullptr};
std::atomic<uint32_t> s_hits{0};          // complete hits since reset, all segments
std::atomic<uint32_t> s_dacUnknown{0};    // complete hits whose pass had no DAC estimate
std::atomic<uint32_t> s_lost{0};          // a new hit on a pad before the last one was heard
std::atomic<uint32_t> s_passUs{0};
uint32_t s_passPads = 0;                  // render thread only

inline uint32_t now32() { return static_cast<uint32_t>(esp_timer_get_time()); }

/* snprintf that never leaves `len` past the buffer: a report cut short
 * stays a report. */
void append(char *out, size_t cap, size_t &len, const char *fmt, ...)
{
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + len, cap - len, fmt, ap);
    va_end(ap);
    if (n > 0) len = std::min(cap - 1, len + static_cast<size_t>(n));
}

inline Slot *slot(uint8_t pad) { return pad < kPads ? &s_slots[pad] : nullptr; }

void stamp(uint8_t pad, Stage st)
{
    Slot *s = slot(pad);
    if (s && s->armed.load(std::memory_order_acquire)) s->t[st].store(now32(), std::memory_order_relaxed);
}

} // namespace

extern "C" void lat_trace_note_in(uint8_t pad, uint32_t uart_rx_us)
{
    Slot *s = slot(pad);
    if (!s || !s_store.load(std::memory_order_acquire)) return;
    if (s->armed.exchange(false, std::memory_order_acq_rel)) s_lost.fetch_add(1, std::memory_order_relaxed);
    for (auto &t : s->t) t.store(0, std::memory_order_relaxed);
    const uint32_t armed = s_armUs[pad].exchange(0, std::memory_order_relaxed);
    if (armed && uart_rx_us - armed < kArmMaxAgeUs) s->t[kArm].store(armed, std::memory_order_relaxed);
    s->t[kRx].store(uart_rx_us, std::memory_order_relaxed);
    s->t[kParse].store(now32(), std::memory_order_relaxed);
    s->armed.store(true, std::memory_order_release);
}

extern "C" void lat_trace_note_posted(uint8_t pad)  { stamp(pad, kPosted); }
extern "C" void lat_trace_note_handler(uint8_t pad) { stamp(pad, kHandler); }
extern "C" void lat_trace_note_engine(uint8_t pad)  { stamp(pad, kEngine); }
extern "C" void lat_trace_note_fire(uint8_t pad)    { stamp(pad, kFire); }

extern "C" void lat_trace_note_arm(uint8_t pad)
{
    if (pad >= kPads || !s_store.load(std::memory_order_acquire)) return;
    const uint32_t t = now32();
    s_armUs[pad].store(t ? t : 1u, std::memory_order_relaxed);
}

extern "C" void lat_trace_pass_begin(uint32_t now_us)
{
    s_passUs.store(now_us, std::memory_order_relaxed);
    s_passPads = 0;
}

extern "C" void lat_trace_voice_start(uint8_t pad)
{
    Slot *s = slot(pad);
    if (!s || !s->armed.load(std::memory_order_acquire)) return;
    if (s->t[kVoice].load(std::memory_order_relaxed)) return;
    s->t[kPass].store(s_passUs.load(std::memory_order_relaxed), std::memory_order_relaxed);
    s->t[kVoice].store(now32(), std::memory_order_relaxed);
    s_passPads |= 1u << pad;
}

extern "C" void lat_trace_pass_end(uint32_t write_done_us, uint32_t dac_us)
{
    if (!s_passPads) return;
    Store *st = s_store.load(std::memory_order_acquire);
    for (int pad = 0; pad < kPads; ++pad) {
        if (!(s_passPads & (1u << pad))) continue;
        Slot &s = s_slots[pad];
        s.t[kWrite].store(write_done_us, std::memory_order_relaxed);
        s.t[kDac].store(dac_us, std::memory_order_relaxed);
        if (!st) { s.armed.store(false, std::memory_order_release); continue; }
        const uint32_t n = s_hits.load(std::memory_order_relaxed);
        Record &r = st->records[n % kRecords];
        r.pad = static_cast<uint8_t>(pad);
        for (int i = 0; i < kStages; ++i) r.t[i] = s.t[i].load(std::memory_order_relaxed);
        for (int g = 0; g < kSegmentCount; ++g) {
            const Segment &seg = kSegments[g];
            const uint32_t a = r.t[seg.from], b = r.t[seg.to];
            /* A stage the hit did not pass through (no DAC estimate this pass)
             * is a zero stamp; the segment carries a sentinel the report skips. */
            st->samples[g][n % kSamples] = (a && b) ? static_cast<int32_t>(b - a) : INT32_MIN;
        }
        if (!dac_us) s_dacUnknown.fetch_add(1, std::memory_order_relaxed);
        s_hits.store(n + 1, std::memory_order_release);
        s.armed.store(false, std::memory_order_release);
    }
    s_passPads = 0;
}

extern "C" bool lat_trace_reset(void)
{
    /* Taken on the first reset and kept: the render may be writing into it at
     * any moment, so it is never handed back. Nothing is traced before. */
    if (!s_store.load(std::memory_order_acquire)) {
        auto *st = static_cast<Store *>(heap_caps_calloc(1, sizeof(Store), MALLOC_CAP_SPIRAM));
        if (!st) return false;
        s_store.store(st, std::memory_order_release);
    }
    s_hits.store(0, std::memory_order_release);
    s_dacUnknown.store(0, std::memory_order_relaxed);
    s_lost.store(0, std::memory_order_relaxed);
    return true;
}

extern "C" size_t lat_trace_report(char *out, size_t cap)
{
    if (!out || !cap) return 0;
    Store *st = s_store.load(std::memory_order_acquire);
    const uint32_t hits = s_hits.load(std::memory_order_acquire);
    size_t len = 0;
    append(out, cap, len, "LAT: hits=%u lost=%u dac_unknown=%u\r\n", (unsigned)hits,
           (unsigned)s_lost.load(), (unsigned)s_dacUnknown.load());
    if (!st || !hits) return len;
    auto *v = static_cast<int32_t *>(heap_caps_malloc(kSamples * sizeof(int32_t), MALLOC_CAP_SPIRAM));
    if (!v) {
        append(out, cap, len, "LAT: ERR no_mem\r\n");
        return len;
    }
    const uint32_t have = hits < kSamples ? hits : kSamples;
    for (int g = 0; g < kSegmentCount; ++g) {
        uint32_t n = 0;
        int64_t sum = 0;
        for (uint32_t i = 0; i < have; ++i) {
            const int32_t x = st->samples[g][i];
            if (x == INT32_MIN) continue;
            v[n++] = x;
            sum += x;
        }
        if (!n) {
            append(out, cap, len, "LAT: seg=%s n=0\r\n", kSegments[g].name);
            continue;
        }
        /* The ROM's qsort, not std::sort: this report is not worth 0.6 kB of
         * image, and every kB of image is PSRAM the UI loses. */
        qsort(v, n, sizeof(int32_t), [](const void *a, const void *b) {
            const int32_t x = *static_cast<const int32_t *>(a), y = *static_cast<const int32_t *>(b);
            return (x > y) - (x < y);
        });
        append(out, cap, len,
                        "LAT: seg=%s n=%u min=%d p50=%d p95=%d max=%d mean=%d\r\n",
                        kSegments[g].name, (unsigned)n, (int)v[0], (int)v[(n - 1) / 2],
                        (int)v[(n * 95 + 99) / 100 - 1], (int)v[n - 1], (int)(sum / n));
    }
    heap_caps_free(v);
    return len;
}

extern "C" size_t lat_trace_raw(char *out, size_t cap, uint32_t first, uint32_t count)
{
    if (!out || !cap) return 0;
    size_t len = 0;
    Store *st = s_store.load(std::memory_order_acquire);
    const uint32_t hits = s_hits.load(std::memory_order_acquire);
    const uint32_t oldest = hits > kRecords ? hits - kRecords : 0;
    if (first < oldest) first = oldest;
    const uint32_t stop = (count && first + count < hits) ? first + count : hits;
    if (st) {
        for (uint32_t i = first; i < stop; ++i) {
            const Record &r = st->records[i % kRecords];
            append(out, cap, len,
                            "LATREC: i=%u pad=%u rx=%u parse=%u posted=%u handler=%u engine=%u "
                            "pass=%u voice=%u write=%u dac=%u arm=%u fire=%u\r\n",
                            (unsigned)i, r.pad, (unsigned)r.t[kRx], (unsigned)r.t[kParse],
                            (unsigned)r.t[kPosted], (unsigned)r.t[kHandler], (unsigned)r.t[kEngine],
                            (unsigned)r.t[kPass], (unsigned)r.t[kVoice], (unsigned)r.t[kWrite],
                            (unsigned)r.t[kDac], (unsigned)r.t[kArm], (unsigned)r.t[kFire]);
        }
    }
    append(out, cap, len, "LATREC: end n=%u\r\n", (unsigned)hits);
    return len;
}
