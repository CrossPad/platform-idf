/* Power and restart: the soft-off boot gate, the qualified wake, what the STM's
 * power states do to the ESP, and the restarts into the ROM bootloader. */
#include "esp_sleep.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "soc/rtc_cntl_reg.h"
#include "soc/system_reg.h"
#endif
#include "nvs_flash.h"
#include "tinyusb.h"
#include "include/hil_control.h"
#include "include/power_button.h"
#include "bsp/esp-bsp.h"
#include "bsp_sdmmc.h"
#include "driver/rtc_io.h"
#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "espnow.h"
#include "espnow_utils.h"
#endif
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "stm32_pwr_sync.h"   // STM32 power-state IRQ callback (set_callback / ack)
#include "stm32_i2c.h"        // Early wake qualification reads
#include "stm32_reg_map.h"
#ifdef USE_LVGL
#include "crosspad-gui/components/ui_toast.h"
#endif
#include "charge_overlay.h"   // charge-while-off pad gauge + "Charging" screen
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
#else
#include "engine_arena.hpp"
#include "SamplerEnginePort.hpp"
#endif

static const char *TAG = "crosspad_main";

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
void usb_otg_hw_reset(void)
{
    SET_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_USB_RST);
    CLEAR_PERI_REG_MASK(SYSTEM_PERIP_RST_EN0_REG, SYSTEM_USB_RST);
}
#endif

/* Last power state the STM pushed, and how many arrived (HIL: LED_STATE). */
std::atomic<uint8_t> g_hil_last_pwr_state{0xFF};
std::atomic<uint32_t> g_hil_pwr_state_count{0};

/* The STM powered us up only to show the charge screen (PWR_STATE_CHARGE at
 * the wake gate): nothing else of the instrument starts -- no engine, no
 * start-up sound, no kits, no USB audio, no radio. A wake from it restarts
 * into a normal boot (app_pwr_state_cb). */
static bool s_charge_boot_mode = false;
bool charge_boot_active(void) { return s_charge_boot_mode; }

#ifdef BSP_BOARD_CROSSPAD_S3
/* A STOP1 wake powers the ESP on the first power-button edge; hold the boot
 * until the button stays down long enough to make the wake intentional.
 * Separate from REG_CTL_LONG_PRESS, which is the already-awake action. */
static constexpr uint32_t WAKE_HOLD_MS = 1500;
static constexpr uint32_t WAKE_RELEASE_DEBOUNCE_MS = 40;
bool s_wake_hold_qualified = false;
crosspad::PadAnimator::AnimToken s_wake_hold_anim = crosspad::PadAnimator::kNoAnim;
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

void force_pad_strip_dark()
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
void app_pwr_state_cb(uint8_t state, void *user_data);

/* Worker job, every 40 ms: the STM's power state and the button, over I2C.
 * A UART-only STM (rev 1.x) has neither register: it reports the button in
 * its CMD_BUTTONS_DIGITAL frames, times its own power-off hold and has no
 * sleep states, so the gestures see a board that is always awake. */
uint32_t softoff_job(void *)
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

void qualify_power_button_wake()
{
    const uint8_t pwrState = wait_for_stm_power_state();
    s_charge_boot_mode = (pwrState == PWR_STATE_CHARGE);
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

bool recovery_pad_held()
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

void app_pwr_state_cb(uint8_t state, void *user_data)
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
            if (s_charge_boot_mode) {
                /* Woken from the charge screen: nothing of the instrument is
                 * up, so boot it whole. The STM's WOKE outlives the restart.
                 * Dark first: a lit panel shows white while the ESP resets. */
                charge_overlay_stop();
                crosspad::getStm32CommandSender().sendBrightness(0);
                force_pad_strip_dark();
                vTaskDelay(pdMS_TO_TICKS(50));
                esp_restart();
            }
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
