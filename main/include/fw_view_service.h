// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file fw_view_service.h
 * @brief Registers this board's IFwViewService with crosspad-gui, whose
 *        Firmware views (the Firmware app, the network-mode screen) run on it.
 *        Once, after crosspad_gui::bootstrap(), before any view is built.
 */
void fw_view_service_install(void);
