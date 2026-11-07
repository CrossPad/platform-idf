/**
 * @file lvgl_assert_handler.h
 * @brief What an LVGL assert does: a panic, not LVGL's default while(1).
 *
 * Included by LVGL's lv_assert.h through CONFIG_LV_ASSERT_HANDLER_INCLUDE,
 * after lv_conf_internal.h has defined the default. That default spun taskLVGL
 * forever with the display lock held -- in practice on an allocation that
 * failed (LV_ASSERT_MALLOC) or a NULL handed on from one -- and the task
 * watchdog only printed: no CDC, no screen, the board dead until it was reset
 * by hand. A panic leaves a core dump and a RESET_LOG entry and the board comes
 * back by itself. lvgl_mem_psram.c is what keeps those allocations from
 * failing in the first place.
 */
#pragma once

#include <stdlib.h>

#undef LV_ASSERT_HANDLER
#define LV_ASSERT_HANDLER abort();
