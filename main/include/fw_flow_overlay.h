/**
 * @file fw_flow_overlay.h
 * @brief Full-screen progress overlay for the two firmware flows the Firmware
 *        app starts: install a package from the SD card into the other slot
 *        and restart into it, or just restart into the other slot.
 *        Implementation: gui/fw_flow_overlay.cpp.
 *
 * The overlay draws on lv_layer_top(), swallows the power-button click while
 * it is up (nothing to go back to — the flash is being written) and runs the
 * fw_slots calls on tasks of its own, never on the LVGL thread. A flow that
 * ends in esp_restart() never returns; one that ends any other way (install
 * failed, switch refused, restart not possible yet) shows why for a moment,
 * closes, and fw_flow_active() drops to false — the app refreshes on that.
 */
#pragma once

#include <stdbool.h>

/** Installs `package` (a directory name under FW_PKG_ROOT) into the
 * non-running slot and, once written, switches to it and restarts. Returns
 * false when a flow is already running or the task could not be created.
 * LVGL thread only. */
bool fw_flow_install_and_restart(const char* package, bool allowUnknownBuild);

/** Switches to the non-running slot and restarts. `clearRollback` first moves
 * a RolledBack slot back to Installed (the user asked to try it again).
 * Returns false when a flow is already running or the task could not be
 * created. LVGL thread only. */
bool fw_flow_switch_and_restart(bool clearRollback);

/** True from a successful fw_flow_* call until the overlay has closed. */
bool fw_flow_active(void);
