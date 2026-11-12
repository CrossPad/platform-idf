/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Started from an ESP-IDF example (2022-2024 Espressif Systems (Shanghai)
 * CO LTD, Unlicense OR CC0-1.0).
 */

#include "sd_sector_io.h"
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "esp_partition.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include <algorithm>
#include <iterator>
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "soc/rtc_cntl_reg.h"
#include "soc/system_reg.h"
#endif
#include "nvs_flash.h"
#include "nvs.h"
#include "imu_midi.h"
#include "NvsSettingsStore.hpp"
#include "OutputLevelsPort.hpp"

#include "audio/audio_task.h"
#include "freertos/idf_additions.h"
#include "esp_memory_utils.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "bsp_worker.h"
#include "bsp_ulp.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#include "include/usb_descriptors.h"
#include "include/usb_config_manager.h"
#include "include/audio_route_control.h"
#include "include/hil_control.h"
#include "BleCompanion.hpp"
#include "companion_sysex.h"
#include "host_info.h"
#include "usb_host_profile.h"
#include "include/fw_updater.h"
#include "include/cp_account.h"
#include "include/cp_remote.h"
#include "instrument_host.h"
#include "wifi_creds.h"
#include "wifi_link.h"
#include "include/crosspad_deps.h"
#include "include/reset_log.h"
#include "include/power_button.h"
#include "include/lv_async_safe.h"
#include "include/file_transfer.h"
#include "include/usb_transfer.h"
#include "include/net_mode.h"
#include "include/fw_slots.h"
#include "include/fw_assets_sync.h"

#include "bsp/esp-bsp.h"

#include "bsp_sdmmc.h"
#include "sdmmc_cmd.h"

#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_wifi.h"
#include "esp_mac.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "espnow.h"
#include "espnow_utils.h"
#endif

#include <MIDI.h>
#include "EspTinyUsbMidiTransport.h"

#include "esp_vfs_fat.h"
#include "esp_littlefs.h"
#include "stm32_fw_update.h"
#include "stm_fw_overlay.h"

static stm32_fw_update_status_t s_stm_fw_status;

#include "CrossPad.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "CrosspadPlatform.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "crosspad/led/ILedStrip.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "stm32_pwr_sync.h"   // STM32 power-state IRQ callback (set_callback / ack)
#include "stm32_i2c.h"        // Early wake qualification reads
#include "stm32_reg_map.h"
#include <atomic>    // PWR_STATE_* constants
#include "stm32_hal_init.h"   // charger telemetry (SoC gauge for the status bar)
#include "crosspad/kit/KitFolders.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad-sampler/logic/kit_gate.hpp"
// CrosspadSettings singleton instance is managed via static inline in crosspad-core
CrosspadSettings* settings = CrosspadSettings::getInstance();
CrosspadStatus status;

static const char *TAG = "crosspad_main";

#ifdef USE_LVGL
// Single definition point for the LVGL input-device groups/drivers declared
// extern in display.h. display.h is pulled into many BSP translation units
// through crosspad_s3.h, so defining them there was a tentative definition
// in each — the linker only got away with it by merging them as common
// symbols.
lv_group_t *default_group = nullptr; // currently not used
lv_group_t *keypad_group = nullptr;
lv_group_t *encoderLeft_group = nullptr;
lv_group_t *encoderTop_group = nullptr;

lv_indev_t *indev_keypad = nullptr;
lv_indev_t *indev_encoderLeft = nullptr;
lv_indev_t *indev_encoderTop = nullptr;
lv_indev_t *indev_touchpad = nullptr;
#endif

#include "display.h"
#ifdef USE_LVGL
#include "crosspad-gui/components/ui_toast.h"
#endif
#include "processMIDI.hpp"
#include "ota_cdc.h"
#include "cdc_line.h"
#include "charge_overlay.h"   // charge-while-off pad gauge + "Charging" screen
#include "app/settings/settings_app.h"
#include "sample_player_port.hpp"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "include/usb_audio_bridge.h"
#endif
#ifdef BSP_BOARD_CROSSPAD_S3
#include "audio_task.h"
#include "performance_monitor.h"
#include "midi_clock.h"
#include "motion_stream.h"
#endif


#if CONFIG_CROSSPAD_SAMPLER_MARCEL
extern void smpl_stream_ply_test(void);
#else
#include "engine_arena.hpp"
#include "SamplerEnginePort.hpp"
#endif


#include "include/boot_power.h"
#include "include/cdc_rx.h"

#define BASE_PATH "/usb" // base path to mount the partition
#define LOG_MEM_INFO    (1)

/* Cleared while a kit scan is in flight; see hil_control.h. */
std::atomic<bool> g_kit_scan_done{false};
std::atomic<bool> g_sd_probed{false};

/* What the IMU recognised goes on the event bus, where any app can subscribe to
 * it without knowing there is an LSM6DS3TR-C behind it. Runs in the IMU task;
 * the bus is asynchronous, so subscribers are dispatched off it. */
static void imu_post_gestures(uint32_t events, uint8_t orientation_6d)
{
    auto& bus = crosspad::getEventBus();
    static const struct { uint32_t bit; crosspad::MotionGesture gesture; } kMap[] = {
        {BSP_IMU_EVENT_SINGLE_TAP,  crosspad::MotionGesture::SingleTap},
        {BSP_IMU_EVENT_DOUBLE_TAP,  crosspad::MotionGesture::DoubleTap},
        {BSP_IMU_EVENT_FREE_FALL,   crosspad::MotionGesture::FreeFall},
        {BSP_IMU_EVENT_WAKE_UP,     crosspad::MotionGesture::WakeUp},
        {BSP_IMU_EVENT_ORIENTATION, crosspad::MotionGesture::Orientation},
        {BSP_IMU_EVENT_STILL,       crosspad::MotionGesture::Still},
    };
    for (const auto& m : kMap) {
        if (events & m.bit) bus.postMotionGesture(m.gesture, orientation_6d);
    }
}

/* Picking the board up wakes it from LIGHT sleep the way a touch does: the
 * IMU's wake-up event (motion over its threshold) tells the STM there was user
 * activity. Only in LIGHT -- an awake board does not need it, and a board being
 * played shakes all the time. Runs in the IMU task. */
static void imu_event_cb(uint32_t events, uint8_t orientation_6d, void *)
{
    imu_post_gestures(events, orientation_6d);
    motion_stream_event(events, orientation_6d);
    if (!(events & BSP_IMU_EVENT_WAKE_UP) || g_hil_last_pwr_state.load() != PWR_STATE_LIGHT) return;
    static int64_t s_last_us = 0;
    const int64_t now = esp_timer_get_time();
    if (now - s_last_us < 2000000) return;   // one wake request per motion burst
    s_last_us = now;
    ESP_LOGI(TAG, "motion in LIGHT sleep: waking the board");
    stm32_i2c_write_u8(REG_CMD_USER_ACTIVITY, 1);
}

/* The slowest pad_anim run since the last WORKER verb (hil_control.cpp prints
 * and clears it): its length, then its parts -- the pad frame (the LED strip
 * lock and an RMT frame), the 1 Hz report lines (printf on UART0, a busy
 * wait), the STM telemetry (three I2C reads on the bus the codecs and the
 * IMU share) -- and when it ended, on the "I (ms)" clock. Wall clock: a task
 * that preempts the worker shows in the part it interrupted. */
EXT_RAM_BSS_ATTR uint32_t g_padanim_worst[5];

/* Worker job, every 10 ms: pad LED animation, and the battery gauge at 1 Hz. */
static uint32_t pad_anim_job(void *)
{
    static int bat_div = 0;
    const int64_t t0 = esp_timer_get_time();
    crosspad::getPadManager().update();
    charge_overlay_tick();   // re-samples battery ~1 Hz while charge-idling
    const int64_t t1 = esp_timer_get_time();
    int64_t t2 = t1;

    /* Feed the status-bar battery icon from the STM SoC estimator (~1 Hz).
     * On r20 nothing else populates status.batteryPct (the r19 LPUART
     * voltages message is not sent), so this is the gauge's only source.
     * The EKF SoC is IR-drop-compensated — a naive VBAT curve reads far too
     * low under load. Fall back to leaving batteryPct untouched on old
     * firmware (invalid flag) so the icon simply does not update.
     * No battery / low battery come from the STM's own flags: the old guess
     * (VBAT < 3200 mV = no pack) also fired on a nearly empty real cell, and
     * a missing pack reads as a charged one at the BAT pin. */
    if (++bat_div >= 100) {   // 100 * 10 ms ~= 1 s
        bat_div = 0;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        sample_player_port_report_errors();
#endif
#ifdef CONFIG_IDF_TARGET_ESP32S3
        usb_audio_bridge_report();   // 'SPK #', off the UAC tasks
#endif
        t2 = esp_timer_get_time();
        stm32_charger_telemetry_t bt;
        if (stm32_hal_get_charger_telemetry(&bt) == ESP_OK) {
            if (bt.soc_flags & REG_SOC_FLAG_NO_BATTERY) {
                status.batteryPct = -1;   // no battery pack present
            } else if ((bt.soc_flags & REG_SOC_FLAG_VALID) && bt.soc_pct <= 100) {
                status.batteryPct = (int8_t)bt.soc_pct;
            }
            status.batteryLow = (bt.soc_flags & REG_SOC_FLAG_LOW_BATTERY) != 0;
            status.stm32.chargingStatus =
                (bt.flags & REG_CHG_FLAG_PG) &&
                (bt.chrg_stat == 1 || bt.chrg_stat == 2);
            /* A charger plugged in while on: the charge screen for a few
             * seconds. Seeded as plugged, so a boot on USB shows nothing. */
            static bool s_prev_pg = true;
            const bool pg = (bt.flags & REG_CHG_FLAG_PG) != 0;
            if (pg && !s_prev_pg && status.isGuiInitialised) charge_overlay_peek();
            s_prev_pg = pg;
        }
    }
    const int64_t t3 = esp_timer_get_time();
    if ((uint32_t)(t3 - t0) > g_padanim_worst[0]) {
        g_padanim_worst[0] = (uint32_t)(t3 - t0);
        g_padanim_worst[1] = (uint32_t)(t1 - t0);
        g_padanim_worst[2] = (uint32_t)(t2 - t1);
        g_padanim_worst[3] = (uint32_t)(t3 - t2);
        g_padanim_worst[4] = esp_log_timestamp();
    }
    return 0;
}


/* SD hotplug poll — the STM32 detection can be stale, and there is no detect
 * pin, so presence is probed. sdmmc_get_status() issues CMD13, which fails at
 * once when the card is gone but takes the SDMMC transaction lock to do it:
 * behind a kit load streaming samples off the same card, one probe blocked the
 * CDC loop for up to 3.1 s (CDC_STATS maxgap_us), which is how a KIT_STATUS
 * asked mid-swap timed out. Its own task, so a probe that waits delays nothing
 * but the next probe. */
/* Worker job: probe the card every second, back off 1 s -> 30 s while there
 * is none. The rescan after an insert is a long SD walk, so it stays a
 * one-shot task rather than a job.
 * The rescan opens every kit.json, and it does not start while the WiFi radio
 * is on: a card that mounted late, under the boot update check's lease, ran
 * its rescan beside the radio, and newlib aborted on a FILE lock it could not
 * have -- the boot kit_scan panic again, in kit_rescan (2026-10-30). It waits
 * for the radio to stop, the list reading as scanning meanwhile, which also
 * keeps the update checks from taking a lease (fw_updater.cpp). */
static void start_kit_rescan()
{
    if (xTaskCreatePinnedToCore([](void*) {
        auto* kitLoader = static_cast<crosspad::PortableKitLoader*>(crosspad::getKitManager());
        if (kitLoader) {
            kitLoader->scanKitFolders({std::begin(crosspad::KitFolders::kAll), std::end(crosspad::KitFolders::kAll)});
            ESP_LOGI("KitLoader", "Rediscovered %d kits after SD reinsert", kitLoader->getKitCount());
        }
        g_kit_scan_done.store(true);
        /* Headless (lvgl_port_init failed) there is no LVGL to queue on:
           lv_async_call_locked would assert in lvgl_port_lock. */
        if (status.isGuiInitialised) lv_async_call_locked(crosspad_sampler::kit_gate_refresh_if_open, nullptr);
        vTaskDelete(nullptr);
    }, "kit_rescan", 8192, nullptr, 1, nullptr, 0) != pdPASS) {
        g_kit_scan_done.store(true);
    }
}

static uint32_t sd_hotplug_job(void *)
{
    static uint32_t retryDelayMs = 1000;
    static bool rescanPending = false;
    sdmmc_card_t* card = bsp_sdcard_get_handle();
    if (card != nullptr) {
        if (sdmmc_get_status(card) != ESP_OK) {
            bsp_sdcard_unmount();
            status.sdCardDetected = false;
            retryDelayMs = 1000;   // card yanked — probe fast again
            if (rescanPending) {   // nothing left to scan: the old list stands, as after any removal
                rescanPending = false;
                g_kit_scan_done.store(true);
            }
        }
    } else if (bsp_sdcard_mount() == ESP_OK) {
        status.sdCardDetected = true;
        retryDelayMs = 1000;
        /* Cleared here, not in the task: a KIT_LIST between the create and
           the task's first instruction would otherwise read the vector the
           rescan is about to grow. */
        g_kit_scan_done.store(false);
        rescanPending = true;
        if (!wifi_link_radio_idle()) ESP_LOGI(TAG, "card mounted: kit rescan waits for the WiFi radio to stop");
    } else {
        // Each failed mount is a full SDMMC init + 2 log lines.
        retryDelayMs = (retryDelayMs < 30000) ? retryDelayMs * 2 : 30000;
    }
    g_sd_probed.store(true);
    /* fw_updater_busy(): a job started a moment ago has not taken its lease yet. */
    if (rescanPending && wifi_link_radio_idle() && !fw_updater_busy()) {
        rescanPending = false;
        start_kit_rescan();
    }
    return bsp_sdcard_get_handle() != nullptr ? 1000 : retryDelayMs;
}

/* What kind of boot this is. The two one-shot flags are read first; the rest
 * is learned on the way and written by the stage that learns it. */
struct Boot {
    bool transferMode = false;   // the card is the host's (usb_transfer.h); no engine, no kits, no BLE
    bool netMode = false;        // WiFi has the RAM (net_mode.h); no engine, no kits, no BLE
    bool chargeMode = false;     // woken by a charger while off; known past the wake gate
    bool headless = false;       // no display came up
    bool recoveryHeld = false;   // pad 16 through the boot, acted on once the card is up
    /* An ordinary boot: the launcher, the engines, the radios. */
    bool normal() const { return !transferMode && !netMode && !chargeMode; }
};

/* The card's bounce buffer, NVS, the firmware slots and the settings every later
 * stage reads. */
static void boot_storage_and_settings(void)
{
    /* The card's sector bounce, while internal RAM is still whole. */
    sd_bounce_init();

#ifdef BSP_BOARD_CROSSPAD_S3
    // Pulse the LCD/touch reset now: the panel's post-reset settle then runs
    // under the NVS, USB and STM32 bring-up instead of as its own wait.
    bsp_display_reset_early();
#endif

    NvsSettingsStore::getInstance().init();
    fw_slots_init();
    settings->loadFrom(NvsSettingsStore::getInstance());
    {
        /* After a Store update the other slot holds the image we came from. */
        FwSlotInfo slots[2];
        fw_slots_get(slots);
        crosspad::output_levels_migrate(slots[slots[0].running ? 1 : 0].version);
    }

    // Early, so overlays can persist changes before Settings is ever opened.
    RegisterSettingsUIPlatform();
}

/* The queue the command loop drains, and the three sources that wake it. */
static void boot_command_queue(void)
{
    /* 64 x 136 B: 9 kB, in PSRAM. The producer is the TinyUSB task's CDC
     * callback (a memcpy, no ISR), the consumer the main loop; neither needs
     * this storage internal, and BLE does. */
    app_queue = xQueueCreateWithCaps(64, sizeof(app_message_t), MALLOC_CAP_SPIRAM);
    assert(app_queue);
    /* A BLE companion command wakes this loop through the same queue: an
     * empty message drains as a no-op and hil_control_ble_poll() at the top
     * of the next pass answers it, instead of after the periodic timeout. */
    crosspad::bleCompanionSetWake([] {
        app_message_t wake{};
        xQueueSend(app_queue, &wake, 0);
    });
    companion_sysex_set_wake([] {
        app_message_t wake{};
        xQueueSend(app_queue, &wake, 0);
    });
    cp_remote_set_wake([] {
        app_message_t wake{};
        xQueueSend(app_queue, &wake, 0);
    });
}

/* USB in the profile this boot runs in. From here every restart stops USB DMA
 * first. */
static void boot_usb(Boot &boot)
{
    usb_host_profile_init();   // before the first SOF the feedback is shaped for
    ESP_ERROR_CHECK(usb_config_manager_init(boot.transferMode ? USB_MODE_TRANSFER
                                                         : USB_MODE_DEFAULT));
#ifdef BSP_BOARD_CROSSPAD_S3
    /* From here on every esp_restart() -- OTA, slot switch, transfer and
     * network mode, soft-off, bootloader -- stops USB DMA before the next
     * image is loaded. A panic's restart does not run shutdown handlers. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_register_shutdown_handler(usb_otg_hw_reset));
#endif
}

/* crosspad-core's singletons and the services that hang off them. Needs the
 * settings. */
static void boot_platform(void)
{
    settings->stmRouting.SetUsbPassThru(true);

    crosspad::CrosspadPlatform_Init(settings, &status);
    host_info_init();
    cp_account_init();
    fw_updater_init();
    wifi_creds_init();
    instrument_host_init();                  // an app may borrow the pitched engine

    midiCallbacks_init();
    crosspad::getPadManager().begin();
    hil_pad_inject_init();
}

/* The assets partition. Ahead of the STM link: the bundled STM image lives here. */
static void boot_assets_fs(void)
{
    // Ahead of the STM link: the bundled STM image lives here.
    ESP_LOGI(TAG, "Initializing LittleFS");
    esp_vfs_littlefs_conf_t lfs_conf = {
        .base_path = "/assets",
        .partition_label = "assets",
        .partition = NULL,
        .format_if_mount_failed = true,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };
    ESP_ERROR_CHECK(esp_vfs_littlefs_register(&lfs_conf));
}

/* The STM32 link, the wake gate, and what only starts after an intentional wake.
 * A charge boot is known only past the gate; it and the recovery pad are
 * written into the boot. */
static void boot_stm32_and_wake(Boot &boot)
{
#ifdef BSP_BOARD_CROSSPAD_S3
    // Black frame before reading the STM state, so an ESP-only USB reset
    // cannot light the pads while the STM is still in soft-off.
    force_pad_strip_dark();
    // bsp_i2c before bsp_stm32_init: stm32_hal_init() probes the rev2 I2C link.
    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_i2c_init());
    ESP_ERROR_CHECK(bsp_stm32_init());       // GPIO + UART (+ I2C link on rev2)
    // In-field STM reflash. With a display the decision is the user's
    // (stm_fw_overlay, once the launcher is up); headless updates here.
    stm32_fw_update_check(STM32_FW_BUNDLED_PATH, &s_stm_fw_status);
    crosspad_deps_check_runtime();           // linked core/gui vs headers, STM proto vs reg_map
    midi_clock_init();                       // after the STM UART: it sends every clock tick there
    reset_log_init();                        // record why we booted, from both sides of the link
    // Blank the retained framebuffer before the wake gate and LVGL startup.
    crosspad::getStm32CommandSender().sendBrightness(0);
    // ESP drives backlight + ARGB across the STM's low-power transitions.
    stm32_pwr_sync_set_callback(app_pwr_state_cb, nullptr);
    qualify_power_button_wake();
    boot.recoveryHeld = recovery_pad_held();
    // Background services only after an intentional wake.
    crosspad::CrosspadPlatform_StartStm32(); // Start Stm32Manager/config sync
    crosspad::CrosspadPlatform_StartVibration(); // Probe + start DRV2605 haptics (async task)
    ESP_ERROR_CHECK(bsp_worker_start());
    if (bsp_ulp_start() != ESP_OK) {
        ESP_LOGW(TAG, "ULP coprocessor not started");
    }
    bsp_worker_add("softoff", 40, softoff_job, nullptr);
    ESP_LOGI(TAG, "  - STM32 UART2 communication: Ready");
#else
    ESP_LOGI(TAG, "  - STM32: not present on this board");
#endif
    /* Past the wake gate: a charge boot is known only there. */
    boot.chargeMode = charge_boot_active();
    cp_remote_init(boot.normal() && CROSSPAD_FW_RADIO);   // none of these boots holds a remote session
#if CONFIG_TINYUSB_CDC_COUNT && CONFIG_TINYUSB_CDC_ENABLED
    ESP_LOGI(TAG, "  - USB CDC ACM interface: Ready");
#endif
#if CONFIG_TINYUSB_MIDI_COUNT
    ESP_LOGI(TAG, "  - USB MIDI interface: Ready");
#endif

    // BLE MIDI auto-start moved past the launcher (see the app loop below):
    // with BT_CTRL_RUN_IN_FLASH_ONLY=y (sdkconfig.v2) nimble_port_init() works
    // fine post-boot, so a board with BLE switched off no longer spends any
    // internal RAM or boot time on it, and a board with it on gets the same
    // bleMidi_start() the Settings toggle uses, just deferred until there is
    // something running to hand it to.
}

/* The board I2C bus and whether an IMU answers on it. */
static void boot_imu_probe(void)
{
    ESP_LOGI(TAG, "Crosspad initialization complete");
    ESP_LOGI(TAG, "Starting application...");

    ESP_ERROR_CHECK_WITHOUT_ABORT(bsp_i2c_init());

#ifdef BSP_BOARD_CROSSPAD_S3
    ESP_LOGI(TAG, "Initializing IMU...");
    esp_err_t imu_ret = bsp_imu_init();
    if (imu_ret == ESP_OK) {
        ESP_LOGI(TAG, "IMU initialized successfully");
        status.imuDetected = true;

    } else {
        ESP_LOGW(TAG, "IMU initialization failed: %s", esp_err_to_name(imu_ret));
        status.imuDetected = false;
    }
#else
    status.imuDetected = false;
#endif
}

/* The card, and the firmware work that needs it and NVS both up: a pending slot
 * switch, recovery, the assets of the running slot. Before the display hands
 * LVGL an image and before the engine competes for PSRAM. */
static void boot_card(Boot &boot)
{
    /* In transfer mode the card belongs to the host, raw — a FATFS mount here
     * would be the second writer on one volume. usb_transfer_storage_attach()
     * already brought it up from the USB install. */
    if (!boot.transferMode) {
        status.sdCardDetected = (bsp_sdcard_mount_ex(true) == ESP_OK);
        // A pending FW_SWITCH is resolved here: NVS and the SD card (the
        // settings.json a switch may have exported) are both up, and nothing
        // below yet reads a per-firmware setting such as Kit — kit discovery,
        // the audio engine and the LVGL/GUI task all start after this.
        fw_switch_finish_boot();
        // After the mount: the switch exports the leaving firmware's settings
        // to its package on the card. app_main's stack is internal RAM, which
        // the otadata write needs.
        if (boot.recoveryHeld) {
            const char *why = nullptr;
            if (fw_recovery_switch(&why)) {
                ESP_LOGW(TAG, "recovery: restarting into the main firmware");
                esp_restart();
            }
            ESP_LOGW(TAG, "recovery not done: %s", why ? why : "?");
        }
        // The `assets` LittleFS partition is shared by both OTA slots: a
        // switch into a slot built with a different asset set left the old
        // icons/fonts in flash until this rewrites them from the running
        // slot's own package on the card. Before bsp_display_start() ever
        // hands LVGL an image, and before the ~1 MB staging buffer this
        // needs competes with the sample engine's kit for PSRAM.
        if (fw_assets_sync() == FwAssetsResult::Rewritten) {
            // The bundled STM32 image lives in /assets, which the rewrite just
            // replaced with the running firmware's own — the decision above was
            // taken against the previous firmware's bundle.
            stm32_fw_update_check(STM32_FW_BUNDLED_PATH, &s_stm_fw_status);
        }
        // Up to ~4.4 s of erase and write just went by inside a trial boot's
        // 30 s window, before anything can confirm.
        fw_trial_watchdog_feed();
    }
}

/* The display. Without one the boot is headless. */
static void boot_display(Boot &boot)
{
#ifdef BSP_BOARD_CROSSPAD_S3
    bsp_display_brightness_set(0);
#endif
    lv_display_t* disp = bsp_display_start();
    boot.headless = (disp == NULL);
    if (boot.headless) {
        ESP_LOGW(TAG, "Display unavailable - running headless (MIDI/pads only)");
    }
}

/* The IMU task (after the display: its touch driver owns the GPIO ISR service)
 * and the pads' first picture. */
static void boot_imu_task_and_pads(Boot &boot)
{
#ifdef BSP_BOARD_CROSSPAD_S3
    /* After the display: its touch driver owns the GPIO ISR service the IMU's
     * data-ready line hangs off (bsp_imu.cpp, attach_drdy_line). */
    if (status.imuDetected) {
        const esp_err_t imu_ret = bsp_imu_start_task(20, 5, 3096);   // 20 ms poll only without INT2
        bsp_imu_set_event_callback(imu_event_cb, nullptr);
        if (imu_ret != ESP_OK) {
            ESP_LOGW(TAG, "IMU task start failed: %s", esp_err_to_name(imu_ret));
        }
        /* Board motion as MIDI, per the persisted setting. Starts nothing when
         * the setting is off, which is the default — a feature nobody enabled
         * must not hold a stack on a board this short of internal RAM. */
        imu_midi_init();
    }
#endif


#ifdef USE_LVGL
    // With a screen the splash is the pads' first picture: black until its
    // first frame, or the controller's default green flashed in between.
    if (!boot.headless) {
        const crosspad::RgbColor dark[crosspad::PadAnimator::kPadCount] = {};
        crosspad::getPadAnimator().showFrame(dark);
    }
#endif
    // The pads come back as bright as they were left, like the screen.
    crosspad::getPadLedController().setBrightness(settings->RGBbrightness);

    /* From here a repaint only marks the strip dirty and pad_anim_job sends
     * it, one frame per 10 ms at most. Sent at once, a frame is ~0.74 ms of
     * RMT under the strip's mutex: the engine's note callbacks paid that on
     * audio_rt, and waited for the frames LVGL and crosspad_events were
     * sending. */
    crosspad::getPadLedController().setCoalescedRefresh(true);
    bsp_worker_add("pad_anim", 10, pad_anim_job, nullptr);

    // The boot splash drives the pads along with the screen (boot_splash.cpp);
    // a qualified wake's hold-progress (built only without LVGL) makes way
    // for the kit's colours.
#ifdef BSP_BOARD_CROSSPAD_S3
    if (s_wake_hold_qualified) crosspad::getPadAnimator().stop(s_wake_hold_anim);
#endif
}

/* The STM link task and, with a display, the LVGL task. */
static void boot_link_and_gui_tasks(Boot &boot)
{
    xTaskCreatePinnedToCoreWithCaps(uartLoop, "UART drv", 4000, NULL, 6, NULL, 0,
                                    MALLOC_CAP_SPIRAM);   // STM link, no flash

    if (!boot.headless) {
        xTaskCreatePinnedToCore(LVGLTask, "LVGL drv", 5500, NULL, 4, NULL, 0);
    }
}

/* A trial boot confirms itself once it has stayed up. Past bsp_stm32_init() and
 * the LVGL task's creation. */
static void boot_confirm_timer(void)
{
    // Confirm this boot once it has actually stayed up — a fresh
    // install-and-switch is still ESP_OTA_IMG_PENDING_VERIFY at this point
    // (see fw_slots.h) and the bootloader rolls back to the previous slot on
    // its own if nothing ever confirms it. Armed here, past both bsp_stm32_init()
    // (ESP_ERROR_CHECK above — a board that never got that far never reaches
    // this line either) and the LVGL task's own creation. Periodic, not
    // one-shot: fw_confirm_running() can fail to create its write task when
    // internal RAM is tight, and a dropped one-shot left the image
    // unconfirmed until the 30 s trial watchdog rolled a good slot back. The
    // confirm task stops and deletes this timer once there is nothing left to
    // confirm, which is on its first tick when this boot was not a trial.
    static constexpr uint32_t kConfirmDelayMs = 10000;
    static const esp_timer_create_args_t kConfirmTimerArgs = {
        .callback = [](void *) { fw_confirm_running(); },
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "fw_confirm",
        .skip_unhandled_events = false,
    };
    esp_timer_handle_t confirmTimer;
    if (esp_timer_create(&kConfirmTimerArgs, &confirmTimer) == ESP_OK) {
        fw_confirm_set_timer(confirmTimer);
        esp_timer_start_periodic(confirmTimer, (uint64_t)kConfirmDelayMs * 1000);
    }
}

/* The sample engine's arena and the start-up sound. Before the kit list and
 * before BLE. */
static void boot_sample_engine(Boot &boot)
{
    if (boot.normal()) {
        /* Before BLE takes its block and before the first preload reads. */
        sample_player_port_stage_init();
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        smpl_stream_ply_test();
#else
        /* The arena is the native engine's from boot: taken here, before the
         * kit list and BLE, as the prebuilt streamer's Init() took it -- the
         * scan below is sized against what is left outside it. Nothing goes
         * into it until a sampler kit's load ends in native_sampler_take_kit()
         * (or a pitched kit builds its bank) -- but the start-up sound, which
         * the native engine's audition voice plays from crosspad/STARTUP.wav
         * as the prebuilt streamer's preview did: one 64 kB page of the
         * arena for its ring, handed back by the first kit load. */
        /* A kit without the sample engine (its profile's "sample_engine", a
         * game) leaves the arena's 4.6 MB to its app. */
        if (CROSSPAD_FW_SAMPLE_ENGINE) {
            engine_arena_init();
            native_sampler_boot_sound("/sdcard/crosspad/STARTUP.wav");
        }
#endif
    }
}

/* The kit list, after the mount and once the launcher is up. On a task of its
 * own: the recursive scan needs more stack than the main task has. */
static void boot_kit_scan(Boot &boot)
{
    // The kit list is a few hundred kB of PSRAM. Scanned before the sample
    // engine had its arena it collided with that allocation and the splash
    // decode -- bad_alloc in listKits(), boot loop. After the engine the
    // headroom is what the launcher runs with; the splash is still decoded and
    // cached at that point, though, and vector growth needs one contiguous
    // block of capacity * sizeof(KitInfo) (1 kB per kit as of core 1.2.0, 128
    // kB at 76 kits). That margin was thin enough that 40 bytes per kit
    // brought it down: std::bad_alloc out of discoverKitsRecursive, abort on
    // core 1, boot loop with a large card. The copy listKits() returned on
    // top of that -- discarded here -- was the next thing to fail (80 kits,
    // phone on the USB port), hence scanKits().
    // So wait for the launcher, which is where LoadMainScreen() drops LVGL's
    // image cache and hands back the splash. Bounded: headless never paints,
    // and a display that never comes up must not cost the kit list.
    /* Network mode loads no kit: nothing there plays one, and the list costs
     * PSRAM the network stack would rather have. */
    if (status.sdCardDetected && !boot.netMode && !boot.chargeMode && CROSSPAD_FW_SAMPLE_ENGINE) {
        if (xTaskCreatePinnedToCore([](void* arg) {
            const bool noGui = (arg != nullptr);
            for (int i = 0; !noGui && !status.isGuiInitialised && i < 100; ++i) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            auto* kitLoader = static_cast<crosspad::PortableKitLoader*>(crosspad::getKitManager());
            if (kitLoader) {
                kitLoader->scanKitFolders({std::begin(crosspad::KitFolders::kAll), std::end(crosspad::KitFolders::kAll)});
                ESP_LOGI("KitLoader", "Discovered %d kits", kitLoader->getKitCount());
            }
            /* Headless takes the same path, only without the wait — the list is
               equally unreadable while it is being built. */
            g_kit_scan_done.store(true);
            if (!noGui) lv_async_call_locked(crosspad_sampler::kit_gate_refresh_if_open, nullptr);
            vTaskDelete(nullptr);
        }, "kit_scan", 8192, boot.headless ? (void*)1 : nullptr, 2, nullptr, 1) != pdPASS) {
            g_kit_scan_done.store(true);
        }
    } else {
        /* No card (or no sample engine): the list is empty and final, so
           KIT_* must not wait for a scan that will never run. */
        g_kit_scan_done.store(true);
    }
}

/* The audio tasks and, once the codecs are up, the stored USB profile. */
static void boot_audio(Boot &boot)
{
    /* Transfer mode starts no engine: the arena stays unallocated, the RT task
     * has no card to stream from, and the profile it enumerated has no audio.
     * Network mode neither: its internal RAM belongs to WiFi. Nor a kit
     * whose profile says "audio": false (its app plays nothing). */
    if (boot.normal() && CROSSPAD_FW_AUDIO) AudioTasks_Start();

    /* Persisted USB profile, and USB Audio is the default: the instrument is
     * an audio device first. It has no CDC endpoint, so a board that has never
     * been told otherwise answers the host tools only over SysEx on USB MIDI
     * (`crosspad-hil usb-mode set default`, which persists).
     * The audio bridge takes the I2S handles, and those exist only once the RT
     * task has run its setup — applied any earlier, the switch failed at every
     * boot ("I2S0 not initialized") and the board fell back to CDC. So wait for
     * the codecs, bounded like the splash gate; a board whose audio never comes
     * up stays reachable over CDC, and its stored choice is left alone.
     * Network mode has no engine, so no I2S for the bridge: it stays on CDC
     * the same way. */
    if (settings->usbAudioEnabled && boot.normal() && CROSSPAD_FW_AUDIO) {
        constexpr TickType_t kAudioReadyMaxMs = 2500;
        const TickType_t waitStart = xTaskGetTickCount();
        while (!AudioTasks_IsReady()
               && (xTaskGetTickCount() - waitStart) < pdMS_TO_TICKS(kAudioReadyMaxMs)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (AudioTasks_IsReady()) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(usb_config_manager_set_mode(USB_MODE_AUDIO));
        } else {
            ESP_LOGE(TAG, "Audio not ready after %u ms, staying in the CDC profile",
                     (unsigned)kAudioReadyMaxMs);
        }
    }
}

/* OTA over CDC, its overlay, and an STM32 update that is due. */
static void boot_ota_and_overlays(Boot &boot)
{
#ifdef CONFIG_IDF_TARGET_ESP32S3
    espnow_storage_init();
#endif

    ota_init();

#ifdef USE_LVGL
    /* Transfer mode's screen is the whole UI; an overlay over it would offer a
     * firmware update that reads the assets partition mid-transfer. */
    if (!boot.headless && !boot.transferMode) {
        ota_overlay_init();
        /* An STM reflash is a normal boot's decision; network mode is a short visit. */
        if (!boot.netMode && !boot.chargeMode && (s_stm_fw_status.due || s_stm_fw_status.pcb_mismatch)) {
            stm_fw_overlay_show(&s_stm_fw_status);   /* also raised on a PCB mismatch: says so, offers nothing */
        }
    }
#endif
    if (boot.headless && boot.normal() && s_stm_fw_status.due) {
        // No screen: the pads are the progress bar, as they are for ESP OTA.
        auto& anim = crosspad::getPadAnimator();
        anim.startProgress();
        bool ok = stm32_fw_update_auto(STM32_FW_BUNDLED_PATH,
            [](uint16_t done, uint16_t total, void *) {
                crosspad::getPadAnimator().setProgress((float)done / (float)total);
            }, nullptr);
        if (ok) anim.playSuccess(); else anim.playError();
    }
    ESP_LOGI(TAG, "All systems operational - Crosspad ready");
}

/* The card hotplug probe, and LVGL calls from other tasks. */
static void boot_card_hotplug(Boot &boot)
{
    /* The hotplug probe mounts the card it finds — never while the host has it —
     * and rescans kits, which network mode does not load. */
    /* A boot that mounted no card holds its update check until the first probe
     * (fw_updater.cpp): a card a second late then scans before the radio starts. */
    if (status.sdCardDetected) g_sd_probed.store(true);
    if (boot.transferMode || boot.netMode || boot.chargeMode || !bsp_worker_add("sd_hotplug", 1000, sd_hotplug_job, nullptr))
        g_sd_probed.store(true);
    lv_async_safe_start();
}

/* Drains the CDC command queue, runs the launcher's one-shots, and nothing else.
 * Anything that can block belongs on another task: this loop is what answers
 * every off-device query. */
static void command_loop(Boot &boot)
{
    /* Main application loop — drains the CDC command queue and nothing else.
     * Anything that can block belongs on another task: this loop is what
     * answers every off-device query. */
    app_message_t msg;
    while (1) {
        hil_control_ble_poll();
        companion_sysex_poll();
        cp_remote_poll();
// One-shot: bring BLE MIDI up, per the persisted setting, once the
        // device is actually usable — with a display, once the launcher has
        // painted (status.isGuiInitialised); headless, at once, since there
        // is no launcher to wait for. Riding this loop's existing cadence
        // avoids spinning up a task just to poll one flag; bleMidi_boot()
        // is the same enableBLE check + bleMidi_start() the early boot path
        // used to run, just no longer ahead of the display/LVGL/audio
        // allocations it used to compete with for internal RAM.
        // Never in transfer or network mode: nothing there sends MIDI, and the
        // radio would take internal RAM the MSC path / WiFi needs.
        static bool s_bleAutoStartDone = false;
        if (!s_bleAutoStartDone && boot.normal() && (boot.headless || status.isGuiInitialised)) {
            s_bleAutoStartDone = true;
            bleMidi_boot();
        }

        // Update checks start once the launcher has painted, like BLE above:
        // the boot check (when enabled) and the daily idle watch. Normal boots
        // only -- network mode has no launcher to be idle in.
        static bool s_fwUpdaterStarted = false;
        if (!s_fwUpdaterStarted && boot.normal() && status.isGuiInitialised) {
            s_fwUpdaterStarted = true;
            fw_updater_launcher_ready();
        }

#ifdef USE_LVGL
        // The other slot rolled back on its own since the last boot
        // (fw_slots_init() found it and queued this) — one toast, once the
        // launcher has actually painted. Off the LVGL thread here, so the
        // call into LVGL goes through the same locked forwarder BLE/CDC
        // handlers use, never lv_async_call() directly.
        static bool s_fwRollbackToastDone = false;
        if (!s_fwRollbackToastDone && !boot.transferMode && !boot.chargeMode && status.isGuiInitialised) {
            s_fwRollbackToastDone = true;
            static char s_fwRollbackToastMsg[160];
            char failedPkg[FW_PKG_NAME_LEN];
            char runningPkg[FW_PKG_NAME_LEN];
            if (fw_rollback_toast_pending(failedPkg, sizeof(failedPkg), runningPkg, sizeof(runningPkg))) {
                snprintf(s_fwRollbackToastMsg, sizeof(s_fwRollbackToastMsg),
                         "%s did not start - still on %s",
                         failedPkg[0] ? failedPkg : "the other firmware",
                         runningPkg[0] ? runningPkg : "this one");
                lv_async_call_locked([](void *data) {
                    ui_toast_show(static_cast<const char *>(data), 3000, UI_TOAST_WARNING);
                }, s_fwRollbackToastMsg);
            }
        }

        // fw_assets_sync() found no matching assets.bin on the card for the
        // slot booted this time — one toast, once the launcher has painted.
        static bool s_fwAssetsToastDone = false;
        if (!s_fwAssetsToastDone && !boot.transferMode && !boot.chargeMode && status.isGuiInitialised) {
            s_fwAssetsToastDone = true;
            static char s_fwAssetsToastMsg[128];
            if (fw_assets_sync_toast_pending(s_fwAssetsToastMsg, sizeof(s_fwAssetsToastMsg))) {
                lv_async_call_locked([](void *data) {
                    ui_toast_show(static_cast<const char *>(data), 3000, UI_TOAST_WARNING);
                }, s_fwAssetsToastMsg);
            }
        }
#endif

        // Wake at least once a second to report queue drops and to run the
        // stream stall checks below.
        const TickType_t timeout = pdMS_TO_TICKS(1000);
        static int64_t s_lastDrainUs = 0;
        static bool    s_queueWasBacked = false;
        if (xQueueReceive(app_queue, &msg, timeout)) {
            /* Only meaningful when something was already waiting: otherwise the
             * "gap" is just how long the sender took to send. */
            if (s_queueWasBacked && s_lastDrainUs) {
                const uint32_t gap = (uint32_t)(esp_timer_get_time() - s_lastDrainUs);
                if (gap > g_cdc_max_gap_us.load(std::memory_order_relaxed)) {
                    g_cdc_max_gap_us.store(gap, std::memory_order_relaxed);
                }
            }
            if (msg.buf_len) {
                if (ota_is_active()) {
                    ota_process_data(msg.buf, msg.buf_len);
                } else if (fx_stream_active()) {
                    fx_process_data(msg.buf, msg.buf_len);
                } else {
                    /* Debug, not info: a hexdump per command costs ~17 ms of
                     * blocking UART from this prio-1 loop and overflows
                     * app_queue under a pad stream. Raise the level to see it. */
                    ESP_LOGD(TAG, "Data from channel %d:", msg.itf);
                    ESP_LOG_BUFFER_HEXDUMP(TAG, msg.buf, msg.buf_len, ESP_LOG_DEBUG);

                    /* One CDC read is not one command: USB does not preserve
                     * write boundaries, and the handlers match on a prefix, so
                     * an unframed second command was silently dropped. */
                    cdc_dispatch_lines(msg.buf, msg.buf_len, msg.itf);
                }
            }
            s_queueWasBacked = uxQueueMessagesWaiting(app_queue) > 0;
            s_lastDrainUs = esp_timer_get_time();
        } else {
            s_queueWasBacked = false;
            /* Idle tick, once a second. */

            /* Report drops only here: they come in bursts, and a line per
             * drop would flood the console that caused the overflow. */
            static uint32_t reportedDrops = 0;
            const uint32_t drops = g_cdc_dropped.load(std::memory_order_relaxed);
            if (drops != reportedDrops) {
                reportedDrops = drops;
                ESP_LOGW(TAG, "CDC: %u commands dropped (app_queue full)",
                         (unsigned)reportedDrops);
            }
        }

        /* Stream stall checks run on their own timestamps every pass, not only
         * on a receive timeout: companion wakes (SysEx HOST_INFO from the
         * MIDI hub every 2 s, BLE, remote) post empty messages to app_queue,
         * so with a host attached the receive never timed out and a broken-off
         * FS_PUT/OTA held the CDC port in stream mode until a reset. Skipped
         * while data is still queued: that is the stream, not a stall. */
        if (uxQueueMessagesWaiting(app_queue) == 0) {
            ota_poll_timeout();
            fx_abort_timeout();
        }

    } // app loop
}

extern "C" void app_main(void)
{
    // Literal first statement: a trial boot (running image still
    // ESP_OTA_IMG_PENDING_VERIFY) gets an RTC watchdog that resets the chip
    // if fw_confirm_running() never disables it — a hang anywhere in this
    // function, including before bsp_stm32_init() ever runs, still ends in
    // the bootloader rolling back to the previous slot. No NVS, no heap, no
    // scheduler dependency: see fw_slots.h.
    fw_trial_watchdog_arm();

    /* One-shot, and cleared by the read: the rest of the boot is shaped by
     * this answer, and the next boot is normal whatever happens here. */
    Boot boot;
    boot.transferMode = usb_transfer_boot_claim();
    /* Network mode has the same one-shot shape (net_mode.h). Each request
     * restarts at once, so at most one of the two flags is ever armed. */
    boot.netMode = net_mode_boot_claim();

    /* The order below is the boot. Each stage says what it needs before it. */
    boot_storage_and_settings();
    boot_command_queue();
    boot_usb(boot);
    boot_platform();
    boot_assets_fs();
    boot_stm32_and_wake(boot);
    boot_imu_probe();
    boot_card(boot);
    boot_display(boot);
    boot_imu_task_and_pads(boot);
    boot_link_and_gui_tasks(boot);
    boot_confirm_timer();
    boot_sample_engine(boot);
    boot_kit_scan(boot);
    boot_audio(boot);
    boot_ota_and_overlays(boot);
    boot_card_hotplug(boot);
    command_loop(boot);
}  // app_main


