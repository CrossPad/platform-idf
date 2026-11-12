/* CDC verbs: BLE MIDI, TRS MIDI and DAW control. Dispatch order is in hil_control.cpp. */
#include "sd_bench.h"
#include "sd_sector_io.h"
#include "audio_route_control.h"
#include "esp_attr.h"
#include "esp_heap_trace.h"
#include "esp_memory_utils.h"
#include "bsp_worker.h"
#include "i2s_driver.h"
#include "lat_trace.h"
#include "include/hil_control.h"
#include "include/hil_text.h"
#include "crosspad/instrument/InstrumentPlayState.hpp"
#include "crosspad/control/AppName.hpp"
#include "include/usb_serial_prompt.h"
#include "include/usb_transfer.h"
#include "usb_audio_port.h"
#include "usb_device_uac.h"
#include "include/usb_host_profile.h"
#include "include/net_mode.h"
#include "include/net_mode_screen.h"
#include "include/fw_library.h"
#include "include/fw_slots.h"
#include "include/kit_convert_port.h"
#include "include/ota_cdc.h"
#include "fw_flow_overlay.h"
#include "usb_config_manager.h"
#include "esp_app_desc.h"
#include "crosspad/app/AppVersions.hpp"
#include "EspDspKernels.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "SequenceStorePort.hpp"
#include "include/crosspad_deps.h"
#include "include/reset_log.h"
#include "stm32_hal_init.h"
#include "stm32_fw_update.h"
#include "stm32_i2c.h"
#include "stm32_uart.h"
#include "stm32_reg_map.h"
#include "midi_loop_test.h"

#include <algorithm>
#include <atomic>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <memory>
#include <new>

#include "esp_attr.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "lvgl.h"
#include "src/core/lv_refr_private.h"      /* SCREENSHOT strips: lv_snapshot's own layer redraw */
#include "src/display/lv_display_private.h"
#include "crosspad_s3.h"   /* bsp_display_get_input_dev() for UI_STATE touch= */
#include "include/lv_async_safe.h"
#include "crosspad-gui/components/settings_ui.h"
#include "include/power_button.h"
#include "include/twin_mirror.h"
#include "include/twin_state.h"
#include "crosspad/settings/ISettingsUI.hpp"
#include "crosspad-gui/platform/IGuiPlatform.h"
#include "include/host_info.h"
#include "include/fw_updater.h"
#include "include/cp_account.h"
#include "include/cp_remote.h"
#include "include/fw_ui_state.h"
#include "include/wifi_verbs.h"
#include "include/wifi_creds.h"
#include "include/lvgl_stats.h"

#include "gui.h"
#include "audio/audio_task.h"
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad-mixer/MixerPresets.hpp>
#define HIL_HAS_MIXER 1
#endif
#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/components/mode_overlay.h"
#include "CrosspadPlatform.hpp"
#include "crosspad/pad/PadManager.hpp"
#include "crosspad/pad/PadLedController.hpp"
#include "EspLedStrip.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/audio/IAudioBuffer.hpp"
#include "imu.h"
#include "motion_stream.h"
#include "imu_midi.h"
#include "include/touch_trace.h"
#include "crosspad-gui/components/touch_gesture.h"
#include "crosspad-gui/components/transport_bar.h"
#include "crosspad-gui/components/ui_probe.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad/imu/ImuMidiMapper.hpp"
#include "crosspad/event/EventData.hpp"
#include "crosspad/event/IEventBus.hpp"
#include "nvs.h"
#include "esp_system.h"
#include "NvsSettingsStore.hpp"
#include "crosspad/audio/IInputMonitor.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/pad/IPadMix.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"
#include "SongEnginePort.hpp"
#include "bsp_sdmmc.h"
#include "bsp_es8388.h"
#include "crosspad/audio/OutputEq.hpp"
#include "crosspad/audio/IInputGain.hpp"
#include "crosspad/audio/IInputPairs.hpp"
#include "crosspad/sampletools/ISampleTools.hpp"
#include "crosspad/sampletools/SliceMath.hpp"
#include "crosspad/audio/IAudioModule.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "CrosspadPlatform.hpp"
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
#include "crosspad-dawcontrol/DawControlState.hpp"
#endif
#include "crosspad-sampler/ui/sampler_panel.hpp"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "ml_smpl_stream_ply.h"
#include "MarcelSampleNode.hpp"
#endif
#include "SamplerEnginePort.hpp"
#include "crosspad/kit/KitInfo.hpp"
#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "PitchedKitPort.hpp"
#include "engine_arena.hpp"
#include "bsp_ulp.h"
#include "crosspad/instrument/IInstrumentHost.hpp"
#include "engine_handoff.h"
#include "instrument_host.h"
#include "PitchedSampleNode.hpp"
#include "crosspad/dsp/FftFactory.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "crosspad/instrument/SampleBank.hpp"
#include "indev/encoder_hil.h"
#include "EspBleMidi.hpp"
#include "BleCompanion.hpp"
#include "processMIDI.hpp"
#include "esp_heap_caps.h"
#include "performance_monitor.h"
#include "midi_clock.h"
#include "crosspad-gui/components/boot_splash.h"
#include "audio_rt.h"
#include "include/app_globals.h"
#include "sample_player_port.hpp"
#include "SamplerPlatformPort.hpp"

#include "hil_verbs.h"

/* ── BLE MIDI ──────────────────────────────────────────────────────────
 * The radio has no console of its own, so every question a BLE test asks is
 * answered here: stack up, peer subscribed, interval, message counts. */

/* BLE_START / BLE_STOP / BLE_ENABLE: bringing NimBLE up or down waits on its
 * host task and can take seconds (the HIL allows 15 s). The Settings toggle
 * does it on the LVGL thread; so do these, through the same calls. The verb
 * answers "OK queued"; BLE_STATUS req= says pending / ok / err / deferred. */
enum : int { kBleReqNone = 0, kBleReqPending, kBleReqOk, kBleReqErr, kBleReqDeferred };
static std::atomic<int> s_ble_req{kBleReqNone};
struct BleJob { int8_t op; uint8_t mode; bool persist; };   /* op: 1 start, 0 stop */
static BleJob s_ble_job;

static const char *ble_req_name(int r)
{
    switch (r) {
    case kBleReqPending:  return "pending";
    case kBleReqOk:       return "ok";
    case kBleReqErr:      return "err";
    case kBleReqDeferred: return "deferred";
    default:              return "-";
    }
}

static void async_ble_job(void *)
{
    const BleJob job = s_ble_job;
    if (job.persist) {
        if (auto *cfg = crosspad::CrosspadSettings::getInstance()) {
            cfg->wireless.enableBLE = job.op == 1;
            cfg->saveTo(NvsSettingsStore::getInstance());
        }
    }
    if (job.op == 1) {
        const bool up = bleMidi_start(job.mode);
        s_ble_req.store(up ? kBleReqOk : bleMidi_deferred_for_wifi() ? kBleReqDeferred : kBleReqErr);
    } else {
        bleMidi_stop();
        s_ble_req.store(kBleReqOk);
    }
}

static void ble_job_post(int8_t op, uint8_t mode, bool persist)
{
    s_ble_job = BleJob{op, mode, persist};
    s_ble_req.store(kBleReqPending);
    lv_async_call_locked(async_ble_job, nullptr);
}

static void handle_ble_status(void)
{
    auto &ble = crosspad::getEspBleMidi();
    auto st = ble.getStats();
    auto dev = ble.getConnectedDevice();
    /* self= is what a host-side scan matches on: the service UUID and the
     * name are shared by every peripheral and every CrossPad on hardware. */
    auto self = ble.getOwnAddress();
    char reply[300];
    snprintf(reply, sizeof(reply),
             "BLE: supported=%d running=%d state=%s mode=%s self=%s peer=%s itvl=%u "
             "txoff=%d rxoff=%d "
             "tx_msg=%u tx_pkt=%u tx_drop=%u tx_err=%u rx_msg=%u rx_pkt=%u req=%s\r\n",
             crosspad::EspBleMidi::isSupported() ? 1 : 0,
             ble.isRunning() ? 1 : 0, ble.getStateName(),
             ble.getMode() == crosspad::BleMidiMode::Server ? "server" : "host",
             self.empty() ? "-" : self.c_str(),
             dev.address.empty() ? "-" : dev.address.c_str(),
             (unsigned)ble.getConnInterval(),
             (int)ble.getTxNoteOffset(), (int)ble.getNoteOffset(),
             (unsigned)st.txMessages, (unsigned)st.txPackets,
             (unsigned)st.txDropped, (unsigned)st.txErrors,
             (unsigned)st.rxMessages, (unsigned)st.rxPackets, ble_req_name(s_ble_req.load()));
    cdc_reply(reply);
}

static void handle_ble_devices(void)
{
    auto devices = crosspad::getEspBleMidi().getScanResults();
    char reply[320];
    int n = snprintf(reply, sizeof(reply), "BLEDEV: count=%u", (unsigned)devices.size());
    for (const auto &d : devices) {
        if (n >= (int)sizeof(reply) - 60) break;
        n += snprintf(reply + n, sizeof(reply) - n, " | %s %s %d",
                      d.address.c_str(), d.name.c_str(), (int)d.rssi);
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);
}

bool verb_ble_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_STATUS", 10) == 0) {
        handle_ble_status();
        return true;
    }
    return false;
}

bool verb_ble_devices(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_DEVICES", 11) == 0) {
        handle_ble_devices();
        return true;
    }
    return false;
}

bool verb_ble_stop(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_STOP", 8) == 0) {
        cdc_reply("OK queued\r\n");   /* reply first */
        ble_job_post(0, 0, false);      /* on the LVGL thread; BLE_STATUS req= */
        return true;
    }
    return false;
}
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
bool verb_daw_state(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "DAW_STATE", 9) == 0) {
        /* What DAW Control shows, for a bench that cannot read the LCD:
         * the three rows the host sent, transport bits, beat and mode. */
        dawcontrol::StateSnapshot s;
        dawcontrol::snapshot(s);
        char rbuf[192];
        snprintf(rbuf, sizeof(rbuf),
                 "DAW: running=%d connected=%d daw=%u mode=%u transport=%u beat=%u beats=%lu "
                 "row0=\"%s\" row1=\"%s\" row2=\"%s\"\r\n",
                 s.running ? 1 : 0, s.connected ? 1 : 0, s.dawId, s.mode, s.transport, s.beat,
                 (unsigned long)s.beatCount, s.rows[0], s.rows[1], s.rows[2]);
        cdc_reply(rbuf);
        return true;
    }
    return false;
}
#endif
bool verb_sysex_echo(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (strncmp(cmd, "SYSEX_ECHO", 10) == 0) {
        /* Bounce foreign-manufacturer SysEx from USB back to the host. A DAW
         * under Wine only hears MIDI whose ALSA source is the device it
         * opened, so a host tool sharing the FL Studio script reaches it by
         * going through the board. Off at boot, never saved. */
        if (sscanf(cmd, "SYSEX_ECHO %d", &a) == 1 && (a == 0 || a == 1)) {
            usbMidi_setSysExEcho(a == 1);
        }
        cdc_reply(usbMidi_getSysExEcho() ? "SYSEX_ECHO: 1\r\n" : "SYSEX_ECHO: 0\r\n");
        return true;
    }
    return false;
}

bool verb_ble_enable(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "BLE_ENABLE %d", &a) == 1 && (a == 0 || a == 1)) {
        /* Persisted, unlike BLE_START/BLE_STOP: the same switch Settings ->
         * BLE MIDI flips, so a bench can put the board in the rc1 shape (BLE
         * off, ~60 kB more internal RAM) without driving the UI, and the
         * choice survives the reboots a release gate causes. */
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg) { cdc_reply("ERR no settings\r\n"); return true; }
        /* Saved and applied on the LVGL thread, as the Settings toggle does:
         * a stack start or stop waits on NimBLE's host task. BLE_STATUS req=. */
        cdc_reply("OK queued\r\n");   /* reply first */
        ble_job_post(a == 1 ? 1 : 0, cfg->wireless.bleMidiMode, true);
        return true;
    }
    return false;
}

bool verb_ble_start(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_START", 9) == 0) {
        /* BLE_START [0|1] — 0 = Host (central), 1 = Server (peripheral).
         * Defaults to the mode in settings. */
        int mode = crosspad::CrosspadSettings::getInstance()->wireless.bleMidiMode;
        sscanf(cmd, "BLE_START %d", &mode);
        /* On the LVGL thread (ble_job_post): the outcome -- up, refused for
         * lack of RAM, or put off while WiFi holds the radio -- is BLE_STATUS
         * req=ok|err|deferred once it is no longer pending. */
        cdc_reply("OK queued\r\n");   /* reply first */
        ble_job_post(1, (uint8_t)mode, false);
        return true;
    }
    return false;
}

bool verb_ble_scan(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "BLE_SCAN %d", &a) == 1) {
        crosspad::getEspBleMidi().startScan((uint16_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_ble_scan_2(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_SCAN", 8) == 0) {
        crosspad::getEspBleMidi().startScan(5000);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_ble_connect(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_CONNECT", 11) == 0) {
        char addr[24] = {0};
        sscanf(cmd, "BLE_CONNECT %23s", addr);
        cdc_reply(crosspad::getEspBleMidi().connectToDevice(addr) ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    return false;
}

bool verb_ble_disconnect(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "BLE_DISCONNECT", 14) == 0) {
        crosspad::getEspBleMidi().disconnect();
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_ble_txoff(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "BLE_TXOFF %d", &a) == 1 && a >= -127 && a <= 127) {
        /* Transport-level, not persisted — proves the shift on the wire
         * without touching the user's settings. */
        crosspad::getEspBleMidi().setTxNoteOffset((int8_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_ble_send(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    int &b = l.b;
    if (sscanf(cmd, "BLE_SEND %d %d", &a, &b) >= 1 && a >= 0 && a < 128) {
        /* Straight at the BLE transport: a dead link vs an unrouted pad. */
        auto &ble = crosspad::getEspBleMidi();
        ble.sendNoteOn((uint8_t)a, (uint8_t)(b < 0 ? 100 : b), 0);
        ble.sendNoteOff((uint8_t)a, 0, 0);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_midi_trs(VerbLine &l)
{
    char *cmd = l.cmd;
    static crosspad::STMRoutingSettings s_route_saved;
    static bool s_route_saved_valid = false;
    /* MIDI_TRS [TYPE A|B | SKEW <0.1 %> | FLIP <seg> <1-in-N> | RESET |
     * ROUTE <hex r0> <hex r1> | ROUTE SAVED] -- the STM's TRS MIDI pair (STM
     * 1.14). Every form answers with the state and the per-segment counters.
     * Nothing is saved: ROUTE SAVED (or a reboot) restores the routing, the
     * next settings push the type. */
    if (strncmp(cmd, "MIDI_TRS", 8) == 0) {
        const char *a = cmd + 8;
        esp_err_t err = ESP_OK;
        unsigned v0 = 0, v1 = 0;
        int sv = 0;
        if (strncmp(a, " TYPE ", 6) == 0 && (a[6] == 'A' || a[6] == 'B')) {
            err = stm32_i2c_write_u8(REG_CTL_MIDI_OUT_TYPE, a[6] == 'B' ? 1 : 0);
        } else if (sscanf(a, " SKEW %d", &sv) == 1) {
            if (sv < -128 || sv > 127) { cdc_reply("ERR SKEW -128..127\r\n"); return true; }
            err = stm32_i2c_write_u8(REG_CTL_MIDI_TX_SKEW, (uint8_t)(int8_t)sv);
        } else if (sscanf(a, " FLIP %u %u", &v0, &v1) == 2) {
            if (v0 > MIDI_SEG_ESP_IN) { cdc_reply("ERR FLIP <seg 0..6> <1-in-N>\r\n"); return true; }
            const uint8_t cfg[5] = {(uint8_t)v0, (uint8_t)(v1 >> 24), (uint8_t)(v1 >> 16),
                                    (uint8_t)(v1 >> 8), (uint8_t)v1};
            err = stm32_i2c_write(REG_CTL_MIDI_FLIP_SEG, cfg, sizeof cfg);
        } else if (strncmp(a, " RESET", 6) == 0) {
            err = stm32_i2c_write_u8(REG_CMD_MIDI_STATS_RESET, 1);
        } else if (strncmp(a, " ROUTE SAVED", 12) == 0) {
            auto &rt = crosspad::CrosspadSettings::getInstance()->stmRouting;
            if (s_route_saved_valid) rt = s_route_saved;
            s_route_saved_valid = false;
            const uint8_t r[2] = {rt.packByte1(), rt.packByte2()};
            err = stm32_i2c_write(REG_CTL_ROUTING_0, r, sizeof r);
        } else if (sscanf(a, " ROUTE %x %x", &v0, &v1) == 2) {
            /* Into the settings in RAM, unsaved: Stm32Manager pushes them every
             * 3 s, and would put the stored routing back over a bare write. */
            auto &rt = crosspad::CrosspadSettings::getInstance()->stmRouting;
            if (!s_route_saved_valid) s_route_saved = rt;
            s_route_saved_valid = true;
            rt.unpack((uint8_t)v0, (uint8_t)v1);
            const uint8_t r[2] = {(uint8_t)v0, (uint8_t)v1};
            err = stm32_i2c_write(REG_CTL_ROUTING_0, r, sizeof r);
        } else if (*a) {
            cdc_reply("ERR MIDI_TRS [TYPE A|B|SKEW n|FLIP seg n|RESET|ROUTE r0 r1|ROUTE SAVED]\r\n");
            return true;
        }
        uint8_t cfg[7], rt[2], d[4 * REG_DEBUG_MIDI_WORDS];
        if (err != ESP_OK || stm32_i2c_read(REG_CTL_MIDI_OUT_TYPE, cfg, sizeof cfg) != ESP_OK ||
            stm32_i2c_read(REG_CTL_ROUTING_0, rt, sizeof rt) != ESP_OK ||
            stm32_i2c_read(REG_DEBUG_MIDI, d, sizeof d) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        uint32_t w[REG_DEBUG_MIDI_WORDS];
        for (int i = 0; i < REG_DEBUG_MIDI_WORDS; i++)
            w[i] = d[4*i] | (d[4*i+1] << 8) | (d[4*i+2] << 16) | ((uint32_t)d[4*i+3] << 24);
        const uint32_t flip_n = ((uint32_t)cfg[3] << 24) | (cfg[4] << 16) | (cfg[5] << 8) | cfg[6];
        char out[480];
        snprintf(out, sizeof(out),
                 "MIDI_TRS: type=%c skew=%d flip_seg=%u flip_n=%lu route=%02X,%02X usb_out=%lu "
                 "trs_queued=%lu trs_sent=%lu trs_drop=%lu in_rx=%lu in_fe=%lu in_ne=%lu in_ore=%lu "
                 "in_drop=%lu to_esp=%lu to_esp_drop=%lu to_usb=%lu to_usb_drop=%lu esp_in=%lu "
                 "flips=%lu line_min_a=%lu line_min_b=%lu in_rearm=%lu\r\n",
                 cfg[0] == 1 ? 'B' : (cfg[0] == 0 ? 'A' : '?'), (int)(int8_t)cfg[1], cfg[2],
                 (unsigned long)flip_n, rt[0], rt[1],
                 (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
                 (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7],
                 (unsigned long)w[8], (unsigned long)w[9], (unsigned long)w[10], (unsigned long)w[11],
                 (unsigned long)w[12], (unsigned long)w[13], (unsigned long)w[14],
                 (unsigned long)(w[15] & 0xFFFF), (unsigned long)(w[15] >> 16), (unsigned long)w[16]);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_midi_loop(VerbLine &l)
{
    char *cmd = l.cmd;
    /* MIDI_LOOP [CAPTURE 0|1 | SEND <count> <rate> | STOP] -- the ESP's end of
     * the MIDI loop test (midi_loop_test.h). */
    if (strncmp(cmd, "MIDI_LOOP", 9) == 0) {
        const char *a = cmd + 9;
        unsigned on = 0, count = 0, rate = 0;
        if (sscanf(a, " CAPTURE %u", &on) == 1) {
            midi_loop_capture(on != 0);
        } else if (sscanf(a, " SEND %u %u", &count, &rate) == 2) {
            if (!midi_loop_send(count, rate)) { cdc_reply("ERR busy_or_args\r\n"); return true; }
        } else if (strncmp(a, " STOP", 5) == 0) {
            midi_loop_stop();
        } else if (*a) {
            cdc_reply("ERR MIDI_LOOP [CAPTURE 0|1|SEND count rate|STOP]\r\n");
            return true;
        }
        char out[256];
        midi_loop_format(out, sizeof out);
        cdc_reply(out);
        return true;
    }
    return false;
}
