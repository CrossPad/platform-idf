/**
 * @file fw_slots.h
 * @brief What firmware sits in each OTA slot, installing a package from the
 *        SD card into the non-running one, and switching which slot boots
 *        next.
 *
 * The two ota_* app partitions (partitions_16mb.csv) are labelled 'A' (ota_0)
 * and 'B' (ota_1) — a fixed mapping, independent of which one is currently
 * running. Installing never touches the boot slot — fw_install_start() only
 * ever writes the slot that is *not* running. fw_switch_request() is the one
 * function that moves the boot partition.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"

#include "crosspad/fw/FwPackage.hpp"
#include "fw_library.h"

using FwSlotState = crosspad::FwSlotState;
using FwSlotInfo = crosspad::FwSlotInfo;

enum class FwInstallPhase : uint8_t { Idle, Verifying, Writing, Done, Failed };

#define FW_INSTALL_ERROR_LEN 64

/* Fixed array, not a pointer into shared storage: fw_install_status() returns
 * this by value under a mutex, but only the struct's own bytes are copied
 * while the mutex is held — a pointer into a module-static buffer would keep
 * changing after the mutex is released, underneath whatever is still reading
 * .error (an LVGL timer polling this is exactly that). */
struct FwInstallStatus {
    FwInstallPhase phase;
    uint8_t percent;
    char error[FW_INSTALL_ERROR_LEN];  // "" when there is none
};

/** Reconciles the running slot's registry entry against what is actually
 * running (an OTA outside this module replaces a slot's image without going
 * through fw_install_start()). Call once, after NVS is up. */
void fw_slots_init(void);

/** out[0] = slot A (ota_0), out[1] = slot B (ota_1). */
void fw_slots_get(FwSlotInfo out[2]);

/** The slot not running holds an image that can boot (not empty, not rolled
 * back) of this kit and version ("v1.3.0" = "1.3.0"). A RAM copy kept current
 * by this module: safe from any task and any stack, no flash read. */
bool fw_slots_other_holds(const char* kit, const char* version);

/** Starts installing `packageName` into the non-running slot on its own task.
 * Returns false when refused synchronously: another install is running, the
 * package is missing, its verdict is not Ok (or is UnknownBuild without
 * allowUnknownBuild), the battery is low with no charger, or a kit is
 * loading. On false `*error` (when given) names the reason, and so does
 * fw_install_status().error — except for "install already running", where the
 * status belongs to the install that *is* running and is left untouched.
 * Never changes the boot slot. */
bool fw_install_start(const char* packageName, bool allowUnknownBuild, const char** error);

/** Snapshot of the current (or last finished) install. */
FwInstallStatus fw_install_status(void);

/** Pure query: true when pkg's declared stmProtoMin is lower than the
 * STM32's currently running protocol (stm32_hal's cached link info, no I2C
 * probe). False when the link isn't up. Safe to call any time, including
 * before committing to an install — unlike fw_install_stm_proto_older()
 * below, it has no dependency on a preceding fw_install_start() call. The
 * single source of truth for the comparison; fw_install_start() itself uses
 * it to fill fw_install_stm_proto_older()'s answer. */
bool fw_package_stm_proto_older(const FwPackage &pkg);

/** True when the package handed to the most recent fw_install_start() call
 * declared a lower stmProtoMin than the STM32's currently running protocol
 * (from stm32_hal's cached link info) — this firmware was built against an
 * older pad-controller protocol than what the board is running right now.
 * Informational only: it never refuses the install, and it says nothing
 * about whether the install actually starts. Meant for FW_INSTALL's CDC
 * reply (`OK warn=stm_proto_older`). The GUI confirm dialog uses
 * fw_package_stm_proto_older() instead, since it needs the answer before
 * fw_install_start() is ever called. */
bool fw_install_stm_proto_older(void);

/** Requests a switch to the other OTA slot. Refuses (`*error` set to a
 * static string) when it is empty, is RolledBack (did not start last
 * time), its image's CPFI board revision is not this board's, an install is currently writing it, or the *running* slot has not
 * confirmed itself yet (leaving a PENDING_VERIFY image behind gets the one
 * that works marked ABORTED by the bootloader). Otherwise: if the running
 * slot has a package name and a directory on the card, exports its current
 * settings into `<pkg>/settings.json` (a failure there is logged, not
 * fatal); records the other slot's package name as the NVS `pending`
 * switch; moves the boot partition to the other slot. Returns true once
 * the switch is committed — the caller restarts after telling the host,
 * never before. */
bool fw_switch_request(const char** error);

/** The way out of a firmware that does not work, without the device's own UI
 * (pad 16 held at boot, main.cpp): fw_switch_request() to the other slot when
 * that holds a main kit (full, lite) and the running one does not -- also
 * from a slot that has not confirmed itself (it is the broken one; the
 * bootloader marks it ABORTED) and to one that rolled back last time.
 * Refused, with `error` set, otherwise. The caller restarts. */
bool fw_recovery_switch(const char** error);

/** Writing the non-running slot now would replace the main firmware: the
 * running slot holds another kit (a game, a single-purpose firmware) and the
 * other one a main kit (full, lite) -- the firmware Quit and the pad-16
 * recovery go back to. An OTA and fw_install_start() are refused then. When
 * true, `otherKit` (may be null) gets that slot's kit, NUL-terminated. */
bool fw_slots_write_would_replace_main(char* otherKit, size_t otherKitLen);

/** Completes a pending switch after boot. When NVS `pending` equals the
 * running slot's own package name and `<pkg>/settings.json` exists on the
 * card, imports it with settingsImportPerFirmware() (device-shared settings
 * excluded) and saves the result through the normal save path so NVS
 * matches. Clears `pending` in every case. Call once, after NVS and the SD
 * card are up and before anything reads a per-firmware setting such as
 * `Kit` — never in USB transfer mode, where the SD card belongs to the
 * host. A no-op when no switch is pending. */
void fw_switch_finish_boot(void);

/** Marks the running slot's image workable and cancels any pending rollback
 * when it is still ESP_OTA_IMG_PENDING_VERIFY (a fresh install-and-switch
 * that has not confirmed itself yet) — moves its registry state to
 * Confirmed. A no-op otherwise. Idempotent and safe to call from any task:
 * the otadata write always runs on a dedicated internal-RAM-stack task of
 * its own, never on the caller's. Called from a periodic timer armed in
 * app_main() and from `FW_CONFIRM`. */
void fw_confirm_running(void);

/** Hands fw_confirm_running() the periodic timer app_main() armed it with, so
 * the confirm task can stop and delete it once the running image is no longer
 * PENDING_VERIFY — including on the first tick of a boot that had nothing to
 * confirm. Until then the timer keeps calling fw_confirm_running(), which is
 * what makes a failed confirm-task creation a retry instead of a lost trial.
 * Call once, before starting the timer. */
void fw_confirm_set_timer(esp_timer_handle_t timer);

/** Moves a slot out of RolledBack back to Installed, so fw_switch_request()
 * will consider it again — the user asking to try a firmware that did not
 * start last time once more. A no-op on a slot in any other state. */
void fw_slot_clear_rollback(char label);

/** True when the flashed bootloader's own build declares rollback support:
 * reads esp_ota_get_bootloader_description() and checks its version against
 * the minimum this project ships rollback with (CONFIG_BOOTLOADER_PROJECT_VER,
 * bumped alongside CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y — the two travel
 * together). False when the bootloader predates that (no description, or a
 * lower version). Deterministic and independent of whether a trial has ever
 * actually run on this device — replaces an earlier NVS-history heuristic
 * that read false on every freshly-flashed unit regardless of the actual
 * bootloader, which made it useless for factory gating. */
bool fw_rollback_supported(void);

/** Arms the RTC watchdog (WDT_RWDT) for kTrialWatchdogTimeoutS if — and only
 * if — the running image is still ESP_OTA_IMG_PENDING_VERIFY (a fresh
 * install-and-switch that has not confirmed itself yet). A no-op otherwise.
 * Call as the very first statement of app_main(), before anything else: no
 * NVS, no heap allocation, no task/scheduler dependency — raw register
 * writes through wdt_hal, exactly the shape ESP-IDF's own
 * esp_restart_noos()/light-sleep safety net use. If the image never confirms
 * (a hang, a crash loop, or bsp_stm32_init() failing so fw_confirm_running()'s
 * own timer is never armed), the watchdog resets the chip — the bootloader
 * still sees this same image PENDING_VERIFY, marks it ABORTED, and rolls
 * back to the previous slot on its own. fw_confirm_running() disables this
 * watchdog once it actually confirms. */
void fw_trial_watchdog_arm(void);

/** Resets the trial watchdog's countdown without disabling it — for the one
 * boot step that can legitimately take seconds out of the 30 s window before
 * anything confirms (fw_assets_sync()'s partition rewrite). A no-op unless
 * fw_trial_watchdog_arm() actually armed it this boot. */
void fw_trial_watchdog_feed(void);

/** True from fw_trial_watchdog_arm() finding the running image PENDING_VERIFY
 * until fw_confirm_running() has marked it valid: this boot is a trial, and
 * any crash in it rolls the slot back. A RAM flag, safe from any task and any
 * stack. */
bool fw_running_on_trial(void);

/** True once, right after fw_slots_init() found the other slot rolled back
 * on its own (state newly set to RolledBack) — never twice for the same
 * event. On true, copies the failed slot's package name into failedPkg and
 * the running slot's own package name into runningPkg (either may be "").
 * Meant to be polled once the launcher is up, to raise one toast. */
bool fw_rollback_toast_pending(char *failedPkg, size_t failedLen, char *runningPkg, size_t runningLen);
