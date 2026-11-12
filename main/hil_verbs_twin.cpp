/* CDC verbs: the browser twin and the motion stream. Dispatch order is in hil_control.cpp. */
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

/* ── Command dispatch ──────────────────────────────────────────────── */

/* The TWIN_* verbs that read the GUI (TWIN_INFO, TWIN_FW, TWIN_SETTINGS,
 * TWIN_STATE). They keep their replies and snapshots in statics and reach this
 * from two tasks (the CDC loop, the companion's MIDI), so the caller holds
 * twin_verb_lock() around them. False: not one of them. */
enum { JOB_RUNNING, JOB_DONE, JOB_ABANDONED };
/* Waits for an async job: true when it finished (the caller frees it). On a
 * timeout the job is left to free itself when it ends, unless it ended just
 * now, which counts as finished. */
static bool job_wait(std::atomic<int> &state, int ms)
{
    for (int waited = 0; state.load() == JOB_RUNNING && waited < ms; waited += 5) vTaskDelay(pdMS_TO_TICKS(5));
    return state.load() == JOB_DONE || state.exchange(JOB_ABANDONED) == JOB_DONE;
}

static SemaphoreHandle_t twin_verb_lock(void)
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

static bool twin_verb(const char *cmd)
{
    /* replies and snapshots in PSRAM: the TWIN_* verbs are a twin's, not the board's own,
     * and the internal RAM is what the image is short of */
    if (strncmp(cmd, "TWIN_INFO ", 10) == 0) {
        EXT_RAM_BSS_ATTR static char reply[200];
        auto *ui = crosspad::getSettingsUI();
        const char *arg = cmd + 10;
        if (!ui) { cdc_reply("ERR TWIN_INFO no_ui\r\n"); return true; }
        if (*arg == 'P') {
            const char *info = ui->getPlatformInfo();
            snprintf(reply, sizeof reply, "TWIN_INFO P %s\t%s\r\n", ui->getPlatformName(), info ? info : "");
        } else if (*arg == 'C') {
            crosspad::CoprocFwInfo c{};
            const bool ok = ui->getCoprocFwInfo(c);
            snprintf(reply, sizeof reply, "TWIN_INFO C %d\t%s\t%s\t%d\t%d\r\n", ok ? 1 : 0, c.running, c.bundled,
                     c.due ? 1 : 0, c.canUpdate ? 1 : 0);
        } else {
            const int i = atoi(arg), n = ui->getInfoRowCount();
            const char *label = "", *value = "";
            ui->getInfoRow(i, &label, &value);
            snprintf(reply, sizeof reply, "TWIN_INFO %d %d %s\t%s\r\n", n, i, label ? label : "", value ? value : "");
        }
        cdc_reply(reply);
        return true;
    }
    /* TWIN_FW M|S <slot>|L <index> -- what the Firmware app shows, for the
     * twin's copy of it: M the misc state (rollback, USB mode, auto-accept,
     * a flow running, the running version), S a slot, L a package of the SD
     * card library (scanned at index 0). */
    if (strncmp(cmd, "TWIN_FW ", 8) == 0) {
        EXT_RAM_BSS_ATTR static char reply[240];
        const char *arg = cmd + 8;
        if (*arg == 'M') {
            const esp_app_desc_t *app = esp_app_get_description();
            snprintf(reply, sizeof reply, "TWIN_FW M %d %d %d %d %s\r\n", fw_rollback_supported() ? 1 : 0,
                     usb_config_manager_get_mode() == USB_MODE_DEFAULT ? 1 : 0,
                     NvsSettingsStore::getInstance().readU8("ota_prefs", "auto_accept", 0) ? 1 : 0,
                     fw_flow_active() ? 1 : 0, app ? app->version : "-");
        } else if (*arg == 'S') {
            FwSlotInfo slots[2];
            fw_slots_get(slots);
            const int i = atoi(arg + 1) ? 1 : 0;
            const FwSlotInfo &s = slots[i];
            snprintf(reply, sizeof reply, "TWIN_FW S %d %d %d %d %c\t%s\t%s\t%s\r\n", i, s.running ? 1 : 0,
                     (int)s.state, s.hasDesc ? 1 : 0, s.label, s.package, s.version, s.kit);
        } else if (*arg == 'L') {
            EXT_RAM_BSS_ATTR static FwPackage s_pkgs[16];
            static size_t s_n = 0;
            const int i = atoi(arg + 1);
            /* The scan wants the Firmware app's 8 kB of stack (update_app.cpp),
             * not this 4 kB main task: a task of its own, waited for. It scans
             * into a job of its own, so one that outlives the wait writes
             * nothing the next TWIN_FW reads (and frees the job itself). */
            if (i == 0) {
                /* One scan at a time: a request after a timeout finds the
                 * first still running and is told so, not given a second task. */
                static std::atomic<bool> s_scanRunning{false};
                if (s_scanRunning.exchange(true)) { cdc_reply("ERR TWIN_FW busy\r\n"); return true; }
                struct ScanJob { FwPackage pkgs[16]; size_t n = 0; std::atomic<int> state{JOB_RUNNING}; };
                auto *job = new (std::nothrow) ScanJob;
                s_n = 0;
                if (job && xTaskCreatePinnedToCoreWithCaps([](void *arg) {
                        auto *j = static_cast<ScanJob *>(arg);
                        j->n = fw_library_scan(j->pkgs, 16);
                        if (j->state.exchange(JOB_DONE) == JOB_ABANDONED) delete j;
                        s_scanRunning.store(false);
                        vTaskDeleteWithCaps(nullptr);
                    }, "twin_fw_scan", 8192, job, 2, nullptr, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) == pdPASS) {
                    if (job_wait(job->state, 5000)) {
                        s_n = job->n;
                        for (size_t k = 0; k < s_n; ++k) s_pkgs[k] = job->pkgs[k];
                        delete job;
                    }
                } else {
                    delete job;
                    s_scanRunning.store(false);
                }
            }
            if (i < 0 || (size_t)i >= s_n) {
                snprintf(reply, sizeof reply, "TWIN_FW L %u %d\r\n", (unsigned)s_n, i);
            } else {
                const FwPackage &k = s_pkgs[i];
                snprintf(reply, sizeof reply, "TWIN_FW L %u %d %d %u %u %lu %d %d %d %s\t%s\t%s\r\n", (unsigned)s_n, i,
                         (int)k.verdict, k.boardRev, k.stmProtoMin, (unsigned long)k.imageSize, k.hasAssets ? 1 : 0,
                         k.hasSettings ? 1 : 0, fw_package_stm_proto_older(k) ? 1 : 0, k.name, k.version, k.kit);
            }
        } else {
            snprintf(reply, sizeof reply, "ERR TWIN_FW M|S n|L n\r\n");
        }
        cdc_reply(reply);
        return true;
    }
    /* TWIN_SETTINGS <offset> -- the settings as NVS holds them
     * (twin_state::settings_text), for a twin whose set= hash disagrees:
     * snapshotted at offset 0, then read out 400 characters at a time. */
    if (strncmp(cmd, "TWIN_SETTINGS ", 14) == 0) {
        static std::string s_text;
        const size_t off = strtoul(cmd + 14, nullptr, 10);
        if (off == 0) {
            /* Described on the LVGL task into a job of its own: one that comes
             * after the wait writes nothing this verb reads. */
            struct TextJob { std::string text; std::atomic<int> state{JOB_RUNNING}; };
            s_text.clear();
            if (auto *job = new (std::nothrow) TextJob) {
                lv_async_call_locked([](void *arg) {
                    auto *j = static_cast<TextJob *>(arg);
                    if (auto *cfg = crosspad::CrosspadSettings::getInstance()) j->text = twin_state::settings_text(*cfg);
                    if (j->state.exchange(JOB_DONE) == JOB_ABANDONED) delete j;
                }, job);
                if (job_wait(job->state, 1000)) { s_text = std::move(job->text); delete job; }
            }
        }
        EXT_RAM_BSS_ATTR static char reply[440];
        const size_t n = off < s_text.size() ? std::min<size_t>(400, s_text.size() - off) : 0;
        snprintf(reply, sizeof reply, "TWIN_SETTINGS %u %u %.*s\r\n", (unsigned)s_text.size(), (unsigned)off,
                 (int)n, s_text.c_str() + (off < s_text.size() ? off : 0));
        cdc_reply(reply);
        return true;
    }
    /* TWIN_STATE -- what the twin checks itself against (twin_state.h),
     * described on the LVGL task at one tick. */
    /* TWIN_HELLO -- which twin can mirror this board: the protocol and the source id of
     * what the screen is built from (twin_state.h kProtocol, tools/twin_source_id.cmake). */
    if (strcmp(cmd, "TWIN_HELLO") == 0) {
        char reply[160];
        snprintf(reply, sizeof reply, "TWIN_HELLO proto=%d src=%s ver=%s\r\n", twin_state::kProtocol, twin_source_id,
                 esp_app_get_description()->version);
        cdc_reply(reply);
        return true;
    }
    if (strcmp(cmd, "TWIN_STATE") == 0) {
        /* the companion reply holds 480; described on the LVGL task into a job
         * of its own, so one that comes after the wait writes nothing read here */
        struct StateJob { char text[470]; std::atomic<int> state{JOB_RUNNING}; };
        EXT_RAM_BSS_ATTR static char s_reply[470];
        auto *job = new (std::nothrow) StateJob;
        if (!job) { cdc_reply("ERR TWIN_STATE no_mem\r\n"); return true; }
        lv_async_call_locked([](void *arg) {
            auto *j = static_cast<StateJob *>(arg);
            char (&s_state)[470] = j->text;
            auto *running = crosspad_gui::AppOrchestrator::getInstance().getRunningApp();
            if (running && !running->isStarted()) running = nullptr;
            /* The settings the screen is drawn from: launcher style, theme
             * colour, the drawer, the status bar's perf column. */
            auto *cfg = crosspad::CrosspadSettings::getInstance();
            const int n = snprintf(s_state, sizeof s_state, "TWIN_STATE tick=%lu app=%s cfg=L%uT%uQ%uP%u ",
                                   (unsigned long)lv_tick_get(), running ? running->getName() : "-",
                                   cfg ? (unsigned)cfg->launcherStyle : 0u, cfg ? (unsigned)cfg->themeColorIndex : 0u,
                                   cfg && cfg->quickSettingsEnabled ? 1u : 0u, cfg ? (unsigned)cfg->perfStatsFlags : 0u);
            const size_t m = n + twin_state::describe(s_state + n, sizeof s_state - n - 2, app_c,
                                 crosspad_gui::getOverlayParent(),
                                 indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr);
            /* What the status bar and the drawer show that no input decides:
             * levels the drawer's sliders hold, and the board's telemetry. */
            const crosspad_gui::HeapStats h = crosspad_gui::getGuiPlatform().getHeapStats();
            snprintf(s_state + m, sizeof s_state - m - 2,
                     " lvl=V%uR%uB%u st=b%dc%ds%dh%du%da%dn%dw%dm%up%u",
                     cfg ? (unsigned)cfg->masterFX.outVolumeCodec[0][0] : 0u, cfg ? (unsigned)cfg->RGBbrightness : 0u,
                     cfg ? (unsigned)cfg->LCDbrightness : 0u, (int)status.batteryPct,
                     status.stm32.chargingStatus ? 1 : 0, status.stm32.sdDetected ? 1 : 0,
                     status.hostAlive(crosspad::getClock().getTimeUs()) ? 1 : 0, status.fwUpdateAvailable ? 1 : 0,
                     status.bluetoothActive ? 1 : 0, status.bluetoothConnected ? 1 : 0, status.wifiConnected ? 1 : 0,
                     h.sram_total ? (unsigned)((h.sram_total - h.sram_free) * 100 / h.sram_total) : 0u,
                     h.psram_total ? (unsigned)((h.psram_total - h.psram_free) * 100 / h.psram_total) : 0u);
            /* What apps read from the platform rather than from an input: the
             * audio routing, whether the STM takes pad config, the host the hub
             * reported, the song project open on the card (spaces as %20). */
            auto &gp = crosspad_gui::getGuiPlatform();
            crosspad_gui::AudioRouteState ar{};
            const bool haveAr = gp.getAudioRoute(ar);
            char song[40] = "-";
            if (auto *se = crosspad::getPlatformServices().songEngine) {
                std::lock_guard<std::recursive_mutex> g(se->songMutex());
                size_t o = 0;
                for (char c : se->song().name) {
                    if (o + 4 >= sizeof song) break;
                    if (c == ' ' || c == '%') o += (size_t)snprintf(song + o, sizeof song - o, "%%%02X", (unsigned)(uint8_t)c);
                    else song[o++] = c;
                }
                song[o ? o : 1] = 0;
            }
            const size_t k = strlen(s_state);
            snprintf(s_state + k, sizeof s_state - k - 2,
                     " ar=%d,%u,%u,%u,%u,%u,%u,%u,%u sc=%d host=%u,%lu,%lu,%u,%u fwl=%s song=%s set=%08lx"
                     " ble=%s,%s imu=%lu",
                     haveAr ? 1 : 0, ar.captureCodec, ar.adcInput[0], ar.adcInput[1], ar.dacOutput[0], ar.dacOutput[1],
                     ar.mute[0], ar.mute[1], ar.preset, gp.hasStmConfigWrite() ? 1 : 0,
                     (unsigned)status.hostCpuPct, (unsigned long)status.hostRamUsedMb, (unsigned long)status.hostRamTotalMb,
                     (unsigned)status.hostDawsInstalled, (unsigned)status.hostDawsRunning,
                     status.fwLatest[0] ? status.fwLatest : "-", song,
                     cfg ? (unsigned long)twin_state::settings_hash(*cfg) : 0ul,
                     crosspad::EspBleMidi::isSupported() ? crosspad::getEspBleMidi().getStateName() : "-",
                     crosspad::EspBleMidi::isSupported() && !crosspad::getEspBleMidi().getConnectedDevice().address.empty()
                         ? crosspad::getEspBleMidi().getConnectedDevice().address.c_str() : "-",
                     (unsigned long)imu_midi_sent());
            /* A line that filled the buffer lost its tail fields (snprintf cut it): the
             * twin then compares less than it thinks. docs/subsystems/twin-protocol.md. */
            if (strlen(s_state) >= sizeof s_state - 3) ESP_LOGW(TAG, "TWIN_STATE cut at %u bytes: fields lost", (unsigned)strlen(s_state));
            strcat(s_state, "\r\n");
            if (j->state.exchange(JOB_DONE) == JOB_ABANDONED) delete j;
        }, job);
        if (!job_wait(job->state, 1000)) { cdc_reply("ERR TWIN_STATE busy\r\n"); return true; }
        memcpy(s_reply, job->text, sizeof s_reply);
        delete job;
        cdc_reply(s_reply);
        return true;
    }
    return false;
}

bool verb_twin(VerbLine &l)
{
    char *cmd = l.cmd;
    /* TWIN 1|0 -- the browser twin's keepalive (twin_mirror.h): while it keeps
     * coming, the LVGL inputs go out as SysEx 09 frames. */
    if (strncmp(cmd, "TWIN ", 5) == 0) {
        twin_mirror_enable(cmd[5] == '1');
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_motion(VerbLine &l)
{
    char *cmd = l.cmd;
    /* MOTION 1|0 -- a host page's keepalive (motion_stream.h): while it keeps
     * coming, the IMU goes out as SysEx 0A frames. */
    if (strncmp(cmd, "MOTION ", 7) == 0) {
        motion_stream_enable(cmd[7] == '1');
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_twin_2(VerbLine &l)
{
    char *cmd = l.cmd;
    /* TWIN_INFO P|C|<row> -- what Settings shows that the board alone knows,
     * through its own ISettingsUI: P the platform name and info, C the
     * coprocessor firmware, <row> an Info page row. Tab-separated fields. */
    /* The TWIN_* verbs read the GUI: a twin polling across a reboot must not
     * reach it before it exists (getGuiPlatform() asserts). */
    if (strncmp(cmd, "TWIN_", 5) == 0 && (!status.isGuiInitialised || !app_c)) {
        cdc_reply("ERR TWIN not_ready\r\n");
        return true;
    }
    return false;
}

bool verb_twin_3(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "TWIN_", 5) == 0) {
        SemaphoreHandle_t lock = twin_verb_lock();
        if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(2000)) != pdTRUE) { cdc_reply("ERR TWIN busy\r\n"); return true; }
        const bool handled = twin_verb(cmd);
        xSemaphoreGive(lock);
        if (handled) return true;
    }
    return false;
}
