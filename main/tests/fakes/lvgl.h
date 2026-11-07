// SPDX-License-Identifier: GPL-3.0-or-later
// Host stand-in: lv_async_call() runs the callback at once, as LVGL's timer
// would on its own thread.
#pragma once
typedef void (*lv_async_cb_t)(void *user_data);
#ifdef __cplusplus
extern "C" {
#endif
int lv_async_call(lv_async_cb_t cb, void *user_data);
#ifdef __cplusplus
}
#endif
