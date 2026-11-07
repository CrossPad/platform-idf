#pragma once
#ifdef USE_LVGL
#include "lvgl.h"
/** Full-screen "Link to crosspad.app": the code large, the countdown, Cancel.
 *  Opens one LVGL cycle later; a power click (or Cancel) cancels the pairing.
 *  Closing it from code does not: the host cancels when it goes.
 *  LVGL thread only. docs/design/2026-10-07-account-sync-design.md */
void acct_pair_overlay_open(void);
void acct_pair_overlay_close(void);
bool acct_pair_overlay_is_open(void);
#endif
