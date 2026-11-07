#ifndef DISPLAY_H
#define DISPLAY_H

#include <inttypes.h>
#include "lvgl_stats.h"

bool AudioTasks_IsReady(void);   // main/audio/audio_task.h; the splash gate waits on it
#include "esp_timer.h"
#include "CrossPad.h"
#include <vector>
#include "processMIDI.hpp"
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadLedController.hpp"

extern CrosspadSettings * settings;
bool requestKeepAlive = true;

#include "crosspad/platform/IVibration.hpp"
#include "crosspad/platform/PlatformServices.hpp"


#ifdef USE_LVGL
// #include "lv_conf.h"
#include <lvgl.h>
#include "esp_lvgl_port.h"
#include "IdfGuiPlatform.hpp"
#include "fw_view_service.h"
#include "crosspad-gui/platform/touchpad_interaction.h"  // gui_touchpad_event_cb — touch -> STM idle timer
#include "touch_trace.h"                            // raw touch sampling below the object layer
#include "twin_mirror.h"                            // input lockstep for the browser twin
#include "usb_transfer.h"                           // transfer mode replaces the launcher
#include "usb_transfer_screen.h"
#include "net_mode.h"                               // network mode replaces it too
#include "net_mode_screen.h"
#include "crosspad-gui/components/boot_splash.h"   // the cold-boot animation
#include "crosspad-gui/components/touch_gesture.h"       // swipe recognition, same layer

#ifdef BSP_BOARD_CROSSPAD_S3
#include "crosspad_s3.h"
#include "stm32_i2c.h"       // stm32_i2c_read_u8 — wake-from-soft-off detection
#include "stm32_reg_map.h"   // REG_IN_PWR_STATE / PWR_STATE_CHARGE
#include "charge_overlay.h"  // charge-while-off boot detection
#include "indev/encoder.h"
#include "indev/keypad.h"
#define TFT_HOR_RES TFT_WIDTH
#define TFT_VER_RES TFT_HEIGHT
#define DRAW_BUF_SIZE (TFT_HEIGHT * TFT_WIDTH / 8 * (LV_COLOR_DEPTH / 8))
#else
#include "bsp/esp32_p4_function_ev_board.h"
#define TFT_HOR_RES BSP_LCD_H_RES
#define TFT_VER_RES BSP_LCD_V_RES
#define DRAW_BUF_SIZE (BSP_LCD_H_RES * BSP_LCD_V_RES / 8 * (LV_COLOR_DEPTH / 8))
#endif

// LVGL groups for input devices — defined once in main.cpp. display.h is
// pulled into every BSP translation unit through crosspad_s3.h, so a plain
// definition here would be a tentative definition in each of them; the
// linker has only been merging those as common symbols.
extern lv_group_t *default_group; // currently not used
extern lv_group_t *keypad_group;
extern lv_group_t *encoderLeft_group;
extern lv_group_t *encoderTop_group;

// LVGL input devices drivers — defined once in main.cpp
extern lv_indev_t *indev_keypad;
extern lv_indev_t *indev_encoderLeft;
extern lv_indev_t *indev_encoderTop;
extern lv_indev_t *indev_touchpad;

#include "gui.h" // include GUI functions after groups and drivers are defined

extern bool requestKeepAlive;
// clicked, released and others feedback actions eg. vibrations
void touchpad_acted_cb(lv_event_t *event)
{
    auto& vib = crosspad::getVibration();
    if (!vib.isAvailable()) return;

    lv_event_code_t code = lv_event_get_code(event);

    requestKeepAlive = true; // keep the screen on, send msg to STM32

    static bool lastWasLost = false; // last event was "press lost"
    switch (code)
    {
    case LV_EVENT_PRESSED:
        vib.vibrate(crosspad::VibrationPreset::TouchPressed);
        break;
    case LV_EVENT_PRESS_LOST:
        vib.vibrate(crosspad::VibrationPreset::TouchReleased);
        lastWasLost = true;
        break;
    case LV_EVENT_LONG_PRESSED:
        vib.vibrate(crosspad::VibrationPreset::TouchLongPress);
        break;
    case LV_EVENT_LONG_PRESSED_REPEAT:
        vib.vibrate(crosspad::VibrationPreset::TouchLongPressRepeat);
        break;
    case LV_EVENT_RELEASED:
        if (!lastWasLost) vib.vibrate(crosspad::VibrationPreset::TouchReleased);
        lastWasLost = false;
        break;
    default:
        // do nothing
        break;
    }
}

/* Set when REG_IN_PWR_STATE == PWR_STATE_CHARGE at boot: the STM repowered us
 * only to show the charge-while-off feedback. We skip the welcome splash and
 * trigger the charge overlay once LVGL is up. Every other boot -- power-on,
 * a wake from soft-off, a reset, an OTA -- plays the splash: the STM's WOKE
 * outlives the wake it was about, and a reset on EN reads as a power-on too,
 * so nothing here could tell a wake from a reset. */
static bool s_charge_boot = false;

/* The splash's backlight: it fades the panel in and out itself. */
static void splash_backlight(uint8_t level)
{
#ifdef BSP_BOARD_CROSSPAD_S3
    crosspad::getStm32CommandSender().sendBrightness(level);
#else
    bsp_display_brightness_set((int)level);
#endif
}

void setupLVGL()
{
    lvgl_port_lock(3000); // lock LVGL for setup
    lvgl_stats_install(lv_display_get_default());
#if LV_USE_SYSMON && LV_USE_PERF_MONITOR
    /* LVGL's own FPS/CPU label, up since the display was made: the status bar
     * hides it and shows its own figures when asked, but the splash comes
     * before the status bar. */
    lv_sysmon_hide_performance(NULL);
#endif
    TickType_t startTime = xTaskGetTickCount(); // get the current tick count to limit the welcome screen time

#ifdef BSP_BOARD_CROSSPAD_S3
    crosspad::getStm32CommandSender().sendBrightness(0); // turn off backlight to avoid flicker during setup

    /* Charge boot or not. I2C is already up by here (stm32_hal_init ran in
     * app_main). On failure we default to the splash -- the safe choice. */
    {
        uint8_t pwr = 0;
        if (stm32_i2c_read_u8(REG_IN_PWR_STATE, &pwr) == ESP_OK && pwr == PWR_STATE_CHARGE) {
            s_charge_boot = true;
        }
    }
#endif

    /* Rate-limited: a draw that cannot get a layer buffer logs two WARN lines
     * every 10 ms, which is more than the 115200-baud console can carry. The
     * blocking printf then parks taskLVGL in the UART driver with the LVGL
     * lock held, and everything queued behind that lock -- the CDC drain loop
     * included -- stops answering; the board looked dead for a memory warning.
     * The two lines alternate, so "same as the previous line" never matches:
     * the budget is global, so many lines per second, and what exceeds it is
     * counted and reported once per window. */
    lv_log_register_print_cb([](lv_log_level_t level, const char * buf) {
        (void)level;
        constexpr int64_t  kWindowUs      = 1000 * 1000;
        constexpr uint32_t kLinesPerWindow = 20;   /* ~2 kB/s of a 11 kB/s console */
        static int64_t  windowStartUs = 0;
        static uint32_t inWindow      = 0;
        static uint32_t dropped       = 0;
        const int64_t now = esp_timer_get_time();
        if (now - windowStartUs >= kWindowUs) {
            if (dropped) {
                printf("LVGL log: (%" PRIu32 " lines suppressed in the last second)\n", dropped);
                dropped = 0;
            }
            windowStartUs = now;
            inWindow = 0;
        }
        if (++inWindow > kLinesPerWindow) {
            dropped++;
            return;
        }
        printf("LVGL log: %s\n", buf);
    });
    // Welcome splash -- every boot but the charge screen's. Drawn from
    // objects and a 6 kB alpha image, not a decoded picture: the engine arena
    // leaves ~520 kB of PSRAM at this point.
    if (!s_charge_boot) {
        boot_splash_start(lv_screen_active(), (uint8_t)settings->LCDbrightness, splash_backlight, false);
    }

#ifdef BSP_BOARD_CROSSPAD_S3
    /* Touch is optional: a panel whose FT6x36 did not come up is still a
     * usable screen with encoder-only navigation, and bsp_display_indev_init()
     * says so by returning NULL. Passing that NULL on is not survivable —
     * lv_indev_add_event_cb() runs LV_ASSERT_NULL, and LVGL's assert handler
     * is `while(1);`, so this task (prio 4, pinned to core 0) would spin
     * forever holding the LVGL lock, starving IDLE0 and every lower-priority
     * task on that core, main() and the CDC command loop included. */
    indev_touchpad = bsp_display_get_input_dev();
    if (indev_touchpad) {
        lv_indev_add_event_cb(indev_touchpad, touchpad_acted_cb, LV_EVENT_ALL, NULL);
        /* Touch is ESP-owned — the STM pad scan never sees it, so its idle
         * timer only learns about it through REG_CMD_USER_ACTIVITY, which this
         * callback is the sole path to (IGuiPlatform::onUserInteraction ->
         * Stm32Manager::notifyUserActivity). Without it a tap on a dimmed
         * screen did nothing at all: only the encoder, the pads and the power
         * button — the inputs the STM scans itself — brought the backlight
         * back. It is registered separately from touchpad_acted_cb because
         * that one returns early on a board whose haptics failed to init. */
        lv_indev_add_event_cb(indev_touchpad, crosspad_gui::gui_touchpad_event_cb,
                              LV_EVENT_ALL, NULL);
        touch_trace_init(indev_touchpad);
        /* The twin replays what LVGL read, below the recogniser as well: it
         * runs the same recogniser over the same samples. */
        twin_mirror_attach(indev_touchpad);
        /* After the trace, so the trace still records what the panel reported
         * rather than what the recogniser rewrote. */
        crosspad_gui::touch_gesture_attach(indev_touchpad);
        bsp_touch_set_sensitivity((bsp_touch_sensitivity_t)settings->touchSensitivity);
    } else {
        ESP_LOGW("crosspad_main", "No touch input - haptic touch feedback disabled");
    }

    indev_encoderTop = lv_indev_create();
    lv_indev_set_type(indev_encoderTop, LV_INDEV_TYPE_ENCODER);
    lv_indev_set_read_cb(indev_encoderTop, encoderTop_read);
    twin_mirror_attach(indev_encoderTop);
    encoderTop_group = lv_group_create();
    lv_indev_set_group(indev_encoderTop, encoderTop_group);

    // setup keyboard driver for LVGL
    indev_keypad = lv_indev_create();
    lv_indev_set_type(indev_keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev_keypad, keypad_read);
    keypad_group = lv_group_create();
    lv_indev_set_group(indev_keypad, keypad_group);
    // make it possible to get the current pad from the driver callback "semi-globally"
    static int current_pad = -1;
    lv_indev_set_user_data(indev_keypad, &current_pad);
#else
    // P4 devboard: touch is registered by bsp_display_start(); set it as default encoder group input
    indev_touchpad = bsp_display_get_input_dev();
    encoderTop_group = lv_group_create();
    lv_group_set_default(encoderTop_group);
    lv_async_call([](void * p) {
        bsp_display_brightness_set((int)settings->LCDbrightness);
    }, NULL);
#endif

#ifdef BSP_BOARD_CROSSPAD_S3
    // Charge-while-off boot: STM repowered us only to show the feedback. Turn the
    // backlight on (the splash that normally does this does not play) and start
    // the gauge + "Charging" screen. Posted while the lock is still held: this
    // task is not the one running lv_timer_handler.
    if (s_charge_boot) {
        lv_async_call([](void *) {
            crosspad::getStm32CommandSender().sendBrightness((uint8_t)settings->LCDbrightness);
        }, NULL);
    }
#endif

    lvgl_port_unlock(); // unlock LVGL after setup

#ifdef BSP_BOARD_CROSSPAD_S3
    if (s_charge_boot) charge_overlay_start();
#endif

    // The splash, to its end: the launcher is built on the screen it leaves
    // black, so the build never shows as a stall in the animation -- and the
    // kit scan, which waits for the launcher, still starts after the engine
    // has its arena (main.cpp). Also until the codecs are up (the launcher
    // should appear with the pads audible; transfer and network mode have
    // none), never past kSplashMaxMs. A charge boot has neither.
    if (!s_charge_boot) {
        constexpr TickType_t kSplashMaxMs = 6000;
        while ((boot_splash_active() || (!AudioTasks_IsReady() && !usb_transfer_active() && !net_mode_active()))
               && (xTaskGetTickCount() - startTime) < pdMS_TO_TICKS(kSplashMaxMs)) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    LV_LOG_WARN("LVGL setup done successfully");
} // void setupLVGL()


void LVGLTask(void *pvParameters) // core 0
{
    status.currentGUI = CrosspadStatus::GUI_WELCOME;
    setupLVGL();

    // Initialize crosspad-gui framework (registers platform adapters)
    // MUST come before app registry — launcher_register_power_off needs getGuiPlatform()
    static IdfGuiPlatform guiPlatform;
    crosspad_gui::bootstrap(&guiPlatform);
    fw_view_service_install();   // the Firmware views: the app's and network mode's

    /* Transfer mode owns the screen: no launcher, no apps, nothing that could
     * open a file on the card the host is holding. The framework is still
     * bootstrapped above so the power-button gestures resolve as usual. */
    if (usb_transfer_active()) {
        lvgl_port_lock(3000);
        initStyles();
        usb_transfer_screen_create(lv_scr_act());
        if (!s_charge_boot) boot_splash_reveal();   // the splash left the panel dark
        lvgl_port_unlock();
        status.isGuiInitialised = true;
        status.currentGUI = CrosspadStatus::GUI_MAIN;
        lvgl_port_resume();
        while (true) vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* Network mode owns the screen the same way: no launcher, no apps, nothing
     * that would want the engine this boot did not start. */
    if (net_mode_active()) {
        lvgl_port_lock(3000);
        initStyles();
        net_mode_screen_create(lv_scr_act());
        if (!s_charge_boot) boot_splash_reveal();   // the splash left the panel dark
        lvgl_port_unlock();
        status.isGuiInitialised = true;
        status.currentGUI = CrosspadStatus::GUI_MAIN;
        lvgl_port_resume();
        while (true) vTaskDelay(pdMS_TO_TICKS(100));
    }

    InitializeAppRegistry(); // Initialize app registry once
    AddApps(); // add apps to the app list

    lvgl_port_lock(3000); // lock LVGL for setup
    initStyles(); // remember to deinitialize styles at the end
    lvgl_port_unlock(); // unlock LVGL after setup
    LoadMainScreen(lv_scr_act()); // GUI root screen creation
    status.isGuiInitialised = true; // LVGL is ready to use and loop is running
    /* The splash faded the panel out for the launcher to be built in the
     * dark; it is built. */
    if (!s_charge_boot) {
        lvgl_port_lock(3000);
        boot_splash_reveal();
        lvgl_port_unlock();
    }

    status.currentGUI = CrosspadStatus::GUI_MAIN;

    lvgl_port_resume(); // resume LVGL after setup

    while (true)
    {

        if (!lvgl_port_lock(3000)) // lock LVGL for setup
        {
            // LVGL is locked, do not perform any LVGL operations
            continue;
        }

        // mess with LVGL dynamically here //

        lvgl_port_unlock(); // unlock LVGL after setup

        vTaskDelay(10); // vTaskDelay to allow other tasks to run
    }
    crosspad_gui::shutdown(); // deinitialize styles and framework resources
    lv_deinit();
    vTaskDelete(NULL); // delete the task
} // void LVGLTask(void *pvParameters)

#endif // USE_LVGL

#endif // DISPLAY_H