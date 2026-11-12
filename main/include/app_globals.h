/**
 * @file app_globals.h
 * @brief The objects main.cpp and gui.cpp define for the whole application.
 */
#pragma once

#include <atomic>

#include "lvgl.h"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"

extern crosspad::CrosspadSettings *settings;
extern crosspad::CrosspadStatus status;

extern lv_group_t *encoderTop_group;
extern lv_indev_t *indev_encoderTop;   /* the navigation encoder */
extern lv_group_t *app_group;          /* the launcher's group (gui.cpp) */

/* False while listKits() is walking the card (main.cpp: the "kit_scan" task, and
 * the rescan an SD reinsert starts). The scan grows the kit vector in place, so
 * a reader that iterates it mid-scan walks a moved buffer: KIT_LIST and KIT_LOAD
 * answer "ERR scanning" instead of racing it. */
extern std::atomic<bool> g_kit_scan_done;
/* A boot that mounted no card has probed it once more (main.cpp). */
extern std::atomic<bool> g_sd_probed;
