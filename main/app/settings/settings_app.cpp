#ifdef USE_LVGL

#include "settings_app.h"
#include "gui.h"
#include "crosspad/settings/ISettingsUI.hpp"
#include "crosspad-gui/components/settings_ui.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-gui/components/midi_ble_panel.h"
#include "crosspad/app/AppRegistrar.hpp"
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "CrosspadPlatform.hpp"
#include "NvsSettingsStore.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "crosspad/stm32/reg_map.h"
#include "stm32_hal_init.h"
#include "crosspad_s3.h"
#include "crosspad/settings/InfoRowTable.hpp"
#include "crosspad_deps.h"
#include "EspBleMidi.hpp"
#include "processMIDI.hpp"
#include "imu_midi.h"
#include "crosspad/imu/ImuMidiMapper.hpp"
#include "stm_fw_overlay.h"
#include "wifi_page.h"
#include "usb_host_profile.h"
#include "cp_remote.h"
#include "settings_updates.h"
#include "esp_app_desc.h"
#include "crosspad/status/CrosspadStatus.hpp"
#include "esp_mac.h"
#include <cstdio>

// Submodule revisions injected by main/CMakeLists.txt (git short hashes).
#ifndef CROSSPAD_CORE_REV
#define CROSSPAD_CORE_REV "unknown"
#endif
#ifndef CROSSPAD_GUI_REV
#define CROSSPAD_GUI_REV "unknown"
#endif

extern lv_group_t * encoderTop_group;
extern lv_indev_t * indev_encoderTop;
extern CrosspadSettings * settings;
extern CrosspadStatus status;

static lv_group_t * s_settings_group = NULL;

// ---- ISettingsUI implementation ----

class NvsSettingsUI : public crosspad::ISettingsUI {
public:
    void saveSettings() override {
        if (settings) {
            settings->saveTo(NvsSettingsStore::getInstance());
        }
    }
    void exitToHome() override {
        // LoadMainScreen routes through AppOrchestrator::loadMainScreen,
        // which destroys the running app (us) and rebuilds the launcher
        // with a fresh encoder group + focus.
        LoadMainScreen(lv_scr_act());
    }
    const char* getPlatformName() override { return "CrossPad S3"; }
    const char* getPlatformInfo() override { return CONFIG_BSP_BOARD_REV_STR; }
    void enterStm32Dfu() override {
        crosspad::getStm32CommandSender().sendDfuRequest(true);
    }
    // The ROM bootloader reads no encoder or power button; the DFU screen's
    // tap lands here. A hardware reset boots the STM's app from the DFU
    // return marker in BKP0R.
    void leaveStm32Dfu() override {
        bsp_stm32_reset();
    }

    const char* getBackupDir() override { return "/sdcard"; }

    // Settings → Programming: what the STM32 runs vs the image this firmware
    // ships, and the button that starts the overlay's prompt/progress flow.
    bool getCoprocFwInfo(crosspad::CoprocFwInfo& out) override {
        stm32_fw_update_status_t st = {};
        stm32_fw_update_check(STM32_FW_BUNDLED_PATH, &st);
        if (!st.have_image) return false;
        if (st.running.linked) {
            snprintf(out.running, sizeof(out.running), "%u.%u (proto 0x%04X)",
                     st.running.fw_major, st.running.fw_minor, st.running.proto_version);
        } else {
            out.running[0] = '\0';
        }
        if (st.pcb_mismatch) {
            snprintf(out.bundled, sizeof(out.bundled), "v%u.%u for r%u, board r%u",
                     st.bundled.major, st.bundled.minor, st.bundled.pcb, st.running.pcb_rev);
        } else {
            snprintf(out.bundled, sizeof(out.bundled), "Bundled %u.%u (0x%04X)",
                     st.bundled.major, st.bundled.minor, st.bundled.proto);
        }
        out.due = st.due;
        out.canUpdate = st.running.linked && !st.battery_low && !st.pcb_mismatch;
        return true;
    }
    void requestCoprocFwUpdate() override {
        s_info.invalidate();   // Info rows re-read the STM32 identity next visit
        stm_fw_overlay_request();
    }

    // Info page rows. The page asks for the count once as it opens, so the
    // rows (the battery's among them) are read fresh per visit and the STM32
    // identity goes over I2C once per visit, not per redraw.
    int getInfoRowCount() override {
        s_info.invalidate();
        buildInfoRows();
        return s_info.count();
    }

    bool getInfoRow(int idx, const char** label, const char** value) override {
        buildInfoRows();
        return s_info.get(idx, label, value);
    }

private:
    static inline crosspad::InfoRowTable s_info;

    /* The cell and the charger as the STM sees them: "78 %  3.95 V", then
     * what the charger does with the current flowing into the cell. */
    static void addBatteryRows() {
        stm32_charger_telemetry_t t;
        if (stm32_hal_get_charger_telemetry(&t) != ESP_OK) return;
        const unsigned v = t.vbat_adc_mv;
        if (t.soc_flags & REG_SOC_FLAG_NO_BATTERY) {
            s_info.add("Battery", "%s", "not fitted");
        } else if ((t.soc_flags & REG_SOC_FLAG_VALID) && t.soc_pct <= 100) {
            s_info.add("Battery", "%u %%  %u.%02u V", t.soc_pct, v / 1000u, (v % 1000u) / 10u);
        } else {
            s_info.add("Battery", "%u.%02u V", v / 1000u, (v % 1000u) / 10u);
        }
        const unsigned in = t.vbus_mv;
        if (!(t.flags & REG_CHG_FLAG_PG)) {
            s_info.add("Charging", "%s", "no, on battery");
        } else if (t.chrg_stat == 1 || t.chrg_stat == 2) {
            s_info.add("Charging", "%u.%02u A %s, in %u.%02u V", t.ichg_ma / 1000u, (t.ichg_ma % 1000u) / 10u,
                       t.chrg_stat == 1 ? "pre" : "fast", in / 1000u, (in % 1000u) / 10u);
        } else if (t.chrg_stat == 3) {
            s_info.add("Charging", "%s", "done, full");
        } else {
            s_info.add("Charging", "plugged in, not charging (%u.%02u V)", in / 1000u, (in % 1000u) / 10u);
        }
    }

    static void buildInfoRows() {
        if (!s_info.beginBuild()) return;

        addBatteryRows();

        const esp_app_desc_t* app = esp_app_get_description();
        if (app) {
            s_info.add("Firmware", "%s", app->version);
            s_info.add("Built", "%s %s", app->date, app->time);
        }
        if (status.fwUpdateAvailable) {
            s_info.add("Update", "v%s at play.crosspad.app/device", status.fwLatest);
        }
        s_info.add("ESP-IDF", "%s", IDF_VER);
        s_info.add("crosspad-core", "%s %s", CROSSPAD_CORE_VERSION, CROSSPAD_CORE_REV);
        s_info.add("crosspad-gui", "%s %s", CROSSPAD_GUI_VERSION, CROSSPAD_GUI_REV);
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        s_info.add("Audio engine", "%s", "© Marcel Licence");
#endif

        // Installed apps with the commit and pin they were built from. Same
        // data the APP_VERSIONS CDC command reports, so a unit on a bench and
        // a unit in someone's hands answer the question identically.
        s_info.addAppVersions();

        uint8_t mac[6] = {0};
        if (esp_efuse_mac_get_default(mac) == ESP_OK) {
            s_info.add("MAC", "%02X%02X%02X%02X%02X%02X",
                       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        }

        // STM32 co-processor identity straight from its register map. Absent on
        // rev1 (no I2C control plane) — the rows are simply skipped there.
        auto* cs = crosspad::getStm32Manager().configSync();
        if (cs) {
            uint8_t sn[4] = {0};
            if (cs->readReg8(REG_IDENT_SERIAL_B3, sn[0]) && cs->readReg8(REG_IDENT_SERIAL_B2, sn[1]) &&
                cs->readReg8(REG_IDENT_SERIAL_B1, sn[2]) && cs->readReg8(REG_IDENT_SERIAL_B0, sn[3])) {
                uint32_t serial = ((uint32_t)sn[0] << 24) | ((uint32_t)sn[1] << 16) |
                                  ((uint32_t)sn[2] << 8) | sn[3];
                if (serial <= 9999u) s_info.add("Serial", "CP%04u", (unsigned)serial);
            }
            uint8_t major = 0, minor = 0, protoHi = 0, protoLo = 0, pcb = 0, boots = 0;
            if (cs->readReg8(REG_IDENT_FW_MAJOR, major) &&
                cs->readReg8(REG_IDENT_FW_MINOR, minor)) {
                s_info.add("STM32 firmware", "%u.%u", major, minor);
            }
            if (cs->readReg8(REG_IDENT_PROTO_HI, protoHi) &&
                cs->readReg8(REG_IDENT_PROTO_LO, protoLo)) {
                unsigned proto = (protoHi << 8) | protoLo;
                s_info.add("STM32 protocol", proto == STM32_PROTO_VERSION ? "0x%04X" : "0x%04X (expected 0x%04X)",
                           proto, (unsigned)STM32_PROTO_VERSION);
            }
            if (cs->readReg8(REG_IDENT_PCB_REV, pcb))  s_info.add("PCB revision", "%u", pcb);
            if (cs->readBootCount(boots))              s_info.add("STM32 boots", "%u", boots);
        } else {
            s_info.add("STM32 link", "%s", "UART (no I2C control)");
        }
    }
};

static NvsSettingsUI s_nvsSettingsUI;

void RegisterSettingsUIPlatform() {
    crosspad::setSettingsUI(&s_nvsSettingsUI);
}


// ---- BLE MIDI category ----
//
// crosspad-gui's page (midi_ble_panel.h) over EspBleMidi. Registered from the
// platform rather than built into crosspad-gui: only boards with a radio have
// anything to show here. The stack is started and stopped live, so the
// switch is the whole feature — no reboot to pick up a change.

static void ble_set_enabled(bool on) {
    auto* s = CrosspadSettings::getInstance();
    if (!s) return;
    if (on) {
        /* The radios are exclusive and remote management would keep WiFi up for
         * good: BLE asked for by hand wins, so Remote goes off (the page saves
         * the settings next). cp_remote's next pass (<= 1 s) releases its
         * lease; BLE then starts when the radio stops. */
        const bool remoteOff = cp_remote_wanted();
        if (remoteOff) s->wireless.enableRemote = false;
        /* The switch is the saved choice; while WiFi holds the radio the
         * start waits for it, so say when BLE will actually come. */
        const bool deferred = !bleMidi_start(s->wireless.bleMidiMode) && bleMidi_deferred_for_wifi();
        if (remoteOff)
            ui_toast_show("Remote management turned off: BLE and WiFi remote cannot run together", 4000,
                          UI_TOAST_INFO);
        else if (deferred)
            ui_toast_show("BLE starts when WiFi stops", 2500, UI_TOAST_INFO);
    } else {
        bleMidi_stop();
    }
}

static void ble_set_mode(uint8_t mode) {
    auto* s = CrosspadSettings::getInstance();
    // begin() tears the old role down first; a mode change while connected
    // therefore drops the link, which is the honest behaviour — a peripheral
    // and a central are not the same device to the peer.
    if (s && s->wireless.enableBLE) bleMidi_start(mode);
}

static const char* ble_state_name() {
    return bleMidi_deferred_for_wifi() ? "waiting for WiFi to stop" : crosspad::getEspBleMidi().getStateName();
}

// ---- Motion MIDI category ----

/* One callback for every control on the page: each row writes its own bound
 * field, then the mapper is re-read from the settings and the whole lot is
 * persisted. Going through applyTo() rather than poking the mapper here is what
 * keeps the UI, the CDC verbs and the boot path from each applying the table
 * slightly differently. */
static char s_imu_sent[24];

static void imu_midi_changed_cb(lv_event_t * e) {
    (void)e;
    auto* s = CrosspadSettings::getInstance();
    if (!s) return;
    imu_midi_set_enabled(s->imuMidi.enabled);
    crosspad::getSettingsUI()->saveSettings();
}

static void cat_build_imu_midi(lv_obj_t * parent, lv_group_t * group) {
    auto* s = CrosspadSettings::getInstance();
    if (!s) return;

    crosspad_gui::settings_section_header(parent, "Motion MIDI");
    crosspad_gui::settings_row_switch(parent, group, "Enabled",
                                      &s->imuMidi.enabled, imu_midi_changed_cb);

    snprintf(s_imu_sent, sizeof(s_imu_sent), "%u", (unsigned)imu_midi_sent());
    crosspad_gui::settings_row_info(parent, "Messages sent", s_imu_sent);

    /* Slot labels are static storage because the rows keep the pointer. Four
     * slots is the shape core defines; the loop reads kImuMidiSlots so adding a
     * fifth there does not need an edit here. */
    static char slot_names[crosspad::kImuMidiSlots][12];
    for (size_t i = 0; i < crosspad::kImuMidiSlots; ++i) {
        snprintf(slot_names[i], sizeof(slot_names[i]), "Slot %u", (unsigned)(i + 1));
        crosspad_gui::settings_section_header(parent, slot_names[i]);
        auto &slot = s->imuMidi.slots[i];
        crosspad_gui::settings_row_switch(parent, group, "On",
                                          &slot.enabled, imu_midi_changed_cb);
        crosspad_gui::settings_row_dropdown(parent, group, "Source",
                                            "Tilt front/back\nTilt left/right\n"
                                            "Turn X\nTurn Y\nTurn Z",
                                            &slot.source, imu_midi_changed_cb);
        crosspad_gui::settings_row_dropdown(parent, group, "Sends",
                                            "Control change\nPitch bend",
                                            &slot.target, imu_midi_changed_cb);
        crosspad_gui::settings_row_slider(parent, group, "CC number", 0, 127,
                                          &slot.cc, imu_midi_changed_cb);
        crosspad_gui::settings_row_slider(parent, group, "Channel", 0, 15,
                                          &slot.channel, imu_midi_changed_cb);
    }
}

static void register_imu_midi_category() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    crosspad_gui::SettingsCategory cat = { "Motion MIDI", LV_SYMBOL_GPS,
                                           cat_build_imu_midi };
    crosspad_gui::settings_ui_add_category(cat);
}

static void register_ble_category() {
    static bool registered = false;
    if (registered || !crosspad::EspBleMidi::isSupported()) return;
    registered = true;
    crosspad_gui::MidiBleCallbacks cb;
    cb.setEnabled      = ble_set_enabled;
    cb.setMode         = ble_set_mode;
    cb.applyTxOffset   = bleMidi_applyNoteOffsets;
    cb.stateName       = ble_state_name;
    cb.connectedDevice = [] { return crosspad::getEspBleMidi().getConnectedDevice(); };
    cb.scanResults     = [] { return crosspad::getEspBleMidi().getScanResults(); };
    cb.startScan       = [] { crosspad::getEspBleMidi().startScan(8000); };
    cb.connect         = [](const std::string& address) { crosspad::getEspBleMidi().connectToDevice(address); };
    cb.disconnect      = [] { crosspad::getEspBleMidi().disconnect(); };
    crosspad_gui::midi_ble_set_callbacks(cb);
    crosspad_gui::SettingsCategory cat = { "BLE MIDI", LV_SYMBOL_BLUETOOTH, crosspad_gui::midi_ble_settings_build };
    crosspad_gui::settings_ui_add_category(cat);
}

// ---- USB host category ----
//
// The computer USB audio is tuned for (usb_host_profile.h). The dropdown's
// order is usb_host_mode_t's.
static uint8_t s_usb_host_idx;
static char s_usb_host_seen[24];

static void usb_host_changed_cb(lv_event_t * e) {
    (void)e;
    usb_host_profile_set_mode((usb_host_mode_t)s_usb_host_idx);
}

static void cat_build_usb_host(lv_obj_t * parent, lv_group_t * group) {
    s_usb_host_idx = (uint8_t)usb_host_profile_mode();
    crosspad_gui::settings_section_header(parent, "USB audio");
    crosspad_gui::settings_row_dropdown(parent, group, "Computer", "Auto\nmacOS\nLinux",
                                        &s_usb_host_idx, usb_host_changed_cb);
    snprintf(s_usb_host_seen, sizeof(s_usb_host_seen), "%s",
             usb_host_seen_name(usb_host_profile_detected()));
    crosspad_gui::settings_row_info(parent, "Detected", s_usb_host_seen);
}

static void register_usb_host_category() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    crosspad_gui::SettingsCategory cat = { "USB host", LV_SYMBOL_USB, cat_build_usb_host };
    crosspad_gui::settings_ui_add_category(cat);
}

// ---- WiFi category ----
//
// The shared WiFi page (wifi_page.h). Open = a lease (spec §3), so the status
// line has a link to talk about; a request refused for memory offers a restart
// into network mode, once per visit.
static void cat_build_wifi(lv_obj_t * parent, lv_group_t * group) {
    static const wifi_page_opts_t kOpts = {true, true};
    wifi_page_build(parent, group, &kOpts);
}

static void register_wifi_category() {
    static bool registered = false;
    if (registered) return;
    registered = true;
    crosspad_gui::SettingsCategory cat = { "WiFi", LV_SYMBOL_WIFI, cat_build_wifi };
    crosspad_gui::settings_ui_add_category(cat);
}

lv_obj_t * lv_CreateSettings(lv_obj_t * parent, App * a) {
    // Create encoder group for this app
    s_settings_group = lv_group_create();
    lv_indev_set_group(indev_encoderTop, s_settings_group);

    // Main container (fills the app content area)
    lv_obj_t * container = lv_obj_create(parent);
    lv_obj_set_size(container, lv_obj_get_content_width(parent), lv_obj_get_content_height(parent));
    lv_obj_set_style_bg_color(container, lv_color_black(), 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, 0, 0);
    lv_obj_set_style_radius(container, 0, 0);
    lv_obj_center(container);

    // Scrollable content area
    lv_obj_t * content = lv_obj_create(container);
    /* No theme on the list: its scrollbar fade is a style transition, and
     * every step of one invalidates the whole object -- thirteen full-list
     * redraws per encoder step measured. Plain scrollbar instead. */
    lv_obj_remove_style_all(content);
    lv_obj_set_style_width(content, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(content, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(content, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(content, lv_color_make(128, 128, 128), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(content, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_size(content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(content, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 6, 0);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_set_style_radius(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    // Delegate to shared settings UI
    register_imu_midi_category();
    register_ble_category();
    register_wifi_category();
    register_usb_host_category();
    settings_updates_register();
    crosspad_gui::settings_ui_create(content, s_settings_group);

    // Lifecycle callbacks
    a->setOnShow([](lv_obj_t * app) {
        lv_indev_set_group(indev_encoderTop, s_settings_group);
    });

    a->setOnHide([](lv_obj_t * app) {
        lv_indev_set_group(indev_encoderTop, encoderTop_group);
    });

    /* Power-button back: one level up inside Settings, and only when there is
     * a level to pop — from the category list the framework closes the app. */
    a->setOnBack([](lv_obj_t *) { return crosspad_gui::settings_ui_back(); });

    return container;
}

void lv_DestroySettings(lv_obj_t * obj) {
    crosspad_gui::settings_ui_destroy();

    if (s_settings_group) {
        lv_group_delete(s_settings_group);
        s_settings_group = NULL;
    }

    if (obj) lv_obj_delete_async(obj);
}

REGISTER_APP(Settings, nullptr, "settings.png",
             lv_CreateSettings, lv_DestroySettings, nullptr, nullptr, nullptr, 10);

#endif // USE_LVGL
