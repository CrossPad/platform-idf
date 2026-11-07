// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/* The board's side of a kit rate conversion (crosspad/kit/KitRateConvert.hpp):
 * where its memory, card I/O, pacing and free space come from, and its CDC
 * verbs. The worker and the UI are the Sampler's (kit_convert_task). */

namespace crosspad { class IKitConvertHost; }

crosspad::IKitConvertHost *kit_convert_host(void);

/* KIT_RATES, KIT_CONVERT <id> [rate], KIT_CONVERT_STOP, KIT_CONVERT_STATUS,
 * KIT_KEEP_RATE <id> <0|1>, KIT_CONVERT_TIDY <id>. */
bool kit_convert_verbs_handle(const char *cmd, void (*reply)(const char *text));
