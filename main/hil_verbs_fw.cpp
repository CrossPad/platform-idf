/* CDC verbs: the firmware library and A/B slots. Dispatch order is in hil_control.cpp. */
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

/* fw_library_scan() opens up to 16 images off the SD card — the same card the
 * sample engine streams from — so it never runs on the priority-1 CDC drain
 * loop. No worker already takes foreign jobs (fx_worker's switch is closed
 * over file_transfer.cpp's own ops), so a short-lived task does the scan and
 * replies for itself, the same shape as PITCHED_BENCH's own task below. */
static volatile bool s_fw_list_busy = false;

static void handle_fw_list(void)
{
    EXT_RAM_BSS_ATTR static FwPackage pkgs[16];
    size_t n = fw_library_scan(pkgs, 16);
    char row[224];
    for (size_t i = 0; i < n; i++) {
        /* A local copy rather than pkgs[i] directly: the compiler cannot
         * bound a %s read through a runtime-indexed array element, and
         * treats every such field as unboundedly long, which overflows its
         * own truncation check across four %s directives in one call. */
        FwPackage p = pkgs[i];
        /* crosspad::fwPkgVerdictText() returns a literal from another translation
         * unit, so its length is not visible here either; copy it into a
         * buffer sized for the longest verdict text instead of handing
         * snprintf a raw pointer of unknown length. */
        char verdict[40];
        strlcpy(verdict, crosspad::fwPkgVerdictText(p.verdict), sizeof(verdict));
        snprintf(row, sizeof(row), "FWPKG: name=%s kit=%s ver=%s rev=%u size=%u assets=%d settings=%d verdict=%s\r\n",
                 p.name, p.kit, p.version, p.boardRev,
                 (unsigned)p.imageSize, p.hasAssets, p.hasSettings, verdict);
        cdc_reply(row);
    }
    snprintf(row, sizeof(row), "FWPKG: count=%u\r\n", (unsigned)n);
    cdc_reply(row);
}

static void fw_list_task(void *)
{
    handle_fw_list();
    s_fw_list_busy = false;
    /* Created WithCaps, so it must be deleted WithCaps or the stack leaks. */
    vTaskDeleteWithCaps(nullptr);
}

static void handle_fw_slots(void)
{
    static const char *kStateNames[] = { "empty", "installed", "trial", "confirmed", "rolledback" };
    FwSlotInfo slots[2];
    fw_slots_get(slots);
    for (int i = 0; i < 2; i++) {
        /* A local copy, not slots[i] directly: same reason as handle_fw_list()
         * above — a runtime-indexed array element defeats the compiler's
         * format-truncation bound on every %s pulled through it. */
        FwSlotInfo s = slots[i];
        char row[160];
        snprintf(row, sizeof(row), "FWSLOT: slot=%c running=%d state=%s pkg=%s ver=%s kit=%s\r\n",
                 s.label, s.running ? 1 : 0, kStateNames[(int)s.state],
                 s.package[0] ? s.package : "-", s.version[0] ? s.version : "-",
                 s.kit[0] ? s.kit : "-");
        cdc_reply(row);
    }
    char trailer[32];
    snprintf(trailer, sizeof(trailer), "FWSLOT: rollback=%d\r\n", fw_rollback_supported() ? 1 : 0);
    cdc_reply(trailer);
}

/* fw_confirm_running() never writes flash itself — it hands off to its own
 * internal-RAM-stack task (see fw_slots.cpp) — so, unlike FW_INSTALL/FW_SWITCH,
 * FW_CONFIRM needs no task of its own here. */
static void handle_fw_confirm(void)
{
    fw_confirm_running();
    cdc_reply("OK\r\n");
}

/* fw_install_start() looks a package up (fw_library_find() -> fill_package(),
 * which alone carries an esp_app_desc_t — 256 B — plus a couple of path
 * buffers and a struct stat, on top of fw_install_start()'s and
 * handle_fw_install()'s own locals) before it can answer OK/ERR. Measured on
 * the bench: that whole chain, run straight off the CDC drain loop, panics
 * the "main" task with a NULL-deref inside vfprintf the moment a *found*
 * package reaches fill_package() (FW_INSTALL against a name that is not on
 * the card returns before that point and is fine; FW_INSTALL test-v1 and
 * test-v2, both found, both took the board down). FW_SLOTS' own
 * esp_app_desc_t use survives on that same stack because its call chain is
 * shorter — this one needs the extra headroom a dedicated task has, the
 * same reasoning FW_LIST already runs on one. */
static volatile bool s_fw_install_cmd_busy = false;

struct FwInstallCmdArgs {
    char name[FW_PKG_NAME_LEN];
    bool force;
};
static FwInstallCmdArgs s_fw_install_cmd_args;

static void fw_install_cmd_task(void *)
{
    const char *error = nullptr;
    if (fw_install_start(s_fw_install_cmd_args.name, s_fw_install_cmd_args.force, &error)) {
        cdc_reply(fw_install_stm_proto_older() ? "OK warn=stm_proto_older\r\n" : "OK\r\n");
    } else {
        /* From the call, not from the status: a refusal because another
         * install is running deliberately leaves that install's own status
         * alone. */
        char reply[96];
        snprintf(reply, sizeof(reply), "ERR %s\r\n", (error && error[0]) ? error : "refused");
        cdc_reply(reply);
    }
    s_fw_install_cmd_busy = false;
    vTaskDeleteWithCaps(nullptr);
}

static void handle_fw_install(const char *args)
{
    char name[FW_PKG_NAME_LEN] = {0};
    char extra[16] = {0};
    sscanf(args, "%47s %15s", name, extra);
    if (!name[0]) {
        cdc_reply("ERR FW_INSTALL <name> [FORCE]\r\n");
        return;
    }
    if (s_fw_install_cmd_busy) {
        cdc_reply("ERR busy\r\n");
        return;
    }
    s_fw_install_cmd_busy = true;
    strlcpy(s_fw_install_cmd_args.name, name, sizeof(s_fw_install_cmd_args.name));
    s_fw_install_cmd_args.force = strcmp(extra, "FORCE") == 0;

    if (xTaskCreatePinnedToCoreWithCaps(fw_install_cmd_task, "fw_install_cmd", 8192,
                                        nullptr, 1, nullptr, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_fw_install_cmd_busy = false;
        cdc_reply("ERR no task\r\n");
    }
}

static void handle_fw_install_status(void)
{
    static const char *kPhaseNames[] = { "idle", "verifying", "writing", "done", "failed" };
    const FwInstallStatus st = fw_install_status();
    char reply[128];
    snprintf(reply, sizeof(reply), "FWINST: phase=%s percent=%u error=%s\r\n",
             kPhaseNames[(int)st.phase], (unsigned)st.percent,
             st.error[0] ? st.error : "-");
    cdc_reply(reply);
}

/* Same reasoning as fw_install_cmd_task() above: fw_switch_request() can
 * walk the settings JsonDocument + stdio machinery (exporting the leaving
 * slot's own settings before it moves the boot partition), so it never runs
 * on the CDC drain loop's own stack. Success replies before the restart it
 * then triggers — SETTINGS_RESET's own reply-then-reboot shape — because
 * fw_switch_request() has no CDC of its own to flush first.
 *
 * Internal RAM, not PSRAM, unlike fw_install_cmd_task/fw_list_task: those
 * only look things up and hand the real work to a task of their own, but
 * fw_switch_request() itself writes flash directly (nvs_commit(), then
 * esp_ota_set_boot_partition()) — measured on hardware as a coredump
 * (task=fw_switch_cmd, cause=0, vaddr=0x0, right in that tail) every single
 * time. A PSRAM-backed stack is unusable the moment its own call frame
 * reaches into a flash write: the write's critical section disables the
 * cache the CPU also needs for every push/pop on that stack, unlike a plain
 * PSRAM *data* buffer (fw_install_task's chunk, or ArduinoJson's own heap),
 * which the driver stages before entering that section. */
static volatile bool s_fw_switch_cmd_busy = false;

static void fw_switch_cmd_task(void *)
{
    const char *error = nullptr;
    if (fw_switch_request(&error)) {
        cdc_reply("OK reboot=1\r\n");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    char reply[96];
    snprintf(reply, sizeof(reply), "ERR %s\r\n", error && error[0] ? error : "refused");
    cdc_reply(reply);
    s_fw_switch_cmd_busy = false;
    vTaskDeleteWithCaps(nullptr);
}

static void handle_fw_switch(void)
{
    if (s_fw_switch_cmd_busy) {
        cdc_reply("ERR busy\r\n");
        return;
    }
    s_fw_switch_cmd_busy = true;
    if (xTaskCreatePinnedToCoreWithCaps(fw_switch_cmd_task, "fw_switch_cmd", 8192,
                                        nullptr, 1, nullptr, 0,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_fw_switch_cmd_busy = false;
        cdc_reply("ERR no task\r\n");
    }
}

bool verb_fw_list(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "FW_LIST", 7) == 0) {
        if (s_fw_list_busy) { cdc_reply("FWPKG: ERR busy\r\n"); return true; }
        s_fw_list_busy = true;
        /* Stack from PSRAM, sized like PITCHED_BENCH's own task: this task
         * never runs with the flash cache disabled. */
        if (xTaskCreatePinnedToCoreWithCaps(fw_list_task, "fw_list", 8192,
                                            nullptr, 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            s_fw_list_busy = false;
            cdc_reply("FWPKG: ERR no task\r\n");
        }
        return true;
    }
    return false;
}

bool verb_fw_slots(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "FW_SLOTS", 8) == 0) {
        handle_fw_slots();
        return true;
    }
    return false;
}

bool verb_fw_install(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "FW_INSTALL", 10) == 0) {
        const char *args = cmd + 10;
        while (*args == ' ') args++;
        if (*args) handle_fw_install(args);
        else       handle_fw_install_status();
        return true;
    }
    return false;
}

bool verb_fw_switch(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "FW_SWITCH", 9) == 0) {
        handle_fw_switch();
        return true;
    }
    return false;
}

bool verb_fw_confirm(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "FW_CONFIRM", 10) == 0) {
        handle_fw_confirm();
        return true;
    }
    return false;
}
