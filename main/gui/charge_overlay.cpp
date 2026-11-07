/**
 * @file charge_overlay.cpp
 * @brief Charge-while-off UI — pad battery gauge + "Charging" + battery%% screen.
 *
 * The STM repowers the ESP on PWR_STATE_CHARGE (charger plugged while
 * soft-off). We drive the pad gauge (PadAnimator CHARGING) and show a
 * "Charging" + NN%% screen for ~5 s before fading it to black; the pads keep
 * animating after that.
 *
 * A charger plugged in while the board is on shows the same screen for the
 * same ~5 s (charge_overlay_peek), over whatever is running, and then the
 * whole overlay fades away; a tap closes it early. No pad gauge then.
 *
 * Spec: docs 2026-06-10-charge-while-off-animation-design.
 */

#ifdef USE_LVGL

#include "charge_overlay.h"
#include "gui.h"
#include "lvgl.h"
#include "lv_async_safe.h"
#include "esp_log.h"
#include <cstdio>

#include "CrosspadPlatform.hpp"          // getPadAnimator / getPadLedController
#include "crosspad/pad/PadAnimator.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "stm32_hal_init.h"              // stm32_hal_get_charger_telemetry
#include "stm32_reg_map.h"
#include "stm32_i2c.h"                   // stm32_i2c_read_u8 (PCB revision)

static const char *TAG = "charge_overlay";

// ── State ───────────────────────────────────────────────────────────────

static volatile bool s_active = false;
static volatile bool s_peek = false;      // plugged in while on: screen only
static uint32_t      s_tick = 0;          // animation-task ticks (~10 ms) since last sample
static crosspad::PadAnimator::AnimToken s_anim = crosspad::PadAnimator::kNoAnim;  // the pad gauge

// LVGL objects (touched only on the LVGL task via lv_async_call_locked)
static lv_obj_t * s_overlay   = nullptr;   // opaque black full-screen, stays up
static lv_obj_t * s_content   = nullptr;   // holds the text, fades out after hold
static lv_obj_t * s_title_label = nullptr; // Charging / Charged / Not charging
static lv_obj_t * s_pct_label = nullptr;
static lv_obj_t * s_info_label = nullptr;  // U / I / IN detail line(s)
static lv_timer_t * s_dismiss = nullptr;

static const uint32_t SCREEN_HOLD_MS = 5000;   // visible time before fade
static const uint32_t FADE_MS        = 1000;   // fade-to-black duration
static const uint32_t SAMPLE_TICKS   = 100;    // 100 × 10 ms ≈ 1 s battery re-read
static const uint8_t  CHARGE_PAD_BRIGHTNESS = 64;   // ~1/4 — dim while charging
static const uint8_t  CHARGE_SCREEN_BRIGHTNESS = 25; // ~1/4 LCD backlight (0..100)

/* Screen labels. Single-writer (charge task), read on the LVGL task at ~1 s,
 * so a torn read costs one stale field for one frame. */
struct charge_scr_t {
    uint8_t  pct;
    uint16_t vbat_mv;   // U (battery)
    uint16_t vbus_mv;   // IN (charger input)
    uint16_t ichg_ma;   // I (charge current)
    uint8_t  state;     // CHG_UI_*
};
static volatile charge_scr_t s_scr;

// ── Battery model ───────────────────────────────────────────────────────

/* Monotonic LiPo VBAT→SoC lookup, 4.20 V→100% down to 3.30 V→0%. Gauge only;
 * "charge done" comes from chrg_stat. */
static float vbat_to_soc(uint16_t mv) {
    static const struct { uint16_t mv; float soc; } lut[] = {
        { 3300, 0.00f }, { 3500, 0.05f }, { 3700, 0.15f }, { 3800, 0.30f },
        { 3950, 0.50f }, { 4100, 0.75f }, { 4200, 1.00f },
    };
    const int n = sizeof(lut) / sizeof(lut[0]);
    if (mv <= lut[0].mv)     return 0.0f;
    if (mv >= lut[n-1].mv)   return 1.0f;
    for (int i = 1; i < n; i++) {
        if (mv <= lut[i].mv) {
            float span = (float)(lut[i].mv - lut[i-1].mv);
            float f = (float)(mv - lut[i-1].mv) / span;
            return lut[i-1].soc + (lut[i].soc - lut[i-1].soc) * f;
        }
    }
    return 1.0f;
}

// Charge UI state, mirrored into s_scr.state for the screen title.
enum { CHG_UI_IDLE = 0, CHG_UI_CHARGING = 1, CHG_UI_DONE = 2 };

static float s_soc_ema = 0.0f;
static bool  s_soc_ema_init = false;

/* Read the pack; false (outputs untouched) when the STM link is down.
 * *done = BQ termination. *charging = current is actually flowing, not merely
 * plugged, so the UI cannot claim to charge a device that is not. */
static bool read_battery(float *soc_out, bool *done_out, bool *charging_out, uint8_t *pct_out) {
    stm32_charger_telemetry_t t;
    if (stm32_hal_get_charger_telemetry(&t) != ESP_OK) return false;
    /* Prefer the STM's direct ADC; fall back to BQ BATV if it reads <2.5 V. */
    uint16_t vbat_mv = (t.vbat_adc_mv >= 2500) ? t.vbat_adc_mv : t.vbat_mv;

    bool done     = (t.chrg_stat == 3);   // CHRG_ST == ChargeDone
    bool charging = (t.flags & REG_CHG_FLAG_PG) && (t.chrg_stat == 1 || t.chrg_stat == 2);

    float soc;
    if ((t.soc_flags & REG_SOC_FLAG_VALID) && t.soc_pct <= 100) {
        /* STM EKF SoC: IR-drop-compensated and filtered, so no EMA. Under load
         * the terminal voltage sags and a naive curve reads far too low. */
        soc = t.soc_permil / 1000.0f;
    } else {
        /* Firmware without the SoC estimator: naive curve + ~16 s EMA, because
         * charge-current pulses swing the terminal ~30 mV per pad. */
        float soc_raw = vbat_to_soc(vbat_mv);
        if (done) soc_raw = 1.0f;
        if (!s_soc_ema_init) { s_soc_ema = soc_raw; s_soc_ema_init = true; }
        else                 { s_soc_ema += 0.06f * (soc_raw - s_soc_ema); }
        soc = s_soc_ema;
    }
    /* Plugged into a full cell the BQ does not start a new cycle and reports
     * "not charging" (0), not termination — the screen said "Not charging"
     * at 99 %. Power good, no current, SoC ≥ 95 % is a charged pack. */
    if (!done && !charging && (t.flags & REG_CHG_FLAG_PG) && t.chrg_stat == 0 &&
        soc >= 0.95f) {
        done = true;
    }
    if (done) soc = 1.0f;   // "Charged" → show full regardless of source

    *soc_out      = soc;
    *done_out     = done;
    *charging_out = charging;
    *pct_out      = (uint8_t)(soc * 100.0f + 0.5f);

    s_scr.pct     = *pct_out;
    s_scr.vbat_mv = vbat_mv;
    s_scr.vbus_mv = t.vbus_mv;
    s_scr.ichg_ma = t.ichg_ma;
    s_scr.state   = done ? CHG_UI_DONE : (charging ? CHG_UI_CHARGING : CHG_UI_IDLE);
    return true;
}

static const char *charge_title_text(void) {
    switch (s_scr.state) {
        case CHG_UI_CHARGING: return "Charging";
        case CHG_UI_DONE:     return "Charged";
        default:              return "Not charging";
    }
}

/* Cached PCB revision. Rev <= 20 has a one-directional STM-side USB power
 * switch, so charging works only from the top (ESP) port — surfaced as a hint
 * when plugged but not charging. 0 = unknown, no hint. */
static uint8_t s_pcb_rev = 0;
static bool stm_charge_top_port_only(void) {
    return s_pcb_rev != 0 && s_pcb_rev <= 20;
}

/* "U 4.05V  I 0.48A\nIN 5.00V" while charging, else the top-port hint.
 * Static buffer — LVGL copies it. */
static const char *charge_info_text(void) {
    if (s_scr.state == CHG_UI_IDLE && stm_charge_top_port_only()) {
        return "Charge only from\ntop (ESP) port";
    }
    static char buf[48];
    snprintf(buf, sizeof(buf), "U %u.%02uV  I %u.%02uA\nIN %u.%02uV",
             s_scr.vbat_mv / 1000u, (s_scr.vbat_mv % 1000u) / 10u,
             s_scr.ichg_ma / 1000u, (s_scr.ichg_ma % 1000u) / 10u,
             s_scr.vbus_mv / 1000u, (s_scr.vbus_mv % 1000u) / 10u);
    return buf;
}

// ── LVGL screen (runs on the LVGL task) ─────────────────────────────────

static void opa_exec_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* Drop the faded content but keep the opaque black overlay, so the launcher
 * underneath is never revealed. Removed only by charge_overlay_stop(). */
static void fade_done_cb(lv_anim_t *a) {
    (void)a;
    if (s_content) { lv_obj_delete(s_content); s_content = nullptr; s_title_label = nullptr; s_pct_label = nullptr; s_info_label = nullptr; }
}

static void forget_screen(void) {
    s_overlay = nullptr;
    s_content = nullptr;
    s_title_label = nullptr;
    s_pct_label = nullptr;
    s_info_label = nullptr;
}

/* A peek leaves nothing behind: the black overlay goes too. Async, as this
 * runs from the overlay's own click or fade. */
static void peek_close(void) {
    if (s_dismiss) { lv_timer_delete(s_dismiss); s_dismiss = nullptr; }
    if (s_overlay) {
        lv_anim_delete(s_overlay, opa_exec_cb);
        lv_obj_delete_async(s_overlay);
    }
    forget_screen();
    s_peek = false;
}

static void peek_fade_done_cb(lv_anim_t *a) {
    (void)a;
    peek_close();
}

static void peek_click_cb(lv_event_t *e) {
    (void)e;
    if (s_peek) peek_close();
}

static void dismiss_timer_cb(lv_timer_t *timer) {
    (void)timer;
    s_dismiss = nullptr;
    lv_obj_t *target = s_peek ? s_overlay : s_content;
    if (!target) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, target);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FADE_MS);
    lv_anim_set_exec_cb(&a, opa_exec_cb);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_completed_cb(&a, s_peek ? peek_fade_done_cb : fade_done_cb);
    lv_anim_start(&a);
}

static void show_screen_cb(void *param) {
    uint8_t pct = (uint8_t)(uintptr_t)param;

    if (s_overlay) {            // already up — just refresh the readings + timer
        if (!s_peek) {          // a peek became charge mode: keep the black overlay
            lv_anim_delete(s_overlay, opa_exec_cb);
            lv_obj_set_style_opa(s_overlay, LV_OPA_COVER, 0);
        }
        if (s_title_label) lv_label_set_text(s_title_label, charge_title_text());
        if (s_pct_label) lv_label_set_text_fmt(s_pct_label, "%d%%", pct);
        if (s_info_label) lv_label_set_text(s_info_label, charge_info_text());
        if (s_dismiss) lv_timer_reset(s_dismiss);
        return;
    }

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_set_style_pad_all(s_overlay, 0, 0);
    lv_obj_center(s_overlay);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, peek_click_cb, LV_EVENT_CLICKED, nullptr);

    // Content layer (transparent) — fades out after the hold, leaving black.
    s_content = lv_obj_create(s_overlay);
    lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_center(s_content);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    s_title_label = lv_label_create(s_content);
    lv_label_set_text(s_title_label, charge_title_text());
    lv_obj_set_style_text_color(s_title_label, CROSSPAD_THEME_PRIMARY, 0);
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_title_label, LV_ALIGN_CENTER, 0, -44);

    s_pct_label = lv_label_create(s_content);
    lv_label_set_text_fmt(s_pct_label, "%d%%", pct);
    lv_obj_set_style_text_color(s_pct_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_pct_label, &lv_font_montserrat_16, 0);
    lv_obj_align(s_pct_label, LV_ALIGN_CENTER, 0, -16);

    s_info_label = lv_label_create(s_content);
    lv_label_set_text(s_info_label, charge_info_text());
    lv_obj_set_style_text_color(s_info_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_info_label, LV_ALIGN_CENTER, 0, 22);

    s_dismiss = lv_timer_create(dismiss_timer_cb, SCREEN_HOLD_MS, nullptr);
    lv_timer_set_repeat_count(s_dismiss, 1);
}

static void update_label_cb(void *param) {
    uint8_t pct = (uint8_t)(uintptr_t)param;
    if (s_title_label) lv_label_set_text(s_title_label, charge_title_text());
    if (s_pct_label) lv_label_set_text_fmt(s_pct_label, "%d%%", pct);
    if (s_info_label) lv_label_set_text(s_info_label, charge_info_text());
}

static void hide_screen_cb(void *param) {
    (void)param;
    if (s_dismiss) { lv_timer_delete(s_dismiss); s_dismiss = nullptr; }
    if (s_content) lv_anim_delete(s_content, opa_exec_cb);   // cancel any in-flight fade
    if (s_overlay) {
        lv_obj_delete(s_overlay);   // deletes the content child too
        forget_screen();
    }
}

/* Bare-PCB bring-up: with no display registered, every lv_* call below hits
 * LV_ASSERT_NULL, whose handler is `while(1);` — a hang, not an error. Keep
 * the pad gauge, skip the screen. */
static inline bool charge_overlay_have_display(void) {
    return lv_display_get_default() != NULL;
}

// ── Public API ──────────────────────────────────────────────────────────

void charge_overlay_start(void) {
    if (s_pcb_rev == 0) stm32_i2c_read_u8(REG_IDENT_PCB_REV, &s_pcb_rev);  // cache once
    if (!s_active) s_soc_ema_init = false;   // re-seed the EMA for a fresh session
    float soc = 0.0f; bool done = false, charging = false; uint8_t pct = 0;
    bool ok = read_battery(&soc, &done, &charging, &pct);

    if (!s_active) {
        s_peek = false;   // a peek on screen stays up as the charge screen
        s_anim = crosspad::getPadAnimator().startCharging(ok ? soc : 0.0f);
        crosspad::getPadAnimator().setChargingActive(charging || done);
        crosspad::getPadLedController().setBrightness(CHARGE_PAD_BRIGHTNESS);
        crosspad::getStm32CommandSender().sendBrightness(CHARGE_SCREEN_BRIGHTNESS);  // dim LCD ~1/4
        s_active = true;
        s_tick = 0;
        ESP_LOGI(TAG, "charge mode on (vbat→%d%%, charging=%d, done=%d)", pct, charging, done);
        if (charge_overlay_have_display()) lv_async_call_locked(show_screen_cb, (void *)(uintptr_t)pct);
    } else if (ok) {
        crosspad::getPadAnimator().setChargePercent(soc);
        crosspad::getPadAnimator().setChargingActive(charging || done);
        if (charge_overlay_have_display()) lv_async_call_locked(show_screen_cb, (void *)(uintptr_t)pct);  // refresh + restart timer
    }
}

void charge_overlay_peek(void) {
    if (s_active || s_peek || !charge_overlay_have_display()) return;
    if (s_pcb_rev == 0) stm32_i2c_read_u8(REG_IDENT_PCB_REV, &s_pcb_rev);  // cache once
    s_soc_ema_init = false;
    float soc = 0.0f; bool done = false, charging = false; uint8_t pct = 0;
    if (!read_battery(&soc, &done, &charging, &pct)) return;
    s_peek = true;
    s_tick = 0;
    ESP_LOGI(TAG, "charger plugged in (%d%%, charging=%d, done=%d)", pct, charging, done);
    lv_async_call_locked(show_screen_cb, (void *)(uintptr_t)pct);
}

void charge_overlay_tick(void) {
    if (!s_active && !s_peek) return;
    if (++s_tick < SAMPLE_TICKS) return;
    s_tick = 0;

    float soc = 0.0f; bool done = false, charging = false; uint8_t pct = 0;
    if (!read_battery(&soc, &done, &charging, &pct)) return;
    if (s_active) {
        crosspad::getPadAnimator().setChargePercent(soc);
        crosspad::getPadAnimator().setChargingActive(charging || done);
    }
    if (charge_overlay_have_display()) lv_async_call_locked(update_label_cb, (void *)(uintptr_t)pct);
}

void charge_overlay_stop(void) {
    if (!s_active) return;
    s_active = false;
    s_soc_ema_init = false;
    crosspad::getPadAnimator().stop(s_anim);
    s_anim = crosspad::PadAnimator::kNoAnim;
    ESP_LOGI(TAG, "charge mode off");
    if (charge_overlay_have_display()) lv_async_call_locked(hide_screen_cb, nullptr);
}

bool charge_overlay_active(void) { return s_active; }

#endif // USE_LVGL
