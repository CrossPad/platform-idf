#pragma once
#include <atomic>
#include <cstdint>

/* Remote (HIL) encoder injection. The values are consumed by
 * encoderTop_read() — the same read callback the physical encoder feeds — so
 * LVGL sees a genuine encoder indev: focus movement, edit mode, long press and
 * lv_indev_active() all behave as they do under a human hand, which
 * synthesized LV_EVENT_* sends cannot reproduce. */
inline std::atomic<int32_t>& encoderTop_hil_diff() {
    static std::atomic<int32_t> diff{0};
    return diff;
}

/* lv_tick deadline the injected switch stays pressed until. */
inline std::atomic<uint32_t>& encoderTop_hil_press_until() {
    static std::atomic<uint32_t> until{0};
    return until;
}
