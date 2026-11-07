#ifdef USE_LVGL

#include "usb_transfer_screen.h"

#include <stdio.h>

#include "usb_transfer.h"
#include "crosspad-gui/components/power_gesture.h"
#include "crosspad-gui/components/ui_modal.h"

extern lv_group_t * encoderTop_group;

/* The MSC callbacks run on the TinyUSB task and only touch atomics; this timer
 * is the single reader, on the LVGL thread. Nothing here goes near the card. */
static constexpr uint32_t kPollMs = 250;
static constexpr int      kScreenPad = 10;
static constexpr int      kRowGap = 6;
static constexpr int      kExitBtnHeight = 34;
static constexpr int      kContentWidthPct = 100;
static constexpr uint64_t kKiBPerMiB = 1024;
static constexpr uint64_t kBytesPerGiB = 1024ull * 1024 * 1024;

static lv_obj_t * s_state_label;
static lv_obj_t * s_moved_label;

/* One fractional digit out of a binary unit, without floating point. */
static unsigned tenths(uint64_t value, uint64_t unit)
{
    return (unsigned)((value % unit) * 10u / unit);
}

static const char * state_text(usb_transfer_state_t state)
{
    switch (state) {
        case USB_TRANSFER_NO_CARD:  return "No SD card";
        case USB_TRANSFER_WAITING:  return "Waiting for the computer";
        case USB_TRANSFER_MOUNTED:  return "Open on the computer";
        case USB_TRANSFER_BUSY:     return "Transferring...";
        case USB_TRANSFER_EJECTED:  return "Ejected - safe to exit";
        default:                    return "";
    }
}

static lv_color_t state_color(usb_transfer_state_t state)
{
    switch (state) {
        case USB_TRANSFER_NO_CARD:  return lv_palette_main(LV_PALETTE_RED);
        case USB_TRANSFER_BUSY:     return lv_palette_main(LV_PALETTE_AMBER);
        case USB_TRANSFER_EJECTED:  return lv_palette_main(LV_PALETTE_GREEN);
        default:                    return lv_color_white();
    }
}

static void leave_confirmed_cb(bool confirmed, void * user_data)
{
    (void)user_data;
    if (confirmed) usb_transfer_leave();
}

/* Shared by the touch button, the encoder and the power-button click. */
static void request_exit(void)
{
    usb_transfer_status_t st;
    usb_transfer_get_status(&st);
    if (st.state == USB_TRANSFER_BUSY) {
        ui_modal_confirm("Still transferring",
                         "The computer may still be writing. Eject the drive there first.",
                         "Leave anyway", "Wait", leave_confirmed_cb, nullptr, encoderTop_group);
        return;
    }
    usb_transfer_leave();
}

static void exit_clicked_cb(lv_event_t * e)
{
    (void)e;
    request_exit();
}

static bool exit_back_cb(void * user_data)
{
    (void)user_data;
    request_exit();
    return true;
}

static void poll_cb(lv_timer_t * t)
{
    (void)t;
    usb_transfer_status_t st;
    usb_transfer_get_status(&st);

    lv_label_set_text(s_state_label, state_text(st.state));
    lv_obj_set_style_text_color(s_state_label, state_color(st.state), 0);

    char moved[48];
    snprintf(moved, sizeof(moved), "Read %lu.%u MB   Written %lu.%u MB",
             (unsigned long)(st.readKiB / kKiBPerMiB), tenths(st.readKiB, kKiBPerMiB),
             (unsigned long)(st.writeKiB / kKiBPerMiB), tenths(st.writeKiB, kKiBPerMiB));
    lv_label_set_text(s_moved_label, moved);
}

static lv_obj_t * add_label(lv_obj_t * parent, const char * text, const lv_font_t * font,
                            lv_color_t color)
{
    lv_obj_t * label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, LV_PCT(kContentWidthPct));
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    return label;
}

void usb_transfer_screen_create(lv_obj_t * parent)
{
    usb_transfer_status_t st;
    usb_transfer_get_status(&st);

    lv_obj_t * root = lv_obj_create(parent);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_black(), 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_set_style_pad_all(root, kScreenPad, 0);
    lv_obj_set_style_pad_row(root, kRowGap, 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    add_label(root, "USB Transfer", &lv_font_montserrat_16, lv_color_white());
    add_label(root,
              st.cardPresent
                  ? "The SD card is on your computer. Copy kit folders onto it, "
                    "then eject it there."
                  : "No SD card. Put one in and start transfer mode again.",
              &lv_font_montserrat_10, lv_palette_main(LV_PALETTE_GREY));

    if (st.cardPresent) {
        char card[40];
        snprintf(card, sizeof(card), "Card: %lu.%u GB",
                 (unsigned long)(st.cardBytes / kBytesPerGiB),
                 tenths(st.cardBytes, kBytesPerGiB));
        add_label(root, card, &lv_font_montserrat_10, lv_palette_main(LV_PALETTE_GREY));
    }

    s_state_label = add_label(root, state_text(st.state), &lv_font_montserrat_14,
                              state_color(st.state));
    s_moved_label = add_label(root, "", &lv_font_montserrat_10,
                              lv_palette_main(LV_PALETTE_GREY));

    lv_obj_t * exit_btn = lv_button_create(root);
    lv_obj_set_width(exit_btn, LV_PCT(kContentWidthPct));
    lv_obj_set_height(exit_btn, kExitBtnHeight);
    lv_obj_add_event_cb(exit_btn, exit_clicked_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t * exit_label = lv_label_create(exit_btn);
    lv_label_set_text(exit_label, "Exit and restart");
    lv_obj_center(exit_label);

    /* The launcher's group is free in this mode — there is no launcher — so the
     * encoder reaches the only control on the screen without a group of its
     * own. The power-button click gets there through the overlay back stack,
     * which also means an open confirm dialog is cancelled first. */
    if (encoderTop_group) lv_group_add_obj(encoderTop_group, exit_btn);
    crosspad_gui::ui_back_push(exit_back_cb, nullptr);

    lv_timer_create(poll_cb, kPollMs, nullptr);
    poll_cb(nullptr);
}

#endif // USE_LVGL
