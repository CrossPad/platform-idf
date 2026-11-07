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
#ifdef BSP_BOARD_CROSSPAD_S3
#include "audio_task.h"
#include "performance_monitor.h"
#include "midi_clock.h"
#include "motion_stream.h"
#endif


#if CONFIG_CROSSPAD_SAMPLER_MARCEL
extern void smpl_stream_ply_test(void);
extern void sample_player_port_report_errors(void);
#else
#include "engine_arena.hpp"
#include "SamplerEnginePort.hpp"
#endif
extern void sample_player_port_stage_init(void);
#ifdef CONFIG_IDF_TARGET_ESP32S3
extern "C" void usb_audio_bridge_report(void);
#endif


#define BASE_PATH "/usb" // base path to mount the partition
#define LOG_MEM_INFO    (1)

static uint8_t rx_buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];

static QueueHandle_t app_queue;

/* CDC transport counters, reported as CDC_STATS. */
std::atomic<uint32_t> g_cdc_rx_chunks{0};
std::atomic<uint32_t> g_cdc_commands{0};
std::atomic<uint32_t> g_cdc_dropped{0};
/* Where a late reply was spent: inside one handler, or waiting for the loop to
 * get back to the queue. Both are read-and-cleared by CDC_STATS so a caller can
 * bracket a window. */
std::atomic<uint32_t> g_cdc_max_cmd_us{0};
std::atomic<uint32_t> g_cdc_max_gap_us{0};
char g_cdc_max_cmd_verb[20] = "-";
std::atomic<uint32_t> g_cdc_last_stack_free{0};
/* Cleared while a kit scan is in flight; see hil_control.h. */
std::atomic<bool> g_kit_scan_done{false};
std::atomic<bool> g_sd_probed{false};
typedef struct {
    uint8_t buf[CONFIG_TINYUSB_CDC_RX_BUFSIZE + 1];     // Data buffer
    size_t buf_len;                                     // Number of bytes received
    uint8_t itf;                                        // Index of CDC device interface
} app_message_t;

void enter_bootloader();

#ifdef BSP_BOARD_CROSSPAD_S3
/* Hardware-reset the USB-OTG controller. esp_restart() resets the CPUs only
 * (esp_system_reset_modules_on_exit() leaves USB alone), so the controller
 * runs on into the next boot until that image's esp_perip_clk_init() resets
 * it ~0.7 s later: tud_disconnect()'s soft-disconnect keeps the ROM
 * bootloader from enumerating, and the MIDI OUT endpoint, armed for DMA,
 * writes what the host sends into the RAM the bootloader has just loaded the
 * next image's .data into (after an OTA: FreeRTOS's port constants, a Guru
 * Meditation in ipc0's first dispatch, a rollback). Also a shutdown handler
 * run inside esp_restart(): no logging, no allocation, no waiting. */
static void usb_otg_hw_reset(void)
{
    SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_USB_RST);
    CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_USB_RST);
}
#endif

/* Last power state the STM pushed, and how many arrived (HIL: LED_STATE). */
std::atomic<uint8_t> g_hil_last_pwr_state{0xFF};
std::atomic<uint32_t> g_hil_pwr_state_count{0};

#ifdef BSP_BOARD_CROSSPAD_S3
/* A STOP1 wake powers the ESP on the first power-button edge; hold the boot
 * until the button stays down long enough to make the wake intentional.
 * Separate from REG_CTL_LONG_PRESS, which is the already-awake action. */
static constexpr uint32_t WAKE_HOLD_MS = 1500;
static constexpr uint32_t WAKE_RELEASE_DEBOUNCE_MS = 40;
static bool s_wake_hold_qualified = false;
static crosspad::PadAnimator::AnimToken s_wake_hold_anim = crosspad::PadAnimator::kNoAnim;
static std::atomic<bool> s_esp_softoff_started{false};
static std::atomic<bool> s_softoff_nv_armed{false};

static constexpr const char *SOFTOFF_NVS_NAMESPACE = "power";
static constexpr const char *SOFTOFF_NVS_KEY = "softoff";

/* Out of line: two callers below, one of them a task of its own. */
static void __attribute__((noinline)) softoff_gate_write()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SOFTOFF_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_set_u8(handle, SOFTOFF_NVS_KEY, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (err != ESP_OK) {
        s_softoff_nv_armed.store(false);
        ESP_LOGE(TAG, "Failed to persist soft-off gate: %s", esp_err_to_name(err));
    }
}

/* NVS writes flash, and a flash operation on a task whose stack is in PSRAM
 * asserts (spi_flash: esp_task_stack_is_sane_cache_disabled) -- abort(), a
 * PANIC and a core dump instead of the soft-off. The worker's stack is in
 * PSRAM, and softoff_job is where SLEEP_PENDING is usually seen first. From
 * such a task the write runs on a short-lived one with an internal stack, and
 * is waited for (bounded: the STM cuts the rail ~500 ms after it asked). */
void app_arm_softoff_boot_gate()
{
    if (s_softoff_nv_armed.exchange(true)) return;
    if (esp_ptr_internal(pxTaskGetStackStart(nullptr))) {
        softoff_gate_write();
        return;
    }
    static std::atomic<bool> s_written{false};
    if (xTaskCreatePinnedToCore([](void*) {
            softoff_gate_write();
            s_written.store(true);
            vTaskDelete(nullptr);
        }, "softoff_nv", 4096, nullptr, 5, nullptr, tskNO_AFFINITY) != pdPASS) {
        s_softoff_nv_armed.store(false);
        ESP_LOGE(TAG, "Failed to persist soft-off gate: %s", "no task");
        return;
    }
    for (int i = 0; i < 40 && !s_written.load(); ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

static bool app_softoff_boot_gate_is_armed()
{
    nvs_handle_t handle = 0;
    uint8_t value = 0;
    esp_err_t err = nvs_open(SOFTOFF_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) err = nvs_get_u8(handle, SOFTOFF_NVS_KEY, &value);
    if (handle) nvs_close(handle);
    const bool armed = err == ESP_OK && value == 1;
    s_softoff_nv_armed.store(armed);
    return armed;
}

static void app_clear_softoff_boot_gate()
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(SOFTOFF_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) err = nvs_erase_key(handle, SOFTOFF_NVS_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(handle);
    if (handle) nvs_close(handle);
    if (err == ESP_OK) {
        s_softoff_nv_armed.store(false);
    } else {
        ESP_LOGE(TAG, "Failed to clear soft-off gate: %s", esp_err_to_name(err));
    }
}

static void force_pad_strip_dark()
{
    auto& leds = crosspad::getPadLedController();
    leds.setBrightness(0);
    auto& strip = crosspad::getLedStrip();
    strip.lock();
    for (uint16_t i = 0; i < strip.getPixelCount(); ++i) {
        strip.setPixel(i, crosspad::RgbColor(0, 0, 0));
    }
    strip.refresh();
    strip.unlock();
}

[[noreturn]] static void enter_esp_softoff();

/* Armed soft-off gate: how often the dark ESP re-checks the STM, and after
 * how many checks in a row with the STM unreadable it gives up and boots
 * (it then shows its own UI, e.g. the STM-in-DFU screen). Survives deep
 * sleep in RTC memory. */
#define STM_GATE_RECHECK_US        (60LL * 1000 * 1000)
#define STM_GATE_MAX_UNREADABLE    3
RTC_DATA_ATTR static uint8_t s_gate_unreadable_checks;

[[noreturn]] static void deep_sleep_until_stm_wake()
{
    const gpio_num_t wakePin = (gpio_num_t)BSP_UART_RX;
    esp_err_t err = ESP_ERR_INVALID_ARG;

    if (rtc_gpio_is_valid_gpio(wakePin)) {
        err = rtc_gpio_init(wakePin);
        if (err == ESP_OK) err = rtc_gpio_set_direction(wakePin, RTC_GPIO_MODE_INPUT_ONLY);
        if (err == ESP_OK) err = rtc_gpio_pullup_en(wakePin);
        if (err == ESP_OK) err = rtc_gpio_pulldown_dis(wakePin);
        if (err == ESP_OK) {
            err = esp_sleep_enable_ext1_wakeup_io(1ULL << (uint32_t)wakePin,
                                                  ESP_EXT1_WAKEUP_ANY_LOW);
        }
    }

    if (err == ESP_OK) {
        /* Also wake on a timer and look again: an STM that never sends UART
         * traffic again (ROM DFU, wedged) left this sleep with no way out —
         * a dark unit with no UI. wait_for_stm_power_state() decides. */
        esp_sleep_enable_timer_wakeup(STM_GATE_RECHECK_US);
        ESP_LOGI(TAG, "Entering ESP deep sleep; STM UART RX GPIO%d (or %d s timer) is wake source",
                 (int)wakePin, (int)(STM_GATE_RECHECK_US / 1000000));
        esp_deep_sleep_start();
    }

    /* Unreachable in practice (GPIO9 is an RTC IO); stay dark rather than
     * continue the application. */
    ESP_LOGE(TAG, "Cannot arm STM wake for ESP deep sleep: %s",
             esp_err_to_name(err));
    while (true) vTaskDelay(portMAX_DELAY);
}

static uint8_t wait_for_stm_power_state()
{
    uint8_t state = PWR_STATE_AWAKE;
    bool waitingLogged = false;

    /* On USB, the STM's EN pull is a reset, not a power loss — hence the NVS
     * marker. Only SLEEP/SHIP_PENDING means a soft-off is really in progress;
     * any other readable state means the marker is stale and booting is safe.
     * Staying dark on a stale marker livelocks against the STM watchdog. */
    if (app_softoff_boot_gate_is_armed()) {
        ESP_LOGI(TAG, "Persistent soft-off marker present; waiting for explicit STM wake");
        const bool readable = stm32_i2c_read_u8(REG_IN_PWR_STATE, &state) == ESP_OK;
        if (readable && state != PWR_STATE_SLEEP_PENDING && state != PWR_STATE_SHIP_PENDING) {
            app_clear_softoff_boot_gate();
            s_gate_unreadable_checks = 0;
            ESP_LOGI(TAG, "Soft-off gate released by STM state 0x%02X", state);
            return state;
        }
        if (readable) {
            s_gate_unreadable_checks = 0;          // a soft-off STM answers: stay dark
        } else if (++s_gate_unreadable_checks >= STM_GATE_MAX_UNREADABLE) {
            s_gate_unreadable_checks = 0;
            app_clear_softoff_boot_gate();
            ESP_LOGE(TAG, "STM unreadable for %d gate checks; leaving the soft-off gate",
                     STM_GATE_MAX_UNREADABLE);
            return PWR_STATE_AWAKE;
        }
        deep_sleep_until_stm_wake();
    }

    /* The ESP's own VBUS can power it while the STM stays in soft-off, so an
     * unavailable STM is never permission to boot. Wait for a published state —
     * but not forever: a dead I2C link (wedged bus, STM mid-reflash) once held
     * this loop for good, and a device that can never leave a dark gate is a
     * brick with a charged battery. After the deadline we boot anyway, loudly;
     * every later sync still goes through the normal handlers. */
    const int64_t gateDeadlineUs = esp_timer_get_time() + 10 * 1000 * 1000;
    while (stm32_i2c_read_u8(REG_IN_PWR_STATE, &state) != ESP_OK) {
        if (!waitingLogged) {
            ESP_LOGI(TAG, "STM unavailable; holding ESP in dark soft-off gate");
            waitingLogged = true;
        }
        if (esp_timer_get_time() > gateDeadlineUs) {
            ESP_LOGE(TAG, "STM state unreadable for 10 s; leaving the gate and booting");
            return PWR_STATE_AWAKE;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (waitingLogged) {
        ESP_LOGI(TAG, "STM link restored with power state 0x%02X", state);
    }
    return state;
}

[[noreturn]] static void enter_esp_softoff()
{
    if (!s_esp_softoff_started.exchange(true)) {
        app_arm_softoff_boot_gate();
        crosspad::getPadAnimator().stopAll();
        force_pad_strip_dark();
        stm32_pwr_sync_ack();
        /* Let the WS2812 clear frame and the ACK leave the peripherals before
         * resetting — on USB power the reset is the only power boundary. */
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    while (true) vTaskDelay(portMAX_DELAY);
}

/* Watches the STM power state, and doubles as the power button's event path:
 * REG_IN_BUTTONS (bit0 = BTN1) and REG_IN_PWR_STATE are adjacent, so one
 * 2-byte read feeds both. The STM pushes no press/release events while awake
 * and only times the *wake* hold, so the click/hold split is measured here —
 * hence the 40 ms period rather than the 100 ms the soft-off watch needed. */
static void app_pwr_state_cb(uint8_t state, void *user_data);

/* Worker job, every 40 ms: the STM's power state and the button, over I2C.
 * A UART-only STM (rev 1.x) has neither register: it reports the button in
 * its CMD_BUTTONS_DIGITAL frames, times its own power-off hold and has no
 * sleep states, so the gestures see a board that is always awake. */
static uint32_t softoff_job(void *)
{
    if (!BSP_HAS_STM32_I2C) {
        power_button_feed(crosspad::getStm32MessageHandler().input.powerBtnPressed, PWR_STATE_AWAKE,
                          (uint32_t)(esp_timer_get_time() / 1000));
        return 0;
    }
    static uint8_t s_prevPolled = 0xFF;
    uint8_t regs[2] = {0, PWR_STATE_AWAKE};
    if (stm32_i2c_read(REG_IN_BUTTONS, regs, sizeof(regs)) == ESP_OK) {
        const uint8_t state = regs[1];
        if (state == PWR_STATE_SLEEP_PENDING || state == PWR_STATE_SHIP_PENDING) {
            ESP_LOGI(TAG, "Power monitor caught state 0x%02X", state);
            enter_esp_softoff();
        }
        /* LIGHT / AWAKE / CHARGE come once over UART with no retry, and a UART
         * overflow flushes the queue: a lost AWAKE left the LCD at the STM's
         * wake brightness and the pads at a fifth. Apply a state the register
         * has shown for two polls in a row that the UART path did not deliver.
         * Not before the first UART state (0xFF): at boot the WOKE path owns
         * the screen and must not be pre-empted by a register AWAKE. */
        const uint8_t handled = g_hil_last_pwr_state.load();
        if (handled == 0xFF && status.isGuiInitialised && state == s_prevPolled) {
            g_hil_last_pwr_state.store(state);   // baseline once the screen is ours
        }
        if (state == s_prevPolled && state != handled && handled != 0xFF &&
            (state == PWR_STATE_LIGHT || state == PWR_STATE_AWAKE || state == PWR_STATE_CHARGE)) {
            ESP_LOGW(TAG, "Power state 0x%02X missed on UART; applying it from I2C", state);
            app_pwr_state_cb(state, nullptr);
        }
        s_prevPolled = state;
        power_button_feed((regs[0] & 0x01u) != 0, state,
                          (uint32_t)(esp_timer_get_time() / 1000));
    }
    return 0;
}

static void wait_for_stm_cancelled_wake()
{
    ESP_LOGI(TAG, "STM cancelled wake hold; waiting for soft-off");
    app_arm_softoff_boot_gate();
    crosspad::getPadAnimator().stopAll();
    crosspad::getPadLedController().setBrightness(0);

    /* STM owns this transition. Do not send a second POWEROFF command: it can
     * become a deferred request and put the next successful wake back to sleep.
     * ACK SLEEP_PENDING directly once it appears, then wait for the rail cut. */
    while (true) {
        uint8_t state = PWR_STATE_AWAKE;
        if (stm32_i2c_read_u8(REG_IN_PWR_STATE, &state) == ESP_OK &&
            state == PWR_STATE_SLEEP_PENDING) {
            enter_esp_softoff();
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void return_to_soft_off_after_cancelled_wake()
{
    ESP_LOGI(TAG, "Wake hold cancelled; returning to soft-off");
    app_arm_softoff_boot_gate();

    crosspad::getPadAnimator().stopAll();
    crosspad::getPadLedController().setBrightness(0);

    /* Retry: a transient I2C/UART failure must never turn a short accidental
     * press into a full boot. The rail normally goes after the first command. */
    while (true) {
        if (!crosspad::getStm32CommandSender().sendPowerOff()) {
            ESP_LOGW(TAG, "Failed to request soft-off after cancelled wake; retrying");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

static void qualify_power_button_wake()
{
    const uint8_t pwrState = wait_for_stm_power_state();
    if (pwrState == PWR_STATE_SLEEP_PENDING) wait_for_stm_cancelled_wake();
    if (pwrState != PWR_STATE_WOKE) return;

    uint16_t proto = 0;
    esp_err_t identErr =
        stm32_i2c_get_cached_ident(&proto, nullptr, nullptr, nullptr);
    if (identErr != ESP_OK) {
        /* bsp_stm32_init() may have probed while the STM was still asleep. */
        identErr = stm32_i2c_probe(&proto, nullptr, nullptr, nullptr);
    }
    const bool stmWakeTelemetry = identErr == ESP_OK && proto >= 0x000B;

    /* Proto 0x000B tells whether BTN1 caused the wake; encoder/IMU/USB wakes
     * bypass this gate and stay immediate. */
    uint8_t wake[3] = {};
    if (stmWakeTelemetry) {
        if (stm32_i2c_read(REG_IN_PWR_HOLD_HI, wake, sizeof(wake)) != ESP_OK) return;
        const bool active = (wake[2] & PWR_WAKE_FLAG_ACTIVE) != 0;
        const bool qualified = (wake[2] & PWR_WAKE_FLAG_QUALIFIED) != 0;
        if (qualified) {
            s_wake_hold_qualified = true;
            return;
        }
        if (!active) return;
    }

    const uint32_t fallbackStartMs = (uint32_t)(esp_timer_get_time() / 1000);
    uint32_t initialHeldMs = stmWakeTelemetry
        ? (uint32_t)(((uint16_t)wake[0] << 8) | wake[1])
        : 0;
    ESP_LOGI(TAG, "Power-button wake: %u/%u ms elapsed at qualification start%s",
             (unsigned)initialHeldMs, (unsigned)WAKE_HOLD_MS,
             stmWakeTelemetry ? " (STM clock)" : " (ESP fallback)");

    auto& animator = crosspad::getPadAnimator();
#ifndef USE_LVGL
    /* The pads show the hold only where no boot splash draws on them; with
     * one, the hold-progress was a flash of lime before the splash, for a
     * hold the BQ's QON already asks a second of. */
    crosspad::getPadLedController().setBrightness(settings->RGBbrightness);
    s_wake_hold_anim = animator.startProgress();
#endif

    int64_t releasedSinceUs = 0;
    while (true) {
        const int64_t nowUs = esp_timer_get_time();
        uint32_t heldMs = (uint32_t)(nowUs / 1000) - fallbackStartMs;

        uint8_t buttons = 0;
        bool readOk = false;
        bool pressed = false;
        bool stmQualified = false;
        if (stmWakeTelemetry) {
            readOk = stm32_i2c_read(REG_IN_PWR_HOLD_HI, wake, sizeof(wake)) == ESP_OK;
            if (readOk) {
                heldMs = (uint32_t)(((uint16_t)wake[0] << 8) | wake[1]);
                pressed = (wake[2] & PWR_WAKE_FLAG_ACTIVE) != 0;
                stmQualified = (wake[2] & PWR_WAKE_FLAG_QUALIFIED) != 0;
                if (!pressed && !stmQualified) wait_for_stm_cancelled_wake();
            }
        } else {
            readOk = stm32_i2c_read_u8(REG_IN_BUTTONS, &buttons) == ESP_OK;
            pressed = readOk && ((buttons & 0x01u) != 0);
        }

        if (pressed) {
            releasedSinceUs = 0;
        } else if (releasedSinceUs == 0) {
            releasedSinceUs = nowUs;
        } else if ((nowUs - releasedSinceUs) >=
                   (int64_t)WAKE_RELEASE_DEBOUNCE_MS * 1000) {
            return_to_soft_off_after_cancelled_wake();
        }

        animator.setProgress((float)heldMs / (float)WAKE_HOLD_MS);
        animator.update();

        // Require a live pressed sample at the finish line too — an I2C
        // dropout or a release just before 1.5 s must not qualify.
        if (stmQualified || (heldMs >= WAKE_HOLD_MS && pressed)) {
            animator.setProgress(1.0f);
            animator.update();
            s_wake_hold_qualified = true;
            ESP_LOGI(TAG, "Power-button wake accepted");
            return;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Pad 16 held through the boot: the way back to the main firmware from one
 * that does not work (fw_recovery_switch()), whatever app that firmware runs
 * -- decided here, before any of it starts. A pad already held sends no
 * note-on, so the STM's debounced bitmap is read (bit 15 = pad 16), twice a
 * little apart: one sample right after a wake could still be a rescan's. */
static constexpr uint16_t kRecoveryPadBit = 1u << 15;
static constexpr uint32_t kRecoverySampleGapMs = 50;

static bool recovery_pad_held()
{
    for (int i = 0; i < 2; ++i) {
        if (i) vTaskDelay(pdMS_TO_TICKS(kRecoverySampleGapMs));
        uint8_t pads[2];
        if (stm32_i2c_read(REG_IN_PADS_HI, pads, sizeof(pads)) != ESP_OK) return false;
        const uint16_t bitmap = static_cast<uint16_t>((pads[0] << 8) | pads[1]);
        if (!(bitmap & kRecoveryPadBit)) return false;
    }
    ESP_LOGW(TAG, "pad 16 held at boot: recovery to the main firmware");
    return true;
}
#endif

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

static void app_pwr_state_cb(uint8_t state, void *user_data)
{
    (void)user_data;
    g_hil_last_pwr_state.store(state);
    ++g_hil_pwr_state_count;
    switch (state) {
        case PWR_STATE_SLEEP_PENDING:   // soft-off imminent (LPSLEEP) — blank ARGB, release STM
        case PWR_STATE_SHIP_PENDING:    // BATFET cut imminent — same
            charge_overlay_stop();      // no-op unless we were charge-idling
            enter_esp_softoff();
        case PWR_STATE_LIGHT: {         // LIGHT sleep — ESP stays up, dim screen + ARGB
            /* Dark screen: let the IMU sleep its gyro when the board is put
             * down, and wake on motion. While it is in use the accel stays at
             * its full rate, where taps are recognised. */
            bsp_imu_set_inactivity(true);
            // 8 % at most: a user level below that must not get brighter on dim.
            crosspad::getStm32CommandSender().sendBrightness(
                (uint8_t)std::min<int>(8, settings->LCDbrightness));
            // A fifth of the user's level, so a dark device never gets brighter.
            uint8_t dimmed = (uint8_t)(settings->RGBbrightness / 5);
            crosspad::getPadLedController().setBrightness(dimmed);
            break;
        }
        case PWR_STATE_CHARGE:          // charger-idle while off — battery gauge + "Charging" screen
            crosspad::getStm32CommandSender().sendBrightness((uint8_t)settings->LCDbrightness);
            charge_overlay_start();     // drives pad gauge + screen; sets pad brightness
            break;
        case PWR_STATE_WOKE:            // woke from soft-off; LCD still contains the old frame
            bsp_imu_set_inactivity(false);
            // ACK, or the STM re-sends the IRQ every 250 ms for 2 s.
            stm32_pwr_sync_ack();
            charge_overlay_stop();      // leaving charge-idle (user woke the device)
            // The STM cut the amp behind our back on the way into LIGHT;
            // the cached REG_CTL_AUDIO_EN is stale.
            crosspad::getStm32Manager().invalidateAmpState();
            crosspad::getPadLedController().setBrightness(settings->RGBbrightness);
            if (status.isGuiInitialised) {
                /* LVGL live and the frame current — relight now. */
                crosspad::getStm32CommandSender().sendBrightness((uint8_t)settings->LCDbrightness);
            } else {
                /* Boot: stay dark until LVGLTask has flushed the new screen,
                 * or the retained framebuffer shows as an afterimage. */
                crosspad::getStm32CommandSender().sendBrightness(0);
            }
            break;
        case PWR_STATE_AWAKE:           // left LIGHT — ESP/LVGL stayed alive
            bsp_imu_set_inactivity(false);
            charge_overlay_stop();
            crosspad::getStm32Manager().invalidateAmpState();
            crosspad::getPadLedController().setBrightness(settings->RGBbrightness);
            crosspad::getStm32CommandSender().sendBrightness((uint8_t)settings->LCDbrightness);
            break;
        default:
            break;
    }
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

extern "C" void tinyusb_cdc_rx_callback(int itf, cdcacm_event_t *event)
{
    size_t rx_size = 0;
    esp_err_t ret = tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, rx_buf, CONFIG_TINYUSB_CDC_RX_BUFSIZE, &rx_size);
    if (ret == ESP_OK) {

        app_message_t tx_msg;   /* buf filled by the memcpy below */
        tx_msg.buf_len = rx_size;
        tx_msg.itf = (uint8_t)itf;

        memcpy(tx_msg.buf, rx_buf, rx_size);
        const bool binaryStream = ota_is_active() || fx_stream_active();
        if (xQueueSend(app_queue, &tx_msg, binaryStream ? portMAX_DELAY : 0) == pdTRUE) {
            g_cdc_rx_chunks.fetch_add(1, std::memory_order_relaxed);
        } else {
            /* A drop here is silent and looks like a refused command, so it
             * is counted; the main loop reports it. */
            g_cdc_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        ESP_LOGE(TAG, "Read Error");
    }
}

extern "C" void tinyusb_cdc_line_state_changed_callback(int itf, cdcacm_event_t *event)
{
    int dtr = event->line_state_changed_data.dtr;
    int rts = event->line_state_changed_data.rts;
    ESP_LOGI(TAG, "CDC Line state changed %d: DTR:%d, RTS:%d", itf, dtr, rts);
}

void check_usb_bootloader_request(const char* buff, size_t len) {
    const char* bootloader_request = "BOOTLOADER_REQUEST";
    if (len < strlen(bootloader_request)) {
        return;
    }
    if (memcmp(buff, bootloader_request, strlen(bootloader_request)) == 0) {
        ESP_LOGW(TAG, "Entering bootloader from CDC");
        enter_bootloader();
    }
}

/* Run the CDC handler chain over one complete command. */
static void cdc_dispatch_one(const char *line, size_t len, uint8_t itf)
{
    if (len == 0) return;
    g_cdc_commands.fetch_add(1, std::memory_order_relaxed);
    hil_stack_mark();
    const int64_t t0Us = esp_timer_get_time();

    bool handled = ota_check_begin(line, len);
    if (!handled) handled = ota_check_delta_begin(line, len);
    if (!handled) handled = fx_handle_cdc(line, len);
    if (!handled) handled = audio_route_control_handle_cdc(line, len);
    if (!handled) handled = hil_control_handle_cdc(line, len);
    if (!handled) {
        check_usb_bootloader_request(line, len);
        check_usb_mode_command(line, len);
        /* The echo is the host's "unknown verb" signal, so it is a line of
         * its own: since the framing above drops the terminator, an echo
         * without one glued itself to the next reply ("KIT2" + "OK" read
         * "KIT2OK"), and both the unknown verb and the next one timed out. */
        tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf,
                                   (const uint8_t *)line, len);
        tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf,
                                   (const uint8_t *)"\r\n", 2);
        esp_err_t err = tinyusb_cdcacm_write_flush((tinyusb_cdcacm_itf_t)itf, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "CDC ACM write flush error: %s", esp_err_to_name(err));
        }
    }

    g_cdc_last_stack_free.store(hil_stack_note(line, len), std::memory_order_relaxed);
    const uint32_t us = (uint32_t)(esp_timer_get_time() - t0Us);
    if (us > g_cdc_max_cmd_us.load(std::memory_order_relaxed)) {
        g_cdc_max_cmd_us.store(us, std::memory_order_relaxed);
        size_t n = len < sizeof(g_cdc_max_cmd_verb) - 1 ? len : sizeof(g_cdc_max_cmd_verb) - 1;
        memcpy(g_cdc_max_cmd_verb, line, n);
        g_cdc_max_cmd_verb[n] = '\0';
    }
}

/* Split one CDC read into commands and dispatch each.
 *
 * OTA must not be framed: after OTA_BEGIN the stream is firmware bytes, where
 * 0x0A is data. An unterminated tail is held back only when the read filled
 * the buffer; a short read means the sender's write ended there. */
/* USB full-speed bulk max packet: the boundary the host's writes are cut on. */

static void cdc_dispatch_lines(const uint8_t *buf, size_t len, uint8_t itf)
{
    static CdcLineAssembler s_line;

    /* A command ends at its terminator and nowhere else. A short USB packet
     * used to count as the end of a line too, and that guess was wrong twice:
     * a 65-byte command arrived as 64 + 1 and was dispatched truncated, and
     * Web Serial hands the kernel a 46-byte write in two pieces (36 + 10) —
     * the first piece ran as a command with a cut-off path, the second came
     * back as the echo of an unknown one. Every host terminates with LF, so
     * a partial line simply waits in s_line for the rest. */
    for (size_t i = 0; i < len; i++) {
        if (ota_is_active()) {
            /* OTA_BEGIN was accepted earlier in this very buffer. */
            ota_process_data((uint8_t *)&buf[i], len - i);
            s_line.reset();
            return;
        }
        if (fx_stream_active()) {
            /* Same exemption, same reason: after ASSETS_BEGIN or FS_PUT the
             * stream is payload, where 0x0A is data. It ends when the byte
             * count announced in the header has arrived. */
            fx_process_data(&buf[i], len - i);
            s_line.reset();
            return;
        }

        switch (s_line.feed((char)buf[i])) {
        case CdcLineAssembler::Result::Line:
            cdc_dispatch_one(s_line.text(), s_line.size(), itf);
            break;
        case CdcLineAssembler::Result::TooLong: {
            /* Longer than any command this firmware has: the whole line is
             * dropped, not dispatched truncated, and answered once. */
            ESP_LOGW(TAG, "CDC: command over %d bytes discarded", CDC_LINE_MAX - 1);
            static const char kTooLong[] = "ERR too_long\r\n";
            tinyusb_cdcacm_write_queue((tinyusb_cdcacm_itf_t)itf,
                                       (const uint8_t *)kTooLong, sizeof(kTooLong) - 1);
            tinyusb_cdcacm_write_flush((tinyusb_cdcacm_itf_t)itf, 0);
            break;
        }
        case CdcLineAssembler::Result::None:
            break;
        }
    }
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
    const bool transferMode = usb_transfer_boot_claim();
    /* Network mode has the same one-shot shape (net_mode.h). Each request
     * restarts at once, so at most one of the two flags is ever armed. */
    const bool netMode = net_mode_boot_claim();
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
    app_message_t msg;
    usb_host_profile_init();   // before the first SOF the feedback is shaped for
    ESP_ERROR_CHECK(usb_config_manager_init(transferMode ? USB_MODE_TRANSFER
                                                         : USB_MODE_DEFAULT));
#ifdef BSP_BOARD_CROSSPAD_S3
    /* From here on every esp_restart() -- OTA, slot switch, transfer and
     * network mode, soft-off, bootloader -- stops USB DMA before the next
     * image is loaded. A panic's restart does not run shutdown handlers. */
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_register_shutdown_handler(usb_otg_hw_reset));
#endif

    settings->stmRouting.SetUsbPassThru(true);

    crosspad::CrosspadPlatform_Init(settings, &status);
    host_info_init();
    cp_account_init();
    fw_updater_init();
    wifi_creds_init();
    cp_remote_init(!transferMode && !netMode && CROSSPAD_FW_RADIO);   // none of these boots holds a remote session
    instrument_host_init();                  // an app may borrow the pitched engine

    midiCallbacks_init();
    crosspad::getPadManager().begin();
    hil_pad_inject_init();

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

    bool recoveryHeld = false;   // pad 16 through the boot, acted on once the card is up
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
    recoveryHeld = recovery_pad_held();
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

    /* In transfer mode the card belongs to the host, raw — a FATFS mount here
     * would be the second writer on one volume. usb_transfer_storage_attach()
     * already brought it up from the USB install. */
    if (!transferMode) {
        status.sdCardDetected = (bsp_sdcard_mount_ex(true) == ESP_OK);
        // A pending FW_SWITCH is resolved here: NVS and the SD card (the
        // settings.json a switch may have exported) are both up, and nothing
        // below yet reads a per-firmware setting such as Kit — kit discovery,
        // the audio engine and the LVGL/GUI task all start after this.
        fw_switch_finish_boot();
        // After the mount: the switch exports the leaving firmware's settings
        // to its package on the card. app_main's stack is internal RAM, which
        // the otadata write needs.
        if (recoveryHeld) {
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

    // Kit discovery: after the mount, and in its own task — the recursive
    // scan needs more stack than app_main has.

#ifdef BSP_BOARD_CROSSPAD_S3
    bsp_display_brightness_set(0);
#endif
    lv_display_t* disp = bsp_display_start();
    const bool headless = (disp == NULL);
    if (headless) {
        ESP_LOGW(TAG, "Display unavailable - running headless (MIDI/pads only)");
    }

#ifdef BSP_BOARD_CROSSPAD_S3
    /* After the display: its touch driver owns the GPIO ISR service the IMU's
     * data-ready line hangs off (bsp_imu.cpp, attach_drdy_line). */
    if (status.imuDetected) {
        imu_ret = bsp_imu_start_task(20, 5, 3096);   // 20 ms poll only without INT2
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
    if (!headless) {
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

    xTaskCreatePinnedToCoreWithCaps(uartLoop, "UART drv", 4000, NULL, 6, NULL, 0,
                                    MALLOC_CAP_SPIRAM);   // STM link, no flash

    if (!headless) {
        xTaskCreatePinnedToCore(LVGLTask, "LVGL drv", 5500, NULL, 4, NULL, 0);
    }

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

    if (!transferMode && !netMode) {
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
    if (status.sdCardDetected && !netMode && CROSSPAD_FW_SAMPLE_ENGINE) {
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
        }, "kit_scan", 8192, headless ? (void*)1 : nullptr, 2, nullptr, 1) != pdPASS) {
            g_kit_scan_done.store(true);
        }
    } else {
        /* No card (or no sample engine): the list is empty and final, so
           KIT_* must not wait for a scan that will never run. */
        g_kit_scan_done.store(true);
    }
    /* Transfer mode starts no engine: the arena stays unallocated, the RT task
     * has no card to stream from, and the profile it enumerated has no audio.
     * Network mode neither: its internal RAM belongs to WiFi. Nor a kit
     * whose profile says "audio": false (its app plays nothing). */
    if (!transferMode && !netMode && CROSSPAD_FW_AUDIO) AudioTasks_Start();

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
    if (settings->usbAudioEnabled && !transferMode && !netMode && CROSSPAD_FW_AUDIO) {
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

#ifdef CONFIG_IDF_TARGET_ESP32S3
    espnow_storage_init();
#endif

    ota_init();

#ifdef USE_LVGL
    /* Transfer mode's screen is the whole UI; an overlay over it would offer a
     * firmware update that reads the assets partition mid-transfer. */
    if (!headless && !transferMode) {
        extern void ota_overlay_init(void);
        ota_overlay_init();
        /* An STM reflash is a normal boot's decision; network mode is a short visit. */
        if (!netMode && (s_stm_fw_status.due || s_stm_fw_status.pcb_mismatch)) {
            stm_fw_overlay_show(&s_stm_fw_status);   /* also raised on a PCB mismatch: says so, offers nothing */
        }
    }
#endif
    if (headless && !transferMode && !netMode && s_stm_fw_status.due) {
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

    /* The hotplug probe mounts the card it finds — never while the host has it —
     * and rescans kits, which network mode does not load. */
    /* A boot that mounted no card holds its update check until the first probe
     * (fw_updater.cpp): a card a second late then scans before the radio starts. */
    if (status.sdCardDetected) g_sd_probed.store(true);
    if (transferMode || netMode || !bsp_worker_add("sd_hotplug", 1000, sd_hotplug_job, nullptr))
        g_sd_probed.store(true);
    lv_async_safe_start();

    /* Main application loop — drains the CDC command queue and nothing else.
     * Anything that can block belongs on another task: this loop is what
     * answers every off-device query. */
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
        if (!s_bleAutoStartDone && !transferMode && !netMode && (headless || status.isGuiInitialised)) {
            s_bleAutoStartDone = true;
            bleMidi_boot();
        }

        // Update checks start once the launcher has painted, like BLE above:
        // the boot check (when enabled) and the daily idle watch. Normal boots
        // only -- network mode has no launcher to be idle in.
        static bool s_fwUpdaterStarted = false;
        if (!s_fwUpdaterStarted && !transferMode && !netMode && status.isGuiInitialised) {
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
        if (!s_fwRollbackToastDone && !transferMode && status.isGuiInitialised) {
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
        if (!s_fwAssetsToastDone && !transferMode && status.isGuiInitialised) {
            s_fwAssetsToastDone = true;
            static char s_fwAssetsToastMsg[128];
            if (fw_assets_sync_toast_pending(s_fwAssetsToastMsg, sizeof(s_fwAssetsToastMsg))) {
                lv_async_call_locked([](void *data) {
                    ui_toast_show(static_cast<const char *>(data), 3000, UI_TOAST_WARNING);
                }, s_fwAssetsToastMsg);
            }
        }
#endif

        // Wake at least once a second to report queue drops.
        TickType_t timeout = (ota_is_active() || fx_stream_active())
                                 ? pdMS_TO_TICKS(10000) : pdMS_TO_TICKS(1000);
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
            if (ota_is_active()) {
                ota_abort_timeout();
            }
            fx_abort_timeout();

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

    } // app loop
}  // app_main


void enter_bootloader(){
    ESP_LOGW(TAG, "Entering bootloader");

#ifdef BSP_BOARD_CROSSPAD_S3
    // Download boot flag first, before any deinit that could hang.
    REG_WRITE(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT);

    // USB-OTG reset next, also before any deinit that could hang. The shutdown
    // handler repeats it in esp_restart(); a second pulse changes nothing.
    usb_otg_hw_reset();

    vTaskDelay(pdMS_TO_TICKS(100));

    crosspad::CrosspadPlatform_StopStm32();
    bsp_stm32_deinit();
#endif
    bsp_i2c_deinit();
    bsp_sdcard_unmount();

    esp_restart();
}
