#pragma once

// Encoder telemetry in the status bar (detent index and the step it produced).
// Off by default; set to 1 in the build while tuning the encoder.
#ifndef CROSSPAD_ENCODER_DEBUG
#define CROSSPAD_ENCODER_DEBUG 0
#endif
#include "CrossPad.h"
#include "driver/gpio.h"
#include "lvgl.h"
#include "CrosspadPlatform.hpp"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-gui/theme/crosspad_theme.h"
#include "crosspad-gui/widgets/gui_helpers.h"
#include "indev/encoder_hil.h"
#include <cstdio>

extern lv_group_t *encoderTop_group;
extern lv_group_t *encoderLeft_group;

extern lv_indev_t *indev_encoderLeft;
extern lv_indev_t *indev_encoderTop;

void encoderTop_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    auto& handler = crosspad::getStm32MessageHandler();
    /* One detent is one step, and nothing else in here moves the focus. The
     * steps are decoded from the knob's A/B lines as every report arrives
     * (EncoderDetents, in the handler): the STM's count gains counts -- one
     * click was seven of them -- and a step read from it was two. An
     * auto-repeat that used to live here -- the list scrolling on while the
     * knob sat between detents -- is gone too: a detent is a decision
     * somebody made with their fingers. */
    const int ddiff = handler.takeEncoderSteps();

#if CROSSPAD_ENCODER_DEBUG
    // Compact debug telemetry in the status-bar icon flex zone (smallest font,
    // theme-derived colour so it follows the active palette). Off by default:
    // it shares the zone with real status icons and overlaps the centred logo.
    // Build with -DCROSSPAD_ENCODER_DEBUG=1 while tuning the detent logic.
    static char enc_dbg_buf[40] = "";
    static bool enc_dbg_inited  = false;
    std::snprintf(enc_dbg_buf, sizeof(enc_dbg_buf), "d%d %+d",
                  handler.encoderDetent() & 0x0F, ddiff);
    if (!enc_dbg_inited) {
        const lv_color_t text_col =
            lv_color_bw_from_color(CROSSPAD_THEME_PRIMARY);
        if (crosspad_gui::statusbar_add_icon(
                "enc.dbg", enc_dbg_buf, text_col,
                crosspad_gui::StatusIconSide::Left)) {
            crosspad_gui::statusbar_set_icon_font("enc.dbg",
                                                  &lv_font_montserrat_10);
            enc_dbg_inited = true;
        }
    } else {
        crosspad_gui::statusbar_set_icon_content("enc.dbg", enc_dbg_buf);
    }

#endif // CROSSPAD_ENCODER_DEBUG

    // LVGL handles enc_diff + state natively for LV_INDEV_TYPE_ENCODER:
    // Navigation mode: enc_diff moves focus between group objects
    // Edit mode: enc_diff changes the focused widget's value
    // Press: enters edit mode (or clicks buttons)
    // Steps are + where the STM's count rises (one physical direction);
    // negate so GUI focus moves the way the user expects (matches the prior
    // last-minus-current convention this rewrite replaced).
    data->enc_diff = -ddiff + encoderTop_hil_diff().exchange(0);

    const uint32_t now_ms = lv_tick_get();
    const bool hil_held = (int32_t)(encoderTop_hil_press_until().load() - now_ms) > 0;
    data->state = (!gpio_get_level(BSP_BUTTON_2) || hil_held) ? LV_INDEV_STATE_PRESSED
                                                             : LV_INDEV_STATE_RELEASED;
}

#ifdef CrossPad_REV1
#define HAS_SECOND_ENCODER
void encoderLeft_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    auto& handler = crosspad::getStm32MessageHandler();
    // encoder has values from 0 to 127
    static int8_t last_encoder = handler.input.encoderLeft; // initial value
    int8_t encoder = handler.input.encoderLeft;
    int8_t diff = encoder - last_encoder;
    // overflow fix
    if (diff > 64) diff -= 128;
    if (diff < -64) diff += 128;
    data->enc_diff = diff;
    last_encoder = encoder;

    data->state = LV_INDEV_STATE_REL; // encoder button disabled
}
#endif // CrossPad_REV1
