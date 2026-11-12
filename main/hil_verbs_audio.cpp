/* CDC verbs: codecs, routing, the mixer, USB audio and the RT task. Dispatch order is in hil_control.cpp. */
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

static const char *TAG = "hil_control";

/* MIX_PRESET, off the main task: see its verb. */
static volatile bool s_mix_preset_busy = false;
static char s_mix_preset_cmd[160];   // as long as a CDC line can be

static void mix_preset_task(void *)
{
    auto& pr = crosspad::getMixerPresets();
    const char *cmd = s_mix_preset_cmd;
    int n = 0;
    char name[24] = {0};
    bool ok = true;
    if (sscanf(cmd, "MIX_PRESET LOAD %d", &n) == 1)                ok = pr.load(n - 1);
    else if (sscanf(cmd, "MIX_PRESET SAVE %d %23[^\r\n]", &n, name) == 2) ok = pr.save(n - 1, name);
    else if (strcmp(cmd, "MIX_PRESET UNDO") == 0)                   ok = pr.undo();
    pr.refresh();
    char reply[320];
    int len = snprintf(reply, sizeof(reply), "MIXPRE: ok=%d undo=%d dir=%s", ok ? 1 : 0, pr.canUndo() ? 1 : 0,
                       pr.dir().c_str());
    for (int s = 0; s < crosspad::MixerPresets::kSlots && len < (int)sizeof(reply) - 32; ++s) {
        const auto& i = pr.info(s);
        const char k = i.kind == crosspad::MixerPresets::Kind::Saved ? 's'
                     : i.kind == crosspad::MixerPresets::Kind::Factory ? 'f' : 'e';
        len += snprintf(reply + len, sizeof(reply) - len, " %d=%c:%s", s + 1, k, i.name.c_str());
    }
    snprintf(reply + std::min(len, (int)sizeof(reply) - 3), 3, "\r\n");
    cdc_reply(reply);
    s_mix_preset_busy = false;
    /* Created WithCaps, so it must be deleted WithCaps or the stack leaks. */
    vTaskDeleteWithCaps(nullptr);
}

/* Output-bus peak the amp gate reads, plus its decision — an amp on because
 * audio plays looks exactly like one latched on by an idle bus. */
static void handle_audio_level(void)
{
    const crosspad::control::AudioLevel lv = crosspad::control::readAudioLevel(0);
    char reply[128];
    hil_text_audio_level(reply, sizeof(reply), lv, crosspad::getStm32Manager().ampGate().state(),
                         crosspad::getStm32Manager().ampGate().allowed());
    cdc_reply(reply);
}
#ifdef HIL_HAS_MIXER
/* Per-channel and per-output mixer meters, with each channel's route/volume/
 * mute — to place where a rendered voice (SMPL_PEAK non-zero) is lost before
 * the output bus the amp gate reads. */
static void handle_mix_lvl(void)
{
    crosspad::control::readMixerLevels(getMixerEngine(),
        [](const crosspad::control::MixerChannelLevel &row) {
            char reply[176];
            hil_text_mix_channel(reply, sizeof(reply), row);
            cdc_reply(reply);
        },
        [](const crosspad::control::MixerOutputLevel &row) {
            char reply[144];
            hil_text_mix_output(reply, sizeof(reply), row);
            cdc_reply(reply);
        },
        /* The limiter's deepest gain since the last MIX_LVL (1.000 = it never
         * acted): on each output above, and on the pads tap bounces and
         * takes record. */
        [](const crosspad::control::MixerTapLevel &row) {
            char reply[48];
            hil_text_mix_tap(reply, sizeof(reply), row);
            cdc_reply(reply);
        });
    cdc_reply("MIXLVL: end\r\n");
}
#endif


bool verb_in_pair(VerbLine &l)
{
    char *cmd = l.cmd;
    /* IN_PAIR [n] -- which inputs the two ADCs hear (the Mixer's IN pair):
     * the current pair, then every pair by number. */
    if (strncmp(cmd, "IN_PAIR", 7) == 0 && (cmd[7] == 0 || cmd[7] == ' ')) {
        auto* p = crosspad::getPlatformServices().inputPairs;
        if (!p) { cdc_reply("ERR IN_PAIR none\r\n"); return true; }
        int n = 0;
        bool ok = true;
        if (sscanf(cmd + 7, "%d", &n) == 1) ok = p->setPair(n);
        EXT_RAM_BSS_ATTR static char reply[128];
        int len = snprintf(reply, sizeof(reply), "INPAIR: ok=%d pair=%d", ok ? 1 : 0, p->pair());
        for (int i = 0; i < p->pairs() && len < (int)sizeof(reply) - 24; ++i) {
            len += snprintf(reply + len, sizeof(reply) - len, " %d=%s", i, p->pairName(i));
        }
        snprintf(reply + len, sizeof(reply) - len, "\r\n");
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_codec_reg(VerbLine &l)
{
    char *cmd = l.cmd;
    /* CODEC_REG c reg [val] -- raw ES8388 register (hex) on codec c, for
     * bench measurements; read back after a write. */
    if (strncmp(cmd, "CODEC_REG ", 10) == 0) {
        unsigned c = 0, reg = 0, val = 0;
        const int n = sscanf(cmd + 10, "%u %x %x", &c, &reg, &val);
        if (n < 2 || c > 1 || reg > 0x3F || val > 0xFF) {
            cdc_reply("ERR CODEC_REG <codec> <hexreg> [hexval]\r\n");
            return true;
        }
        const auto iface = (bsp_i2s_interface_t)c;
        if (n == 3 && bsp_es8388_write_register(iface, (uint8_t)reg, (uint8_t)val) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        uint8_t v = 0;
        if (bsp_es8388_read_register(iface, (uint8_t)reg, &v) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        char out[40];
        snprintf(out, sizeof(out), "CODECREG %u %02X: %02X\r\n", c, reg, v);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_ingain(VerbLine &l)
{
    char *cmd = l.cmd;
    /* INGAIN [i dB] -- the inputs' preamp gain (0 mics, 1 Line 1/2, 2 Line 3/4). */
    if (strncmp(cmd, "INGAIN", 6) == 0 && (cmd[6] == 0 || cmd[6] == ' ')) {
        auto* g = crosspad::getPlatformServices().inputGain;
        if (!g) { cdc_reply("ERR INGAIN none\r\n"); return true; }
        int i = 0, db = 0;
        if (sscanf(cmd + 6, "%d %d", &i, &db) == 2) g->setGainDb(i, (float)db);
        EXT_RAM_BSS_ATTR static char reply[96];
        int n = snprintf(reply, sizeof(reply), "INGAIN:");
        for (int k = 0; k < g->inputs(); ++k) {
            n += snprintf(reply + n, sizeof(reply) - n, " %s=%d", g->channelName(k), (int)lroundf(g->gainDb(k)));
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_uac_stats(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "UAC_STATS") == 0) {
        /* Host playback, end to end: what the render was short of (under),
         * how close it came (ring_min, frames; -1 = not streaming), and on
         * the USB side how long the speaker task was kept off the CPU and how
         * full TinyUSB's FIFO got -- at its size it overwrites. Maxima and
         * minima reset on read. */
        uint32_t under = 0, refused = 0, capdrop = 0, gap = 0, fifo = 0, fifoSize = 0;
        size_t ring = 0, capRing = 0;
        usb_audio_port_stats(&under, &refused, &capdrop, &ring, &capRing);
        const uint32_t ringMin = usb_audio_port_play_min();
        uac_device_spk_health(&gap, &fifo, &fifoSize);
        char out[300];
        snprintf(out, sizeof(out),
                 "UACSTATS: active=%d under=%lu refused=%lu ring=%u ring_min=%ld spk_gap_max_us=%lu "
                 "fifo_max=%lu/%lu capdrop=%lu cap_ring=%u asrc_ppm=%ld asrc_cyc_max=%lu asrc_read=%lu asrc_loop=%lu\r\n",
                 usb_audio_port_active() ? 1 : 0, (unsigned long)under, (unsigned long)refused,
                 (unsigned)ring, ringMin == UINT32_MAX ? -1L : (long)ringMin, (unsigned long)gap,
                 (unsigned long)fifo, (unsigned long)fifoSize, (unsigned long)capdrop, (unsigned)capRing,
                 (long)usb_audio_port_asrc_ppm(), (unsigned long)usb_audio_port_asrc_cyc_max(),
                 (unsigned long)usb_audio_port_asrc_avg(0), (unsigned long)usb_audio_port_asrc_avg(1));
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_usb_host(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "USB_HOST", 8) == 0 && (cmd[8] == '\0' || cmd[8] == ' ')) {
        /* The computer USB audio is tuned for (usb_host_profile.h): the
         * feedback endpoint's shape and the playback resampler's range.
         * AUTO takes what enumeration showed. Applied at once and persisted. */
        const char *arg = cmd[8] ? cmd + 9 : "";
        if (strcmp(arg, "AUTO") == 0) {
            usb_host_profile_set_mode(USB_HOST_MODE_AUTO);
        } else if (strcmp(arg, "MAC") == 0 || strcmp(arg, "MACOS") == 0) {
            usb_host_profile_set_mode(USB_HOST_MODE_MACOS);
        } else if (strcmp(arg, "LINUX") == 0) {
            usb_host_profile_set_mode(USB_HOST_MODE_LINUX);
        } else if (*arg) {
            cdc_reply("ERR usage USB_HOST [AUTO|MAC|LINUX]\r\n");
            return true;
        }
        char out[64];
        snprintf(out, sizeof(out), "USB_HOST: mode=%s host=%s detected=%s\r\n",
                 usb_host_mode_name(usb_host_profile_mode()), usb_host_profile_name(usb_host_profile_get()),
                 usb_host_seen_name(usb_host_profile_detected()));
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_usb_enum(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "USB_ENUM", 8) == 0 && (cmd[8] == '\0' || cmd[8] == ' ')) {
        /* The control requests since the last bus reset, eight a reply:
         * USB_ENUM <first>. */
        unsigned first = 0;
        if (cmd[8]) sscanf(cmd + 9, "%u", &first);
        char list[8 * 17 + 1];
        const uint32_t count = usb_host_enum_trace(first, 8, list, sizeof(list));
        char out[200];
        snprintf(out, sizeof(out), "USB_ENUM: n=%lu from=%u %s\r\n", (unsigned long)count, first, list);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_uac_late(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "UAC_LATE") == 0) {
        /* The tasks a SOF more than 1.3 ms late interrupted, with counts:
         * whoever held its core with interrupts off. */
        char out[200] = "UACLATE: ";
        const int n = (int)strlen(out);
        uac_device_sof_late(out + n, (int)sizeof(out) - n - 3);
        strlcat(out, "\r\n", sizeof(out));
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_uac_pkt(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "UAC_PKT") == 0) {
        /* The speaker packets as the board took them: received, short,
         * frames skipped between two, frames with two, and the latest an
         * interrupt ran into its frame (us, reset on read). */
        uint32_t pk = 0, sh = 0, skip = 0, dbl = 0, late = 0;
        uac_device_spk_packets(&pk, &sh);
        uac_device_spk_timing(&skip, &dbl, &late);
        uint32_t sofGap = 0, sofCore = 0;
        uac_device_sof_timing(&sofGap, &sofCore);
        char out[140];
        snprintf(out, sizeof(out), "UACPKT: pk=%lu sh=%lu skip=%lu dbl=%lu late_us=%lu sof_gap_us=%lu core=%lu t=%llu\r\n",
                 (unsigned long)pk, (unsigned long)sh, (unsigned long)skip, (unsigned long)dbl,
                 (unsigned long)late, (unsigned long)sofGap, (unsigned long)sofCore,
                 (unsigned long long)esp_timer_get_time());
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_uac_flow(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "UAC_FLOW", 8) == 0) {
        /* The playback stream's rates, short enough for one companion frame:
         * the feedback sent (samples a frame x 1e6), the frames the host
         * delivered and the render took since boot, and the time -- two reads
         * give the host's rate, the render's and the feedback's against each
         * other. */
        uint32_t host = 0, taken = 0, mic = 0, lost = 0;
        usb_audio_port_flow(&host, &taken);
        uac_device_mic_flow(&mic, &lost);
        char out[170];
        uint32_t zh = 0, zp = 0;
        usb_audio_port_zero_runs(&zh, &zp);
        snprintf(out, sizeof(out), "UACFLOW: fb=%lu host=%lu taken=%lu mic=%lu lost=%lu rs=%lu txu=%lu zh=%lu zp=%lu t=%llu\r\n",
                 (unsigned long)(((uint64_t)uac_device_feedback_q16() * 1000000ull) >> 16), (unsigned long)host,
                 (unsigned long)taken, (unsigned long)mic, (unsigned long)lost,
                 (unsigned long)audio_rt_pacer_resyncs(), (unsigned long)audio_rt_tx_underruns(),
                 (unsigned long)zh, (unsigned long)zp, (unsigned long long)esp_timer_get_time());
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_audio_rate(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "AUDIO_RATE", 10) == 0) {
        /* Engine sample rate: what the I2S clock runs at now, and what the
         * saved setting asks for after the next boot. The set form persists
         * the same value Settings -> Audio does; the clock only changes on
         * reboot. Lets a bench flip 44.1k/48k without driving the UI. */
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg) { cdc_reply("AUDIO_RATE: ERR no settings\r\n"); return true; }
        int hz = 0;
        if (sscanf(cmd, "AUDIO_RATE %d", &hz) == 1) {
            if (hz != 44100 && hz != 48000) { cdc_reply("AUDIO_RATE: ERR 44100|48000\r\n"); return true; }
            cfg->audioEngine.sampleRate = (uint32_t)hz;
            cfg->saveTo(NvsSettingsStore::getInstance());
        }
        char rbuf[64];
        snprintf(rbuf, sizeof(rbuf), "AUDIO_RATE: running=%lu configured=%lu\r\n",
                 (unsigned long)bsp_i2s_get_sample_rate(), (unsigned long)cfg->audioEngine.sampleRate);
        cdc_reply(rbuf);
        return true;
    }
    return false;
}

bool verb_outvol(VerbLine &l)
{
    char *cmd = l.cmd;
    /* INSRC (the engine branch's Source switch) is not in this build: main's
     * input pairs (InputPairPort, IN_PAIR) choose what each codec listens to. */

    /* OUTVOL [out pct] -- a codec's whole output volume, older than the
     * per-output levels (OutputLevelsPort). out 1 is codec 1, Line out (the
     * jack marked Out 3/4); out 2 is codec 0, every pair of it: the Speakers
     * and the Phones (the jack marked Out 1/2). A set is hardware only. Bare
     * OUTVOL reads the saved Line out and Speakers levels and the speakers'
     * switch (masterFX.speakersEnabled). The out1=/out2= keys stay as they
     * are: crosspad-hil reads them, and the text in brackets is not parsed. */
    if (strncmp(cmd, "OUTVOL", 6) == 0 && (cmd[6] == 0 || cmd[6] == ' ')) {
        const char* arg = cmd[6] ? cmd + 7 : "";
        while (*arg == ' ') ++arg;
        if (*arg) {
            unsigned out = 1, pct = 50;
            if (sscanf(arg, "%u %u", &out, &pct) != 2 || out < 1 || out > 2 || pct > 100) {
                cdc_reply("ERR OUTVOL out(1|2) pct(0..100)\r\n");
                return true;
            }
            /* out 1 is codec 1, out 2 is codec 0. */
            const esp_err_t err = bsp_es8388_set_volume(
                (out == 1) ? BSP_I2S_INTERFACE_1 : BSP_I2S_INTERFACE_0, (int)pct);
            cdc_reply(err == ESP_OK ? "OK\r\n" : "ERR OUTVOL codec not up\r\n");
            return true;
        }
        crosspad::CrosspadSettings* st = crosspad::CrosspadSettings::getInstance();
        char reply[176];
        snprintf(reply, sizeof(reply),
                 "OUTVOL: out1=%u out2=%u spk_amp=%s"
                 "  (out1=codec1=Line out 3/4, out2=Speakers; set 2 = all codec0: Speakers+Phones 1/2)\r\n",
                 st ? (unsigned)st->masterFX.outVolumeCodec[1][0] : 0u,
                 st ? (unsigned)st->masterFX.outVolumeCodec[0][0] : 0u,
                 (st && st->masterFX.speakersEnabled) ? "on" : "off");
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_mix_eq(VerbLine &l)
{
    char *cmd = l.cmd;
    /* MIX_EQ [out ON|OFF|FLAT | out band OFF | out band PEAK|LOSHELF|HISHELF|HP|LP hz [db] [q]]
     * -- the four bands across an output, for evening out what it plays
     * through. Bare MIX_EQ reports a line per output; db is in tenths and q
     * in hundredths, because this task has no room for printf's float path
     * (see MIX_LVL). */
    if (strncmp(cmd, "MIX_EQ", 6) == 0 && (cmd[6] == 0 || cmd[6] == ' ')) {
        auto& mx = getMixerEngine();
        const char* arg = cmd[6] ? cmd + 7 : "";
        while (*arg == ' ') ++arg;
        if (*arg) {
            unsigned out = 0;
            char rest[80] = {0};
            if (sscanf(arg, "%u %79[^\r\n]", &out, rest) < 1 || out >= mx.maxOutputs()) {
                cdc_reply("ERR MIX_EQ out band ...\r\n");
                return true;
            }
            crosspad::EqSettings eq = mx.getOutputEq((uint8_t)out);
            if (strncmp(rest, "ON", 2) == 0)       eq.on = true;
            else if (strncmp(rest, "OFF", 3) == 0) eq.on = false;
            else if (strncmp(rest, "FLAT", 4) == 0) eq = crosspad::EqSettings{};
            else {
                unsigned band = 0;
                char kind[12] = {0};
                int hz = 1000, dbT = 0, qH = 70;
                const int got = sscanf(rest, "%u %11s %d %d %d", &band, kind, &hz, &dbT, &qH);
                if (got < 2 || band >= crosspad::kEqBands) {
                    cdc_reply("ERR MIX_EQ out band PEAK|LOSHELF|HISHELF|HP|LP hz [db*10] [q*100]\r\n");
                    return true;
                }
                crosspad::EqBand& b = eq.band[band];
                if      (!strcmp(kind, "OFF"))     b.kind = crosspad::EqKind::Off;
                else if (!strcmp(kind, "PEAK"))    b.kind = crosspad::EqKind::Peak;
                else if (!strcmp(kind, "LOSHELF")) b.kind = crosspad::EqKind::LowShelf;
                else if (!strcmp(kind, "HISHELF")) b.kind = crosspad::EqKind::HighShelf;
                else if (!strcmp(kind, "HP"))      b.kind = crosspad::EqKind::HighPass;
                else if (!strcmp(kind, "LP"))      b.kind = crosspad::EqKind::LowPass;
                else { cdc_reply("ERR MIX_EQ kind\r\n"); return true; }
                if (got >= 3) b.hz = (float)hz;
                if (got >= 4) b.gainDb = (float)dbT / 10.0f;
                if (got >= 5) b.q = (float)qH / 100.0f;
                /* A band worth having is a band that is on. */
                if (b.kind != crosspad::EqKind::Off) eq.on = true;
            }
            mx.setOutputEq((uint8_t)out, eq);
            /* A host's command sticks: the desk is saved, off this task (the
             * platform defers it to the LVGL thread). */
            mx.notifyStateChanged();
            cdc_reply("OK\r\n");
            return true;
        }
        static const char* kKinds[] = {"off", "loshelf", "peak", "hishelf", "hp", "lp"};
        for (uint8_t o = 0; o < mx.numOutputs(); ++o) {
            const crosspad::EqSettings eq = mx.getOutputEq(o);
            /* On the stack, not static: this task has a few hundred bytes of
             * internal RAM to spare at boot and every static here is BSS.
             * Safe because nothing below formats a float (see MIX_LVL). */
            char reply[220];
            int n = snprintf(reply, sizeof(reply) - 3, "MIXEQ: out=%u on=%d live=%d", (unsigned)o,
                             eq.on ? 1 : 0, mx.isOutputEqActive(o) ? 1 : 0);
            for (int i = 0; i < crosspad::kEqBands; ++i) {
                const crosspad::EqBand& b = eq.band[i];
                const int k = (int)b.kind;
                n += snprintf(reply + n, sizeof(reply) - 3 - n, " b%d=%s/%d/%d/%d", i,
                              k >= 0 && k <= 5 ? kKinds[k] : "?", (int)b.hz,
                              (int)(b.gainDb * 10.0f), (int)(b.q * 100.0f));
                n = std::min(n, (int)sizeof(reply) - 3);
            }
            snprintf(reply + n, sizeof(reply) - n, "\r\n");
            cdc_reply(reply);
        }
        cdc_reply("MIXEQ: end\r\n");
        return true;
    }
    return false;
}

bool verb_clip(VerbLine &l)
{
    char *cmd = l.cmd;
    /* CLIP [LAUNCH track file [bars]|STOP [track]|Q bars] -- the clip
     * launcher's live transport. Bare CLIP reports a line per track: the
     * song's fader and mute (what the engine plays it at), the loop and how
     * far into it, and the clip waiting for the next line. Names last. */
    if (strncmp(cmd, "CLIP", 4) == 0 && (cmd[4] == 0 || cmd[4] == ' ')) {
        crosspad::ISongEngine* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ERR CLIP no song engine\r\n"); return true; }
        const char* arg = cmd[4] ? cmd + 5 : "";
        while (*arg == ' ') ++arg;
        if (strncmp(arg, "LAUNCH", 6) == 0) {
            unsigned tr = 0, bars = 0;
            char file[96] = {0};
            /* The file may hold spaces: the track and the bars are read off
             * the front and the rest of the line is the name. */
            if (sscanf(arg + 6, " %u %95[^\r\n]", &tr, file) < 2) {
                cdc_reply("ERR CLIP LAUNCH track file [bars]\r\n");
                return true;
            }
            char* sp = strrchr(file, ' ');
            if (sp && sscanf(sp + 1, "%u", &bars) == 1 && bars) *sp = 0;
            cdc_reply(se->liveLaunch((uint8_t)tr, std::string(file), (uint16_t)bars)
                          ? "OK\r\n" : "ERR CLIP launch refused\r\n");
            return true;
        }
        if (strncmp(arg, "STOP", 4) == 0) {
            unsigned tr = crosspad::AudioSong::kTracks;
            sscanf(arg + 4, " %u", &tr);
            cdc_reply(se->liveStop((uint8_t)tr) ? "OK\r\n" : "ERR CLIP not running\r\n");
            return true;
        }
        if (strncmp(arg, "Q", 1) == 0 && (arg[1] == 0 || arg[1] == ' ')) {
            unsigned q = 1;
            sscanf(arg + 1, " %u", &q);
            se->liveQuant((uint8_t)q);
            cdc_reply("OK\r\n");
            return true;
        }
        if (*arg) { cdc_reply("ERR CLIP LAUNCH track file [bars]|STOP [track]|Q bars\r\n"); return true; }
        /* On the heap: a track's state is ~64 B, kTracks of them too much for this task's stack. */
        auto live = std::make_unique<crosspad::ISongEngine::LiveState>();
        crosspad::ISongEngine::LiveState& st = *live;
        if (!se->liveState(st)) { cdc_reply("ERR CLIP no live transport\r\n"); return true; }
        EXT_RAM_BSS_ATTR static char reply[220];
        const int barCenti = (int)(st.bars * 100.0);
        snprintf(reply, sizeof(reply), "CLIP: on=%d bars=%d.%02d q=%u fpb=%u\r\n",
                 st.on ? 1 : 0, barCenti / 100, barCenti % 100,
                 (unsigned)st.quantBars, (unsigned)st.framesPerBar);
        cdc_reply(reply);
        for (int t = 0; t < crosspad::AudioSong::kTracks; ++t) {
            float vol = 0.0f;
            bool muted = false;
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                vol = se->song().tracks[t].volume;
                muted = se->song().tracks[t].muted;
            }
            const crosspad::ISongEngine::LiveTrack& lt = st.tracks[t];
            const int volCenti = (int)(vol * 100.0f);
            snprintf(reply, sizeof(reply),
                     "CLIP: t=%d vol=%d.%02d mute=%d bars=%u loop=%u phase=%u stop=%d pend=%s file=%s\r\n",
                     t, volCenti / 100, volCenti % 100, muted ? 1 : 0, (unsigned)lt.bars,
                     (unsigned)lt.loopFrames, (unsigned)lt.phase, lt.stopping ? 1 : 0,
                     lt.pending.empty() ? "-" : lt.pending.c_str(),
                     lt.file.empty() ? "-" : lt.file.c_str());
            cdc_reply(reply);
        }
        cdc_reply("CLIP: end\r\n");
        return true;
    }
    return false;
}

bool verb_audio_level(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "AUDIO_LEVEL", 11) == 0) {
        handle_audio_level();
        return true;
    }
    return false;
}
#ifdef HIL_HAS_MIXER
bool verb_mix_lvl(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "MIX_LVL", 7) == 0) {
        handle_mix_lvl();
        return true;
    }
    return false;
}

bool verb_mix_preset(VerbLine &l)
{
    char *cmd = l.cmd;
    /* MIX_PRESET [LOAD n|SAVE n name|UNDO] -- the Mixer's presets, slots 1..6:
     * each slot's kind (s saved, f starting point, e empty) and name. On a
     * task of its own, as FW_LIST: every form reads the card, and on the 4 kB
     * main task the FATFS open under it overflowed the stack and rebooted the
     * board (09.10.2026). */
    if (strncmp(cmd, "MIX_PRESET", 10) == 0) {
        if (s_mix_preset_busy) { cdc_reply("MIXPRE: ERR busy\r\n"); return true; }
        s_mix_preset_busy = true;
        snprintf(s_mix_preset_cmd, sizeof(s_mix_preset_cmd), "%s", cmd);
        if (xTaskCreatePinnedToCoreWithCaps(mix_preset_task, "mix_preset", 8192,
                                            nullptr, 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            s_mix_preset_busy = false;
            cdc_reply("MIXPRE: ERR no task\r\n");
        }
        return true;
    }
    return false;
}

bool verb_input_mon(VerbLine &l)
{
    char *cmd = l.cmd;
    /* INPUT_MON MIC|LINE|MICLINE|USB [0|1] -- an input heard live or not, as
     * Arrange's audio track sets it (IInputMonitor: the Mixer's MONITOR at
     * all or off for that input's channels). Bare: whether it is. CDC only,
     * as MIX_ROUTE: it changes the desk. */
    if (strncmp(cmd, "INPUT_MON ", 10) == 0) {
        char name[12] = {};
        int on = -1;
        const int got = sscanf(cmd + 10, "%11s %d", name, &on);
        crosspad::RecInput in = crosspad::RecInput::Mic;
        bool known = true;
        if (strcmp(name, "MIC") == 0)          in = crosspad::RecInput::Mic;
        else if (strcmp(name, "LINE") == 0)    in = crosspad::RecInput::Line;
        else if (strcmp(name, "MICLINE") == 0) in = crosspad::RecInput::MicLine;
        else if (strcmp(name, "USB") == 0)     in = crosspad::RecInput::Usb;
        else                                   known = false;
        crosspad::IInputMonitor* mon = crosspad::getPlatformServices().inputMonitor;
        if (!known || got < 1 || (got == 2 && on != 0 && on != 1)) {
            cdc_reply("ERR usage INPUT_MON MIC|LINE|MICLINE|USB [0|1]\r\n");
        } else if (!mon || !mon->canMonitor(in)) {
            cdc_reply("ERR no_monitor\r\n");
        } else {
            if (got == 2 && !mon->setMonitoring(in, on == 1)) { cdc_reply("ERR set\r\n"); return true; }
            char out[40];
            snprintf(out, sizeof(out), "INPUT_MON: %s %d\r\n", name, mon->monitoring(in) ? 1 : 0);
            cdc_reply(out);
        }
        return true;
    }
    return false;
}

bool verb_mix_route(VerbLine &l)
{
    char *cmd = l.cmd;
    /* MIX_ROUTE ch out 0|1 -- a send on or off, as the Mixer's MONITOR and
     * ROUTE set them (an IN channel sent anywhere holds its ADC on the input):
     * a send switched on whose level was dragged to nothing comes back at unity. */
    {
        int ch = -1, out = -1, on = -1;
        if (sscanf(cmd, "MIX_ROUTE %d %d %d", &ch, &out, &on) == 3) {
            auto& mx = getMixerEngine();
            const bool ok = ch >= 0 && ch < mx.maxChannels() && mx.isChannelActive((uint8_t)ch) &&
                            out >= 0 && out < mx.numOutputs() && (on == 0 || on == 1);
            if (ok) {
                if (on == 1 && mx.getRouteVolume((uint8_t)ch, (uint8_t)out) <= 0.0f) {
                    mx.setRouteVolume((uint8_t)ch, (uint8_t)out, 1.0f);
                }
                mx.setRouteEnabled((uint8_t)ch, (uint8_t)out, on == 1);
            }
            cdc_reply(ok ? "OK\r\n" : "ERR MIX_ROUTE ch out 0|1\r\n");
            return true;
        }
    }
    return false;
}

bool verb_mix_mute(VerbLine &l)
{
    char *cmd = l.cmd;
    /* MIX_MUTE ch 0|1, MIX_VOL ch pct, MIX_OUT_MUTE out 0|1, MIX_OUT_VOL out pct
     * -- a channel's (MIX_LVL numbering) or an output's mute and level, set
     * with the setters the Mixer's M button and fader use. pct is linear gain
     * in percent: 100 is unity, MIX_LVL's vol=1.00. The Mixer screen, when it
     * is open, shows the change on its next tick (it is written from the
     * engine). The desk is saved as the Mixer's close saves it
     * (notifyStateChanged), so a reboot keeps it -- after the reply: the save
     * is posted to the LVGL thread, never run on this task. CDC only, as
     * MIX_ROUTE: nothing on the companion allow-list changes the desk. */
    {
        int id = -1, v = -1;
        const bool isMute = sscanf(cmd, "MIX_MUTE %d %d", &id, &v) == 2;
        const bool isVol  = !isMute && sscanf(cmd, "MIX_VOL %d %d", &id, &v) == 2;
        const bool isOMut = !isMute && !isVol && sscanf(cmd, "MIX_OUT_MUTE %d %d", &id, &v) == 2;
        const bool isOVol = !isMute && !isVol && !isOMut && sscanf(cmd, "MIX_OUT_VOL %d %d", &id, &v) == 2;
        const bool named  = strncmp(cmd, "MIX_MUTE", 8) == 0 || strncmp(cmd, "MIX_VOL", 7) == 0 ||
                            strncmp(cmd, "MIX_OUT_MUTE", 12) == 0 || strncmp(cmd, "MIX_OUT_VOL", 11) == 0;
        if (isMute || isVol || isOMut || isOVol || named) {
            auto& mx = getMixerEngine();
            const bool chOk  = id >= 0 && id < mx.maxChannels() && mx.isChannelActive((uint8_t)id);
            const bool outOk = id >= 0 && id < mx.numOutputs();
            const bool flagOk = v == 0 || v == 1;
            const bool pctOk  = v >= 0 && v <= 100;
            const char* err = nullptr;
            if      (isMute) { if (!chOk || !flagOk)  err = "ERR MIX_MUTE ch 0|1\r\n"; }
            else if (isVol)  { if (!chOk || !pctOk)   err = "ERR MIX_VOL ch pct(0..100)\r\n"; }
            else if (isOMut) { if (!outOk || !flagOk) err = "ERR MIX_OUT_MUTE out 0|1\r\n"; }
            else if (isOVol) { if (!outOk || !pctOk)  err = "ERR MIX_OUT_VOL out pct(0..100)\r\n"; }
            else err = "ERR MIX_MUTE ch 0|1 | MIX_VOL ch pct | MIX_OUT_MUTE out 0|1 | MIX_OUT_VOL out pct\r\n";
            if (err) { cdc_reply(err); return true; }
            const uint8_t i = (uint8_t)id;
            if      (isMute) mx.setChannelMute(i, v == 1);
            else if (isVol)  mx.setChannelVolume(i, (float)v / 100.0f);
            else if (isOMut) mx.setOutputMute(i, v == 1);
            else             mx.setOutputVolume(i, (float)v / 100.0f);
            cdc_reply("OK\r\n");
            lv_async_call_locked([](void*) { getMixerEngine().notifyStateChanged(); }, nullptr);
            return true;
        }
    }
    return false;
}
#endif


bool verb_simd_mode(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "SIMD_MODE") == 0) {
        /* Which kernels the render runs: the S3's (PIE 128-bit float loads)
         * once the platform registered them with core, the portable ones
         * otherwise. */
#ifdef BSP_BOARD_CROSSPAD_S3
        const int pie = crosspad::espDspKernelsRegistered() ? 1 : 0;
#else
        const int pie = 0;
#endif
        char reply[64];
        snprintf(reply, sizeof(reply), "SIMDMODE: pie=%d\r\n", pie);
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_audio_buf(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "AUDIO_BUF", 9) == 0) {
        /* Settings -> Audio -> Buffer (IAudioBuffer): the render's block and
         * how far ahead of the DAC it runs. `AUDIO_BUF` lists the choices,
         * `AUDIO_BUF n` picks one -- live and kept, as the Settings row does
         * -- and `AUDIO_BUF Q frames` moves only the queue, unsaved, for a
         * measurement between the choices. The last line also carries the
         * pacer's counters and each core's idle share since the last call. */
        crosspad::IAudioBuffer *ab = crosspad::getPlatformServices().audioBuffer;
        if (!ab) { cdc_reply("ERR no_audio_buffer\r\n"); return true; }
        const char *arg = cmd + 9;
        while (*arg == ' ') arg++;
        if (*arg == 'Q') {
            audio_rt_set_queue_frames((uint32_t)strtoul(arg + 1, nullptr, 10));
        } else if (*arg >= '0' && *arg <= '9') {
            if (!ab->select((uint8_t)atoi(arg))) { cdc_reply("ERR refused\r\n"); return true; }
        }
        static uint32_t s_idle[2], s_us;
        const uint32_t nowUs = (uint32_t)esp_timer_get_time();
        uint32_t idlePct[2] = {0, 0};
        for (int c = 0; c < 2; ++c) {
            const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(c);
            const uint32_t span = (nowUs - s_us) / 100;
            idlePct[c] = (s_us && span) ? (idle - s_idle[c]) / span : 0;
            s_idle[c] = idle;
        }
        s_us = nowUs;
        const uint32_t rate = ab->sampleRate();
        char line[200];
        for (uint8_t i = 0; i < ab->choices(); ++i) {
            const crosspad::IAudioBuffer::Choice c = ab->choice(i);
            /* Short rows: the whole reply has to fit a companion SysEx reply
             * (481 bytes). The end line carries the rate's queue_us. */
            snprintf(line, sizeof(line), "AUDIOBUF: i=%u block=%u queue=%u\r\n", (unsigned)i,
                     (unsigned)c.blockFrames, (unsigned)c.queueFrames);
            cdc_reply(line);
        }
        const uint32_t q = audio_rt_queue_frames();
        snprintf(line, sizeof(line),
                 "AUDIOBUF: end current=%u block=%u queue=%u queue_us=%u resyncs=%u tx_under=%u "
                 "idle0_pct=%u idle1_pct=%u\r\n",
                 (unsigned)ab->current(), (unsigned)audio_rt_block_frames(), (unsigned)q,
                 rate ? (unsigned)((uint64_t)q * 1000000u / rate) : 0u,
                 (unsigned)audio_rt_pacer_resyncs(), (unsigned)audio_rt_tx_underruns(),
                 (unsigned)idlePct[0], (unsigned)idlePct[1]);
        cdc_reply(line);
        return true;
    }
    return false;
}

bool verb_lat_trace(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "LAT_TRACE", 9) == 0) {
        /* One STM note's trip through the ESP, stage by stage (lat_trace.h).
         * `LAT_TRACE RESET` starts (or restarts) it, `LAT_TRACE` reports per
         * segment, `LAT_TRACE RAW [first [count]]` lists hits with their
         * stamps (to line up with a host's send times; 4 kB a reply, so a
         * host pages through them), `LAT_TRACE NOW` reads the clock the
         * stamps are on. LATTX is the TX DMA's own count: buffers played and
         * buffers played unrefilled. */
        const char *arg = cmd + 9;
        while (*arg == ' ') arg++;
        if (strncmp(arg, "RESET", 5) == 0) {
            cdc_reply(lat_trace_reset() ? "OK\r\n" : "ERR no_mem\r\n");
            return true;
        }
        if (strncmp(arg, "NOW", 3) == 0) {
            /* The trace's clock, for a host lining its own up with it: the
             * reply nearest its request, over many, carries the offset. */
            char now[40];
            snprintf(now, sizeof(now), "LATNOW: us=%u\r\n", (unsigned)(uint32_t)esp_timer_get_time());
            cdc_reply(now);
            return true;
        }
        constexpr size_t kOut = 4096;
        char *out = static_cast<char *>(heap_caps_malloc(kOut, MALLOC_CAP_SPIRAM));
        if (!out) {
            cdc_reply("ERR no_mem\r\n");
            return true;
        }
        if (strncmp(arg, "RAW", 3) == 0) {
            char *next = nullptr;
            const uint32_t first = (uint32_t)strtoul(arg + 3, &next, 10);
            lat_trace_raw(out, kOut, first, (uint32_t)strtoul(next, nullptr, 10));
            cdc_reply(out);
            heap_caps_free(out);
            return true;
        }
        size_t len = lat_trace_report(out, kOut);
        bsp_i2s_tx_clock_t c0, c1;
        bsp_i2s_tx_clock(BSP_I2S_INTERFACE_0, &c0);
        bsp_i2s_tx_clock(BSP_I2S_INTERFACE_1, &c1);
        snprintf(out + len, kOut - len,
                 "LATTX: desc=%u dma_frames=%u block=%u rate=%u sent0=%u unfilled0=%u sent1=%u unfilled1=%u\r\n",
                 (unsigned)BSP_I2S_DMA_DESC_NUM, (unsigned)BSP_I2S_DMA_FRAME_NUM,
                 (unsigned)audio_rt_block_frames(), (unsigned)bsp_i2s_get_sample_rate(),
                 (unsigned)c0.sent, (unsigned)c0.unfilled, (unsigned)c1.sent, (unsigned)c1.unfilled);
        cdc_reply(out);
        heap_caps_free(out);
        return true;
    }
    return false;
}

bool verb_rt_prof(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "RT_PROF", 7) == 0) {
        /* Where the render pass goes, span by span. `RT_PROF 1` starts it,
         * `RT_PROF` reads and clears, `RT_PROF 0` stops. It exists because
         * TASKS can say audio_rt costs 42 % of a core and not which part of
         * the pass that is -- and without that the only honest answer to
         * "everything is slower in the audio profile" is a guess. */
        const char *arg = cmd + 7;
        while (*arg == ' ') arg++;
        if (*arg == '0' || *arg == '1') {
            const bool on = (*arg == '1');
            audio_rt_prof_enable(on);
            audio_mixer_prof_enable(on, &getMixerEngine());
            cdc_reply("OK\r\n");
            return true;
        }
        char reply[224];
        audio_rt_prof_read(reply, sizeof(reply));
        cdc_reply(reply);
        audio_mixer_prof_read(&getMixerEngine(), reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_audio_tasks(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "AUDIO_TASKS %d", &a) == 1 && (a == 0 || a == 1)) {
        ESP_LOGW(TAG, "AUDIO_TASKS override: %s", a ? "resume" : "suspend");
        AudioTasks_SetSuspended(a == 0);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}
