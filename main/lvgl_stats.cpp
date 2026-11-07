#include "lvgl_stats.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_lvgl_port.h>
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
#include <misc/lv_profiler_builtin_private.h>
#endif
#include <esp_timer.h>

namespace {

std::atomic<uint32_t> s_refr{0}, s_refrMaxUs{0}, s_refrTotalUs{0};
std::atomic<uint32_t> s_renderMaxUs{0}, s_renderTotalUs{0};
std::atomic<uint32_t> s_flushes{0}, s_flushWaitMaxUs{0}, s_flushWaitTotalUs{0};
std::atomic<uint32_t> s_invPx{0}, s_invAreas{0}, s_flushPx{0};
int64_t s_refrStart = 0, s_renderStart = 0, s_waitStart = 0;

/* The last invalidated rectangles, oldest overwritten: which objects a UI
 * step actually dirtied. Sizes alone (inv_px) cannot tell a row from the
 * whole list. Lives in PSRAM: the LVGL task never runs off it. */
constexpr size_t kInvRing = 256;
struct InvRect { int16_t x1, y1, x2, y2; };
InvRect* s_invRing = nullptr;
std::atomic<uint32_t> s_invHead{0};
/* LVGL_PROF 2: trace exactly one frame that redraws more than a status-bar
 * tick, so the whole thing fits the profiler buffer. */
std::atomic<bool> s_profArmFrame{false};
std::atomic<bool> s_profInFrame{false};
uint32_t s_invPxFrame = 0;
constexpr uint32_t kProfFramePx = 20000;

void bump(std::atomic<uint32_t>& max, std::atomic<uint32_t>& total, int64_t us) {
    const uint32_t v = (uint32_t)us;
    total.fetch_add(v, std::memory_order_relaxed);
    uint32_t cur = max.load(std::memory_order_relaxed);
    while (v > cur && !max.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void on_event(lv_event_t* e) {
    const int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START:        s_refrStart = now; break;
    case LV_EVENT_REFR_READY:
        s_refr.fetch_add(1, std::memory_order_relaxed);
        bump(s_refrMaxUs, s_refrTotalUs, now - s_refrStart);
        s_invPxFrame = 0;
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
        if (s_profInFrame.load(std::memory_order_relaxed)) {
            lv_profiler_builtin_set_enable(false);
            s_profInFrame.store(false, std::memory_order_relaxed);
        }
#endif
        break;
    case LV_EVENT_RENDER_START:
        s_renderStart = now;
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
        if (s_profArmFrame.load(std::memory_order_relaxed) && s_invPxFrame > kProfFramePx) {
            s_profArmFrame.store(false, std::memory_order_relaxed);
            s_profInFrame.store(true, std::memory_order_relaxed);
            lv_profiler_builtin_set_enable(true);
        }
#endif
        break;
    case LV_EVENT_RENDER_READY:      bump(s_renderMaxUs, s_renderTotalUs, now - s_renderStart); break;
    case LV_EVENT_FLUSH_START: {
        s_flushes.fetch_add(1, std::memory_order_relaxed);
        /* What actually got rendered: the union of the invalidated areas,
         * stripe by stripe, as it goes out to the panel. */
        const lv_area_t* a = static_cast<const lv_area_t*>(lv_event_get_param(e));
        if (a) s_flushPx.fetch_add((uint32_t)lv_area_get_width(a) * (uint32_t)lv_area_get_height(a),
                                   std::memory_order_relaxed);
        break;
    }
    case LV_EVENT_FLUSH_WAIT_START:  s_waitStart = now; break;
    case LV_EVENT_FLUSH_WAIT_FINISH: bump(s_flushWaitMaxUs, s_flushWaitTotalUs, now - s_waitStart); break;
    case LV_EVENT_INVALIDATE_AREA: {
        /* Which pixels every refresh has to redraw: a list that invalidates
         * the whole screen per scroll step shows up here, not in render time. */
        const lv_area_t* a = static_cast<const lv_area_t*>(lv_event_get_param(e));
        if (a) {
            const uint32_t px = (uint32_t)lv_area_get_width(a) * (uint32_t)lv_area_get_height(a);
            s_invPxFrame += px;
            s_invPx.fetch_add(px, std::memory_order_relaxed);
            s_invAreas.fetch_add(1, std::memory_order_relaxed);
            if (s_invRing) {
                const uint32_t h = s_invHead.fetch_add(1, std::memory_order_relaxed);
                s_invRing[h % kInvRing] = { (int16_t)a->x1, (int16_t)a->y1, (int16_t)a->x2, (int16_t)a->y2 };
            }
        }
        break;
    }
    default: break;
    }
}

} // namespace

#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
void (*s_profEmit)(const char*) = nullptr;

void prof_flush_line(const char* buf) {
    if (!s_profEmit) return;
    char line[160];
    const size_t n = strcspn(buf, "\r\n");
    snprintf(line, sizeof(line), "LVGLPROF: %.*s\r\n", (int)n, buf);
    s_profEmit(line);
}

void prof_reinit() {
    lv_profiler_builtin_uninit();
    lv_profiler_builtin_config_t cfg;
    lv_profiler_builtin_config_init(&cfg);
    cfg.buf_size = 64 * 1024;
    cfg.tick_per_sec = 1000000;
    cfg.tick_get_cb = []() -> uint64_t { return (uint64_t)esp_timer_get_time(); };
    cfg.flush_cb = prof_flush_line;
    lv_profiler_builtin_init(&cfg);
}
#endif

void lvgl_stats_install(lv_display_t* disp) {
    if (!disp) return;
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
    prof_reinit();
#endif
    s_invRing = static_cast<InvRect*>(heap_caps_calloc(kInvRing, sizeof(InvRect), MALLOC_CAP_SPIRAM));
    lv_display_add_event_cb(disp, on_event, LV_EVENT_ALL, nullptr);
}

void lvgl_stats_report(char* buf, size_t len) {
    const uint32_t refr = s_refr.exchange(0, std::memory_order_relaxed);
    const uint32_t refrTotal = s_refrTotalUs.exchange(0, std::memory_order_relaxed);
    const uint32_t renderTotal = s_renderTotalUs.exchange(0, std::memory_order_relaxed);
    const uint32_t waitTotal = s_flushWaitTotalUs.exchange(0, std::memory_order_relaxed);
    snprintf(buf, len,
             "LVGLSTATS: refr=%u refr_max_us=%u refr_avg_us=%u render_us=%u render_max_us=%u "
             "flush_wait_us=%u flush_wait_max_us=%u flushes=%u flush_px=%u inv_px=%u inv_areas=%u\r\n",
             (unsigned)refr,
             (unsigned)s_refrMaxUs.exchange(0, std::memory_order_relaxed),
             (unsigned)(refr ? refrTotal / refr : 0),
             (unsigned)renderTotal,
             (unsigned)s_renderMaxUs.exchange(0, std::memory_order_relaxed),
             (unsigned)waitTotal,
             (unsigned)s_flushWaitMaxUs.exchange(0, std::memory_order_relaxed),
             (unsigned)s_flushes.exchange(0, std::memory_order_relaxed),
             (unsigned)s_flushPx.exchange(0, std::memory_order_relaxed),
             (unsigned)s_invPx.exchange(0, std::memory_order_relaxed),
             (unsigned)s_invAreas.exchange(0, std::memory_order_relaxed));
}

void lvgl_stats_inv_dump(void (*emit)(const char*)) {
    const uint32_t head = s_invHead.exchange(0, std::memory_order_relaxed);
    const uint32_t n = head < kInvRing ? head : kInvRing;
    const uint32_t first = head < kInvRing ? 0 : head - kInvRing;
    char line[64];
    for (uint32_t i = 0; i < n && s_invRing; ++i) {
        const InvRect& r = s_invRing[(first + i) % kInvRing];
        snprintf(line, sizeof(line), "LVGLINV: %d %d %d %d\r\n", r.x1, r.y1, r.x2, r.y2);
        emit(line);
    }
    snprintf(line, sizeof(line), "LVGLINV: end dropped=%u\r\n", (unsigned)(head > kInvRing ? head - kInvRing : 0));
    emit(line);
}

void lvgl_stats_prof(int mode, void (*emit)(const char*)) {
#if LV_USE_PROFILER && LV_USE_PROFILER_BUILTIN
    if (!lvgl_port_lock(3000)) { emit("LVGLPROF: err lock\r\n"); return; }
    s_profEmit = emit;
    if (mode == 2) {
        prof_reinit();
        s_profArmFrame.store(true, std::memory_order_relaxed);
        emit("LVGLPROF: armed_frame\r\n");
    } else if (mode == 1) {
        prof_reinit();
        lv_profiler_builtin_set_enable(true);
        emit("LVGLPROF: armed\r\n");
    } else {
        s_profArmFrame.store(false, std::memory_order_relaxed);
        s_profInFrame.store(false, std::memory_order_relaxed);
        lv_profiler_builtin_set_enable(false);
        lv_profiler_builtin_flush();
    }
    s_profEmit = nullptr;
    lvgl_port_unlock();
#else
    emit("LVGLPROF: err not_built\r\n");
#endif
}
