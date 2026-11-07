#pragma once
// LVGL refresh accounting: how long each refresh cycle takes and how much of
// it is spent rendering versus waiting for the panel to take the last flush.
// Read-and-clear over CDC (`LVGL_STATS`), so a bracketed window -- a scroll
// burst, an app start -- reports its own worst and average case.
#include <stddef.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void lvgl_stats_install(lv_display_t* disp);
/// Formats one `LVGLSTATS: ...` line into buf and clears the counters.
void lvgl_stats_report(char* buf, size_t len);
/// Emits one `LVGLINV: x1 y1 x2 y2` row per invalidated area since the last
/// dump (oldest first, at most the last 256), then `LVGLINV: end dropped=N`.
void lvgl_stats_inv_dump(void (*emit)(const char*));
/// LVGL built-in profiler. mode 1 arms a fresh 64 kB trace now, mode 2 arms
/// it for the next frame that redraws more than a status-bar tick (one frame
/// fits the buffer, a whole encoder step does not), mode 0 stops it and emits
/// one `LVGLPROF: <systrace line>` row per event. Built only with
/// LV_USE_PROFILER; otherwise answers `LVGLPROF: err not_built`.
void lvgl_stats_prof(int mode, void (*emit)(const char*));

#ifdef __cplusplus
}
#endif
