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
/* Defined in CrosspadPlatform.cpp beside getMixerEngine(); the metronome is the
 * sequencer's clock, so the bench needs to reach it. */
crosspad::MetronomeNode& getMetronome();
crosspad::PatternSequencer& getSequencer();
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
/* The sdr kit's verbs (components/crosspad-sdr), absent from every other image. */
extern "C" bool sdr_verbs_handle(const char *cmd, void (*reply)(const char *text)) __attribute__((weak));
extern "C" uint32_t audio_rx_skips(void);
extern "C" uint32_t audio_rt_unpaced_passes(void);
extern "C" void     audio_rt_prof_enable(bool on);
extern "C" void     audio_rt_prof_read(char *out, uint32_t cap);
extern "C" void     audio_rt_set_queue_frames(uint32_t frames);
extern "C" uint32_t audio_rt_queue_frames(void);
extern "C" uint32_t audio_rt_block_frames(void);
extern "C" uint32_t audio_rt_pacer_resyncs(void);
extern "C" uint32_t audio_rt_tx_underruns(void);
extern "C" void     audio_mixer_prof_enable(bool on, void *mixer);
extern "C" void     audio_mixer_prof_read(void *mixer, char *out, uint32_t cap);
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

static const char *TAG = "hil_control";

extern crosspad::CrosspadStatus status;

std::atomic<uint32_t> g_hil_pad_press_count{0};
std::atomic<uint32_t> g_hil_pad_release_count{0};
std::atomic<uint32_t> g_hil_note_on_count{0};
std::atomic<uint16_t> g_hil_last_note_on{0};
extern std::atomic<uint32_t> g_hil_stm_press_count;
extern std::atomic<uint32_t> g_hil_stm_at_count;
extern std::atomic<uint32_t> g_stm_uart_ovf;      /* stm32_uart.cpp */
extern std::atomic<uint32_t> g_stm_sbuf_drop;     /* EspStm32MidiTransport.cpp */
extern std::atomic<uint32_t> g_usb_midi_tx_ok;    /* midi_esp_tinyusb_transport.cpp */
extern std::atomic<uint32_t> g_usb_midi_tx_fail;
extern std::atomic<uint8_t>  g_hil_stm_last_note;
extern std::atomic<uint8_t>  g_hil_stm_last_pad;
struct EngineNoteEvent { uint8_t note; uint8_t wavIdx; uint32_t ms; };
extern EngineNoteEvent g_engine_last[8];
struct PortCallers { uint32_t audio_bg; uint32_t other; char lastOther[16]; };
extern PortCallers g_port_opens, g_port_seeks, g_port_closes, g_port_reads;
extern uint32_t g_port_defer, g_port_defer_lost, g_port_evict;
extern uint32_t g_port_staged, g_port_stage_fallback;   /* sample_player_port.cpp, since boot */
extern uint32_t g_port_stream_staged, g_port_stream_fb, g_port_stream_read_max_us;
extern uint32_t g_late_max_ms, g_late_lost, g_reload_win_max_us;   /* SamplerPlatformPort.cpp: held hits, since the last SMPLLAST */
extern uint32_t g_port_pf_hits, g_port_pf_misses;   /* sample_player_port.cpp, since boot */
extern std::atomic<uint32_t> g_port_stream_bytes;
extern std::atomic<uint32_t> g_note_lat_last_us, g_note_lat_max_us, g_note_lat_count;
extern std::atomic<uint32_t> g_note_lat_sum_us;
extern std::atomic<uint32_t> g_engine_noblock, g_engine_underrun;
extern std::atomic<uint32_t> g_rt_overruns;   /* audio_task.cpp */
extern std::atomic<uint32_t> g_engine_last_count;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
extern void sample_player_port_set_poly_cap(uint8_t cap);   /* sample_player_port.cpp */
/* Free sample slots of whichever engine holds the kit (-1: none does). */
static int hil_free_slots(void)
{
    const int n = native_sampler_free_slots();
    return n >= 0 ? n : marcel_engine_free_slots();
}
#else
static int hil_free_slots(void) { return native_sampler_free_slots(); }
#endif

/* Serialises cdc_reply(). Two tasks write the same FIFO: the CDC drain loop
 * and PITCHED_BENCH's own task, which emits a row per measurement while the
 * loop keeps answering other verbs. Without this a "BENCH:" row lands inside
 * another reply's line and the host's prefix reader drops both. Task context
 * only — no ISR reaches cdc_reply (the TinyUSB RX callback queues to
 * app_queue; nothing else calls in), so the plain mutex is the right one. */
static SemaphoreHandle_t cdc_reply_lock(void)
{
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}

/* The CDC TX FIFO is 256 bytes and the queue call silently drops what does
 * not fit, so a reply longer than that goes out in flushed pieces. */
static std::atomic<TaskHandle_t> companionReplyTask{nullptr};
static char *companionReplyBuffer = nullptr;
static size_t companionReplyCapacity = 0;
static void cdc_reply(const char *text)
{
    if (companionReplyTask.load() == xTaskGetCurrentTaskHandle()) {
        const size_t used = strlen(companionReplyBuffer);
        if (used + strlen(text) >= companionReplyCapacity) snprintf(companionReplyBuffer, companionReplyCapacity, "ERR response_too_large\r\n");
        else memcpy(companionReplyBuffer + used, text, strlen(text) + 1);
        return;
    }
    SemaphoreHandle_t lock = cdc_reply_lock();
    /* Bounded, and sent anyway on timeout: a reply the host never sees reads as
     * a dead board, which is worse than one interleaved line. */
    const bool held = lock && xSemaphoreTake(lock, pdMS_TO_TICKS(1000)) == pdTRUE;
    size_t left = strlen(text);
    while (left > 0) {
        const size_t n = left < 128 ? left : 128;
        tinyusb_cdcacm_write_queue(TINYUSB_CDC_ACM_0, (const uint8_t *)text, n);
        tinyusb_cdcacm_write_flush(TINYUSB_CDC_ACM_0, pdMS_TO_TICKS(50));
        text += n;
        left -= n;
    }
    if (held) xSemaphoreGive(lock);
}

static uint32_t s_stack_min = UINT32_MAX;
static char     s_stack_verb[20] = "-";

/* FreeRTOS fills a new stack with this byte (tasks.c tskSTACK_FILL_BYTE), and
 * uxTaskGetStackHighWaterMark() counts how many of it are left at the bottom. */
static constexpr uint8_t kStackFillByte = 0xa5;
/* Left alone below the caller's stack pointer: memset's frame and the save
 * area under it are the only live bytes down there. */
static constexpr size_t kStackPaintGap = 256;
/* Left alone at the bottom: the stack-end watchpoint covers 32 bytes there,
 * aligned up to 32 (port.c vPortSetStackWatchpoint), and a store to it panics. */
static constexpr size_t kStackWatchedBottom = 64;

/* The kernel's mark is the deepest this task has ever been, boot included:
 * the free part is filled again before every command, so the mark read after
 * it is that command's alone. */
void hil_stack_mark(void)
{
    uint8_t *const bottom = pxTaskGetStackStart(nullptr) + kStackWatchedBottom;
    uint8_t *const below = static_cast<uint8_t *>(esp_cpu_get_sp()) - kStackPaintGap;
    if (below > bottom) memset(bottom, kStackFillByte, (size_t)(below - bottom));
}

uint32_t hil_stack_note(const char *line, size_t len)
{
    const uint32_t freeNow = uxTaskGetStackHighWaterMark(nullptr);
    if (freeNow >= s_stack_min) return freeNow;
    s_stack_min = freeNow;
    size_t n = 0;
    while (n < len && n < sizeof(s_stack_verb) - 1 && line[n] != ' ') ++n;
    memcpy(s_stack_verb, line, n);
    s_stack_verb[n] = '\0';
    return freeNow;
}

/* ── App control (must run on the LVGL task) ───────────────────────── */

// Single in-flight launch request — lv_async_call runs before the next CDC
// command can arrive.
EXT_RAM_BSS_ATTR static char s_pending_app[32];

static crosspad_gui::ILvglApp *find_app(const char *name)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    for (const auto &app : orch.getApps()) {
        if (app && crosspad::control::appNameMatches(app->getName(), name)) {
            return app.get();
        }
    }
    return nullptr;
}

static void async_app_start(void *)
{
    auto *app = find_app(s_pending_app);
    if (!app) {
        ESP_LOGW(TAG, "APP_START: '%s' not found", s_pending_app);
        return;
    }
    ESP_LOGI(TAG, "APP_START: launching '%s'", s_pending_app);
    crosspad_gui::AppOrchestrator::onAppSelected(app, app_c);
}

/* APP_STOP rebuilds the launcher, which exists only once LoadMainScreen() has
 * run in a normal boot (isGuiInitialised is set after it; transfer and network
 * mode set it too but have no launcher). Earlier -- the splash, before
 * crosspad_gui::bootstrap() -- LoadMainScreen() asserts in getGuiPlatform(). */
static bool launcher_up(void)
{
    return status.isGuiInitialised && !usb_transfer_active() && !net_mode_active();
}

static void async_app_stop(void *)
{
    if (!launcher_up()) {
        ESP_LOGW(TAG, "APP_STOP: no launcher in this boot (yet), ignored");
        return;
    }
    ESP_LOGI(TAG, "APP_STOP: back to launcher");
    LoadMainScreen(NULL);
}

// The app-initiated exit, as an in-app back button takes it. Unlike APP_STOP
// it does not rebuild the main screen, so teardown leftovers (encoder
// ownership above all) stay observable.
static void async_app_destroy(void *)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    auto *running = orch.getRunningApp();
    if (running && running->isStarted()) {
        running->destroyApp();
        orch.setRunningApp(nullptr);
    }
}

// The framework's own self-close, with no harness bookkeeping: whatever is
// left behind is the app's or the framework's doing.
static void async_app_self_close(void *)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    auto *running = orch.getRunningApp();
    if (running && running->isStarted()) {
        running->destroyAppAsync();
    }
}

// The kit-selector "Load" path. Loading before APP_START Sampler keeps the
// pre-launch overlay from triggering, so scripts stay non-interactive.
static int s_pending_kit = -1;

static void async_kit_load(void *)
{
    // On the LVGL thread: hand the whole load to the background task.
    const bool busy = crosspad_sampler::kit_load_task_is_loading();
    crosspad_sampler::kit_load_task_start(false, s_pending_kit);
    // Say which of the two happened — a queued request must not read as a
    // started one.
    ESP_LOGI(TAG, "KIT_LOAD %d %s", s_pending_kit, busy ? "queued" : "started");
}

/* KIT_DELETE: the kit browser's delete, on the thread the browser runs it on. */
static int s_delete_kit = -1;

static void async_kit_delete(void *)
{
    auto *mgr = crosspad::getKitManager();
    const bool ok = mgr && mgr->removeKit(s_delete_kit);
    ESP_LOGI(TAG, "KIT_DELETE %d %s", s_delete_kit, ok ? "done" : "refused");
}

/* PAD_MIX / KIT_SAVE: a host mixer (crosspad-hil padmix) riding the current
 * kit's pad volumes the way the Pad Mixer app does, then writing kit.json.
 * The kit model is the LVGL thread's, so a set is parked here per pad --
 * newest wins, a held arrow key on the host coalesces -- and applied there,
 * only to the kit it was asked for. */
static std::atomic<int> s_mix_pending[KIT_PADS];   /* volume + 1; 0 = nothing pending */
static std::atomic<int> s_mix_kit{-1};
static std::atomic<bool> s_mix_posted{false};

static void mix_apply_pending(void)
{
    auto *mgr = crosspad::getKitManager();
    crosspad::KitInfo *kit = (mgr && mgr->getCurrentKitId() == s_mix_kit.load()) ? mgr->getCurrentKit() : nullptr;
    auto *mix = crosspad::getPlatformServices().padMix;
    for (int i = 0; i < KIT_PADS; ++i) {
        const int v = s_mix_pending[i].exchange(0);
        if (!v || !kit) continue;
        crosspad::Pad &pad = kit->pads[i];
        pad.volume = v - 1;
        pad.dirtyModified = true;   /* as the Pad Mixer marks it: the Sampler's save prompt offers it */
        if (mix) mix->setPadMix((uint8_t)i, pad.volume, pad.pan);
    }
}

static void async_pad_mix(void *)
{
    s_mix_posted.store(false);   /* first: a set arriving meanwhile posts again */
    mix_apply_pending();
}

static std::atomic<bool> s_kit_save_busy{false};
static std::atomic<int> s_kit_save_ok{-1};
static std::atomic<int> s_kit_save_kit{-1};

/* On the LVGL thread, where the Sampler's own save runs. */
static void async_kit_save(void *)
{
    mix_apply_pending();
    auto *mgr = crosspad::getKitManager();
    crosspad::KitInfo *kit = mgr ? mgr->getCurrentKit() : nullptr;
    bool ok = false;
    /* An unparsed kit carries sixteen empty pads: writing it would take every
     * sample out of the kit. */
    if (kit && kit->parsed && mgr->getCurrentKitId() == s_kit_save_kit.load() &&
        !crosspad_sampler::kit_load_task_is_loading()) {
        ok = mgr->saveKit(*kit);
        if (ok) {
            for (int i = 0; i < KIT_PADS; ++i) kit->pads[i].dirtyModified = false;
        }
    }
    s_kit_save_ok.store(ok ? 1 : 0);
    s_kit_save_busy.store(false);
    ESP_LOGI(TAG, "KIT_SAVE %d %s", s_kit_save_kit.load(), ok ? "done" : "failed");
}

/* SysEx 0x1D 0x0B: the same request the CDC verb makes, for the USB-audio
 * profile where there is no CDC to make it on. Same refusals, no reply. */
void hil_kit_load(int kitId)
{
    if (!g_kit_scan_done.load() || !crosspad::control::kitIdLoadable(kitId)) {
        ESP_LOGW(TAG, "KIT_LOAD %d refused (scan=%d)", kitId, g_kit_scan_done.load() ? 1 : 0);
        return;
    }
    s_pending_kit = kitId;
    lv_async_call_locked(async_kit_load, nullptr);
}

/* Bench pad hits play from this task, not from the loop that received them.
 * A hit is a synchronous call into the sample engine and waits on the card
 * while a kit loads — CDC_STATS showed maxcmd=PAD_PRESS at 1.14 s — which in
 * the CDC drain loop held every reply behind it and KIT_STATUS timed out on
 * the host. Same priority and stack as the STM link task that plays the
 * physical pads, so a bench hit competes for the card like a real one. */
struct PadInjectMsg { HilPadInject kind; uint8_t pad; uint8_t val; };
static QueueHandle_t s_pad_inject_queue;
std::atomic<uint32_t> g_hil_pad_inject_drop{0};

static void pad_inject_task(void *)
{
    PadInjectMsg m;
    for (;;) {
        if (xQueueReceive(s_pad_inject_queue, &m, portMAX_DELAY) != pdTRUE) continue;
        if (m.kind == HilPadInject::Press)        twin_mirror_pad(m.pad, m.val);
        else if (m.kind == HilPadInject::Release) twin_mirror_pad(m.pad, 0);
        if (m.kind == HilPadInject::Release) {
            /* Injected presses record like played ones, through the pad
             * manager's input tap (midiCallbacks.cpp) -- otherwise no scenario
             * could ever test the loop recorder -- and their releases end the
             * notes, so a scenario can record a length. */
            getSequencer().noteOut(m.pad);
        }
        crosspad::control::applyPadInput(crosspad::getPadManager(), m.kind, m.pad, m.val);
    }
}

void hil_pad_inject_init(void)
{
    s_pad_inject_queue = xQueueCreateWithCaps(32, sizeof(PadInjectMsg), MALLOC_CAP_SPIRAM);
    assert(s_pad_inject_queue);
    xTaskCreatePinnedToCoreWithCaps(pad_inject_task, "hil_pad", 4000, nullptr, 6, nullptr, 0,
                                    MALLOC_CAP_SPIRAM);
}

/* For the CDC verbs: the hil_pad task runs at 6 on this core, so a send
 * hands it the CPU before the reply goes out -- and a hit can wait on the card
 * for a second while a kit loads. Room is checked first (only an error is
 * worth answering late), the reply goes out, then the hit is queued. */
static void pad_inject_reply_first(HilPadInject kind, uint8_t pad, uint8_t val, void (*reply)(const char*))
{
    if (!s_pad_inject_queue || uxQueueSpacesAvailable(s_pad_inject_queue) == 0) {
        ++g_hil_pad_inject_drop;
        reply("ERR busy\r\n");
        return;
    }
    reply("OK\r\n");
    hil_pad_inject(kind, pad, val);
}

bool hil_pad_inject(HilPadInject kind, uint8_t pad, uint8_t val)
{
    const PadInjectMsg m{kind, pad, val};
    if (!s_pad_inject_queue || xQueueSend(s_pad_inject_queue, &m, 0) != pdTRUE) {
        ++g_hil_pad_inject_drop;
        return false;
    }
    if (kind == HilPadInject::Press)        ++g_hil_pad_press_count;
    else if (kind == HilPadInject::Release) ++g_hil_pad_release_count;
    return true;
}

/* The last kit loads, one row each, oldest first: when they were asked for,
 * how long the parse and the pad loop took, which pads waited for their voice
 * to end, and what the engine skipped meanwhile. A swap that sounded wrong
 * becomes numbers here. */
static void handle_kit_swap_log(void)
{
    /* Static, both of them: this runs on the main task's stack, which the
     * CDC drain loop keeps small. One record at a time — a ring's worth of
     * them was 1.2 kB of internal RAM for a verb that prints. */
    static crosspad_sampler::KitSwapRecord r;
    EXT_RAM_BSS_ATTR static char out[512];
    static const char* const kWhy[] = {"-", "parse", "none_readable"};   /* KitLoadWhy */
    const int n = crosspad_sampler::kit_load_task_swap_log_count();
    for (int i = 0; i < n; ++i) {
        if (!crosspad_sampler::kit_load_task_swap_log(i, &r)) break;
        size_t len = snprintf(out, sizeof(out),
                              "KITSWAP: seq=%u kit=%d ok=%d req_ms=%u parse_ms=%u done_ms=%u "
                              "gated_blocks=%u gate_max_us=%u gate_total_us=%u gate_count=%u "
                              "underruns=%u noblock=%u deferred=0x%04x forced=0x%04x "
                              "dropped_hits=%u late_hits=%u rt_gap_max_us=%u "
                              "stream_fopens=%u bg_pass_max_us=%u fopen_max_us=%u "
                              "stream_kb=%u loader_kb=%u empty=0x%04x failed=0x%04x why=%s pads=",
                              (unsigned)r.seq, r.kitId, r.ok ? 1 : 0, (unsigned)r.reqMs,
                              (unsigned)r.parseMs, (unsigned)r.doneMs,
                              (unsigned)r.engine.gatedBlocks, (unsigned)r.engine.gateMaxUs,
                              (unsigned)r.engine.gateTotalUs, (unsigned)r.engine.gateCount,
                              (unsigned)r.engine.underruns, (unsigned)r.engine.noFreeBlock,
                              (unsigned)r.deferred, (unsigned)r.forced,
                              (unsigned)r.engine.droppedHits, (unsigned)r.engine.lateHits,
                              (unsigned)r.engine.rtGapMaxUs, (unsigned)r.engine.streamFopens,
                              (unsigned)r.engine.bgPassMaxUs, (unsigned)r.engine.fopenMaxUs,
                              (unsigned)r.engine.streamKb, (unsigned)r.engine.loaderKb,
                              (unsigned)r.empty, (unsigned)r.failed, kWhy[(unsigned)r.why < sizeof kWhy / sizeof kWhy[0] ? (unsigned)r.why : 0]);
        for (int p = 0; p < 16 && len < sizeof(out) - 12; ++p) {
            len += snprintf(out + len, sizeof(out) - len, "%s%d", p ? "," : "",
                            (int)r.padDoneMs[p]);
        }
        snprintf(out + len, sizeof(out) - len, "\r\n");
        cdc_reply(out);
    }
    /* Since boot: preloads read around stdio as one CMD18 per 4 kB, and the
     * ones that fell back to it (no DMA-capable RAM, or no fd position). */
    snprintf(out, sizeof(out), "KITSWAP: io staged=%u fallback=%u\r\n",
             (unsigned)g_port_staged, (unsigned)g_port_stage_fallback);
    cdc_reply(out);
    snprintf(out, sizeof(out), "KITSWAP: end count=%d\r\n", n);
    cdc_reply(out);
}

/* Whether a load is in flight and what waits behind it. KIT_LIST's current=
 * cannot tell "loading" from "the request went nowhere". */
/* Settings backup for the companion: the same JSON file the Settings → Backup
 * page writes. The work runs on the LVGL thread (an import applies brightness,
 * LEDs, vibration and the STM pad config through the GUI platform) and reports
 * through atomics like KIT_LOAD/KIT_STATUS, so the 5 kB main stack never
 * parses JSON. One operation at a time; SETTINGS_STATUS is the result. */
EXT_RAM_BSS_ATTR static char s_settings_path[96];
static std::atomic<bool> s_settings_busy{false};
static std::atomic<int> s_settings_ok{-1};              /* -1 never ran, 0 failed, 1 ok */
static std::atomic<const char *> s_settings_error{nullptr};
static std::atomic<char> s_settings_op{'-'};            /* 'e' export, 'i' import */

static void async_settings_backup(void *)
{
    const char *err = nullptr;
    const bool ok = s_settings_op.load() == 'e'
        ? crosspad_gui::settings_ui_backup_export(s_settings_path, &err)
        : crosspad_gui::settings_ui_backup_import(s_settings_path, &err);
    s_settings_error.store(err);
    s_settings_ok.store(ok ? 1 : 0);
    s_settings_busy.store(false);
}

static bool settings_backup_start(char op, const char *arg)
{
    if (s_settings_busy.load()) return false;
    const char *path = *arg ? arg : crosspad_gui::settings_ui_backup_default_path();
    if (!path || strlen(path) >= sizeof(s_settings_path) || strstr(path, "..")
        || (strncmp(path, "/sdcard/", 8) != 0 && strncmp(path, "/spiflash/", 10) != 0)) {
        return false;
    }
    strcpy(s_settings_path, path);
    s_settings_op.store(op);
    s_settings_ok.store(-1);
    s_settings_error.store(nullptr);
    s_settings_busy.store(true);
    return true;   /* the caller replies, then posts async_settings_backup */
}

static void handle_settings_status(void)
{
    char reply[192];
    const char *err = s_settings_error.load();
    snprintf(reply, sizeof(reply), "SETTINGS: busy=%d op=%c ok=%d err=%s path=%s\r\n",
             s_settings_busy.load() ? 1 : 0, s_settings_op.load(), s_settings_ok.load(),
             err ? err : "-", s_settings_path[0] ? s_settings_path : "-");
    cdc_reply(reply);
}

static void handle_kit_status(void)
{
    char reply[160];
    hil_text_kit_status(reply, sizeof(reply),
                        crosspad::control::readKitStatus(crosspad_sampler::kit_load_task_is_loading(),
                                                         crosspad_sampler::kit_load_task_pending_kit()));
    cdc_reply(reply);
}

/* Waveform cache coverage per sample layer: which pad:layer the panel would
 * draw empty, plus the loader's counters. */
/* Last eight note-ons the engine executed, with the STM's last decoded press
 * beside them — pad 8 sounding like pad 4 is one or the other. */
static void handle_smpl_last(void)
{
    EXT_RAM_BSS_ATTR static char out[640];
    EXT_RAM_BSS_ATTR static uint32_t s_lastBytes, s_lastMs;
    const uint32_t total = g_engine_last_count.load();
    const uint32_t n = total < 8 ? total : 8;
    const uint32_t bytes = g_port_stream_bytes.load(std::memory_order_relaxed);
    const uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t dMs = s_lastMs ? nowMs - s_lastMs : 0;
    size_t len = snprintf(out, sizeof(out), "SMPLLAST: stm_presses=%u stm_note=%u stm_pad=%d engine_total=%u events=",
                          (unsigned)g_hil_stm_press_count.load(), (unsigned)g_hil_stm_last_note.load(),
                          (int)(int8_t)g_hil_stm_last_pad.load() == -1 ? -1 : (int)g_hil_stm_last_pad.load(),
                          (unsigned)total);
    for (uint32_t i = 0; i < n; ++i) {
        const EngineNoteEvent &e = g_engine_last[(total - n + i) % 8];
        len += snprintf(out + len, sizeof(out) - len, "%s%u:%u@%u", i ? "," : "", e.note, e.wavIdx, (unsigned)e.ms);
    }
    snprintf(out + len, sizeof(out) - len,
             " opens=%u/%u(%s) seeks=%u/%u(%s) reads=%u/%u(%s) closes=%u/%u(%s) defer=%u/%u evict=%u"
             " lat_us last=%u avg=%u max=%u n=%u"
             " stream_staged=%u stream_fb=%u stream_kbps=%u stream_read_max_us=%u late_max_ms=%u late_lost=%u pf=%u/%u win_max_us=%u"
             " pool_fixed=0\r\n",
             (unsigned)g_port_opens.audio_bg, (unsigned)g_port_opens.other, g_port_opens.lastOther,
             (unsigned)g_port_seeks.audio_bg, (unsigned)g_port_seeks.other, g_port_seeks.lastOther,
             (unsigned)g_port_reads.audio_bg, (unsigned)g_port_reads.other, g_port_reads.lastOther,
             (unsigned)g_port_closes.audio_bg, (unsigned)g_port_closes.other, g_port_closes.lastOther,
             (unsigned)g_port_defer, (unsigned)g_port_defer_lost, (unsigned)g_port_evict,
             (unsigned)g_note_lat_last_us.load(),
             (unsigned)(g_note_lat_count.load() ? g_note_lat_sum_us.load() / g_note_lat_count.load() : 0),
             (unsigned)g_note_lat_max_us.load(), (unsigned)g_note_lat_count.load(),
             (unsigned)g_port_stream_staged, (unsigned)g_port_stream_fb,
             (unsigned)(dMs ? (bytes - s_lastBytes) / dMs : 0),   /* B/ms = kB/s */
             (unsigned)g_port_stream_read_max_us, (unsigned)g_late_max_ms, (unsigned)g_late_lost,
             (unsigned)g_port_pf_hits, (unsigned)g_port_pf_misses, (unsigned)g_reload_win_max_us);
    /* Rate, worst read and held hits since the previous SMPLLAST. */
    s_lastBytes = bytes;
    s_lastMs = nowMs;
    g_port_stream_read_max_us = 0;
    g_late_max_ms = 0;
    g_late_lost = 0;
    g_reload_win_max_us = 0;
    cdc_reply(out);
}

static void handle_wave_status(void)
{
    crosspad_sampler::WaveformLoaderStats loader;
    crosspad_sampler::waveform_loader_get_stats(&loader);

    /* In PSRAM: this board boots with a couple of hundred bytes of
     * internal RAM to spare, and a reply buffer is read and written
     * only from the CDC loop, never with the cache off. */
    EXT_RAM_BSS_ATTR static crosspad::control::WaveStatus st;
    EXT_RAM_BSS_ATTR static char out[1024];
    /* A cache lookup per layer: resolving a path stats the card, and this is
     * the prio-1 main loop. */
    crosspad::control::readWaveCoverage(st, crosspad_sampler::waveform_cache_contains);
    crosspad::control::setWaveLoader(st, loader, crosspad_sampler::waveform_loader_is_running());
    hil_text_wave_status(out, sizeof(out), st);
    cdc_reply(out);
}

static void handle_kit_list(void)
{
    auto *mgr = crosspad::getKitManager();
    if (!mgr) {
        cdc_reply("ERR no kit manager\r\n");
        return;
    }
    if (!g_kit_scan_done.load()) {
        /* A bare "ERR …", not a "KITS: ERR …" row: the host's multi-line reader
         * accepts an ERR line *in place of* the first row and finishes there,
         * so a scenario gets an error at once instead of idling out the read
         * waiting for a "KITS: end" that is not coming. */
        cdc_reply("ERR scanning\r\n");
        return;
    }
    /* Multi-line, like APP_VERSIONS: one line per batch of kits, then
     * "KITS: end count=N current=M". A single line has been outgrown twice —
     * 256 bytes silently listed the first 17 of 43, then 1024 listed 77 of 83 —
     * and each time the truncation was invisible, so a kit that was on the card
     * and in memory looked like a kit that had failed to copy. */
    /* In PSRAM: this board boots with a couple of hundred bytes of
     * internal RAM to spare, and a reply buffer is read and written
     * only from the CDC loop, never with the cache off. */
    EXT_RAM_BSS_ATTR static char out[512];
    HilKitListText text{out, sizeof(out), 0, 0, cdc_reply};
    hil_text_kit_list_begin(text);
    int listed = 0;
    crosspad::control::readKitList(*mgr, [&text, &listed](const crosspad::control::KitListRow &row) {
        if (!row.removed) ++listed;
        hil_text_kit_list_add(text, row);
    });
    /* count= is what was listed, so a host can tell a dropped line from a
     * deleted kit (its id stays, blank). */
    hil_text_kit_list_end(text, listed, mgr->getCurrentKitId());
}

static void handle_app_list(void)
{
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    char out[256];
    size_t n = snprintf(out, sizeof(out), "APPS: ");
    bool first = true;
    for (const auto &app : orch.getApps()) {
        if (!app) continue;
        n += snprintf(out + n, sizeof(out) - n, "%s%s",
                      first ? "" : ",", app->getName());
        first = false;
        if (n >= sizeof(out) - 48) break;
    }
    auto *running = orch.getRunningApp();
    snprintf(out + n, sizeof(out) - n, " running=%s\r\n",
             running ? running->getName() : "-");
    cdc_reply(out);
}

/* SHA-256 of the ELF this image was linked from, the way the build stamps it
 * into the app descriptor. The only thing that tells two builds of the same
 * commit apart: APP_VERSIONS/LIBVER report the commit, and the descriptor's
 * own compile time is not regenerated for every build, so both can read
 * identical while the board runs a different binary — which is exactly what
 * a silent rollback inside a trial window looks like. Compare against
 * `sha256sum build_<rev>/CrossPad.elf`. */
static void handle_app_sha(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    char hex[sizeof(desc->app_elf_sha256) * 2 + 1];
    for (size_t i = 0; i < sizeof(desc->app_elf_sha256); i++) {
        snprintf(hex + i * 2, 3, "%02x", desc->app_elf_sha256[i]);
    }
    char reply[96];
    snprintf(reply, sizeof(reply), "APPSHA: %s\r\n", hex);
    cdc_reply(reply);
}

// What went into this binary: which app submodules, at which commit, against
// which pin. Lines up one-to-one with the app manager's `status`.
static void handle_app_versions(void)
{
    char line[160];
    size_t count = crosspad::appVersionCount();
    for (size_t i = 0; i < count; ++i) {
        const crosspad::AppVersion *v = crosspad::appVersionGet(i);
        if (!v) continue;
        snprintf(line, sizeof(line),
                 "APPVER: %s id=%s commit=%s ref=%s dirty=%d\r\n",
                 v->component,
                 v->appId && v->appId[0] ? v->appId : "-",
                 v->commit,
                 v->ref && v->ref[0] ? v->ref : "-",
                 v->dirty ? 1 : 0);
        cdc_reply(line);
    }
    char summary[96];
    snprintf(line, sizeof(line), "LIBVER: %s\r\n", crosspad_deps_summary(summary, sizeof(summary)));
    cdc_reply(line);
    stm32_link_info_t link;
    if (stm32_hal_get_link_info(&link) == ESP_OK && link.linked) {
        snprintf(line, sizeof(line), "BOARD: fw=%s pcb=%u\r\n", BSP_BOARD_REV_NAME, (unsigned)link.pcb_rev);
    } else {
        snprintf(line, sizeof(line), "BOARD: fw=%s pcb=-\r\n", BSP_BOARD_REV_NAME);
    }
    cdc_reply(line);
    snprintf(line, sizeof(line), "APPVER: end count=%u\r\n", (unsigned)count);
    cdc_reply(line);
}

/* Erase the settings namespaces so the next boot takes its defaults.
 *
 * Without this there is no way to check any default on a board that has already
 * saved the key: a default can be read in the source and built into a binary,
 * and still not be *observed* anywhere, because the board already has the key
 * in NVS. A default nobody can observe
 * is a default nobody can trust.
 *
 * CDC only, never wired to the UI, and it wants FORCE spelled out — erasing a
 * board's configuration by a typo is not a thing that should be possible. The
 * reboot is part of the verb: CrosspadSettings::load() already ran, so the
 * living instance still holds the old values until the device comes back. */
static const char *const kSettingsNamespaces[] = {
    "cfg_audio", "cfg_audio_eng", "cfg_display", "cfg_esp_rt", "cfg_imu_midi",
    "cfg_keypad", "cfg_rgb", "cfg_stm_rt", "cfg_system", "cfg_vibrate",
    "cfg_wireless",
};

static void handle_settings_reset(void)
{
    unsigned erased = 0, missing = 0, failed = 0;
    for (const char *ns : kSettingsNamespaces) {
        nvs_handle_t h;
        esp_err_t err = nvs_open(ns, NVS_READWRITE, &h);
        if (err == ESP_ERR_NVS_NOT_FOUND) { ++missing; continue; }
        if (err != ESP_OK) { ++failed; continue; }
        if (nvs_erase_all(h) == ESP_OK && nvs_commit(h) == ESP_OK) ++erased;
        else ++failed;
        nvs_close(h);
    }
    /* Saved WiFi networks too: a board that changes hands must not keep the
     * owner's password. Through wifi_creds, not a raw erase: under its lock
     * and with its RAM copy, so a save in the reply window (a connect's
     * promote) starts from the empty list instead of writing the old one back. */
    if (wifi_creds_clear()) ++erased;
    else ++failed;
    /* Update settings the same way: the RAM copies would write the old values
     * back from a check that ends in the reply window. */
    if (fw_updater_forget()) ++erased;
    else ++failed;
    /* The crosspad.app link: a board that changes hands must not keep the
     * owner's account. RAM now, NVS on its writer task, done well within the
     * reboot's wait below. */
    if (cp_account_forget()) ++erased;
    else ++failed;
    char reply[96];
    snprintf(reply, sizeof(reply), "SETTINGS_RESET: erased=%u absent=%u failed=%u reboot=1\r\n",
             erased, missing, failed);
    cdc_reply(reply);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

/* USB transfer mode: the SD card as a USB drive. Entering and leaving are both
 * reboots (usb_transfer.h), so the reply goes out first and the restart waits
 * for it. Deliberately CDC-only — there is no SysEx or BLE way in, because
 * handing a host the storage must need the user at the device. */
static const char *usb_msc_state_name(usb_transfer_state_t state)
{
    switch (state) {
        case USB_TRANSFER_NO_CARD: return "no_card";
        case USB_TRANSFER_WAITING: return "waiting";
        case USB_TRANSFER_MOUNTED: return "mounted";
        case USB_TRANSFER_BUSY:    return "busy";
        case USB_TRANSFER_EJECTED: return "ejected";
        default:                   return "unknown";
    }
}

static void handle_usb_msc(const char *args)
{
    const bool active = usb_transfer_active();
    int on = -1;
    if (sscanf(args, "%d", &on) != 1) {
        usb_transfer_status_t st;
        usb_transfer_get_status(&st);
        /* Outside transfer mode the card is FATFS's, not the MSC layer's, so
         * its presence is the mount's answer — reporting the raw handle there
         * would say "no card" with one sitting in the slot. */
        const bool card = active ? st.cardPresent : status.sdCardDetected;
        int width = 0, khz = 0;
        bsp_sdcard_get_bus(&width, &khz);
        char reply[144];
        snprintf(reply, sizeof(reply),
                 "USB_MSC: active=%d card=%d state=%s size_mb=%llu read_kb=%u write_kb=%u width=%d freq=%d\r\n",
                 active ? 1 : 0, card ? 1 : 0, usb_msc_state_name(st.state),
                 (unsigned long long)(st.cardBytes >> 20),
                 (unsigned)st.readKiB, (unsigned)st.writeKiB, width, khz);
        cdc_reply(reply);
        return;
    }

    if ((on != 0) == active) {
        cdc_reply("OK unchanged\r\n");
        return;
    }
    cdc_reply("OK reboot=1\r\n");
    if (on) usb_transfer_request();
    else    usb_transfer_leave();
}

/* Network mode (net_mode.h): entering and leaving are both restarts, so the
 * reply goes out first and the restart waits for it. CDC-only like USB_MSC —
 * the companion allow-list has no NET_ entry, so there is no SysEx or BLE way
 * in. From transfer mode it is refused: the host may still be writing. */
static constexpr const char *kNetModeUsage = "ERR NET_MODE 0|1 [TAB=wifi|store|updates]\r\n";

static void handle_net_mode(const char *args)
{
    while (*args == ' ') args++;
    const bool active = net_mode_active();
    if (!*args) {
        char reply[64];
        snprintf(reply, sizeof(reply), "NET_MODE: active=%d tab=%s\r\n", active ? 1 : 0,
                 active ? net_tab_name(net_mode_start_tab()) : "-");
        cdc_reply(reply);
        return;
    }
    int on = -1;
    char tabArg[16] = "";
    if (sscanf(args, "%d %15s", &on, tabArg) < 1 || (on != 0 && on != 1)) {
        cdc_reply(kNetModeUsage);
        return;
    }
    net_tab_t tab = NET_TAB_WIFI;
    if (tabArg[0] && (strncmp(tabArg, "TAB=", 4) != 0 || !net_tab_parse(tabArg + 4, &tab))) {
        cdc_reply(kNetModeUsage);
        return;
    }
    if (usb_transfer_active()) { cdc_reply("ERR transfer_mode\r\n"); return; }
    if ((on == 1) == active) { cdc_reply("OK unchanged\r\n"); return; }
    cdc_reply("OK reboot=1\r\n");
    if (on) {
        net_mode_request(tab);
    } else {
        /* The screen lets go as Exit does, on the LVGL thread; the restart does
         * not wait for it, so a stuck UI cannot keep the board in this mode. */
        lv_async_call_locked([](void *) { net_mode_screen_close(); }, nullptr);
        net_mode_leave();
    }
}

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

/* Raw attitude and rate, because "is the IMU alive" and "is the mapping right"
 * were until now answerable only by the absence of an error line in the console,
 * which is a poor instrument. Values are the ones the mapper is fed. */
static void handle_imu(void)
{
    bsp_imu_data_t d = {};
    char reply[192];
    if (bsp_imu_get_data(&d) != ESP_OK) {
        cdc_reply("IMU: err=1\r\n");
        return;
    }
    snprintf(reply, sizeof(reply),
             "IMU: pitch=%.2f roll=%.2f gyro=%.2f,%.2f,%.2f accel=%.3f,%.3f,%.3f "
             "temp=%.1f midi=%d sent=%u\r\n",
             d.fused_pitch, d.fused_roll, d.gyro_x, d.gyro_y, d.gyro_z,
             d.accel_x, d.accel_y, d.accel_z, d.temp,
             imu_midi_enabled() ? 1 : 0, (unsigned)imu_midi_sent());
    cdc_reply(reply);
    bsp_imu_stats_t st = {};
    bsp_imu_get_stats(&st);
    snprintf(reply, sizeof(reply), "IMU: drdy_line=%d drdy=%u samples=%u read_fail=%u\r\n",
             st.drdy_line ? 1 : 0, (unsigned)st.drdy_pulses, (unsigned)st.samples,
             (unsigned)st.read_failures);
    cdc_reply(reply);
    bsp_imu_event_counts_t ev = {};
    bsp_imu_get_events(&ev);
    snprintf(reply, sizeof(reply),
             "IMU: events single_tap=%u double_tap=%u free_fall=%u wake_up=%u orientation=%u d6d=%u "
             "still=%d still_periods=%u\r\n",
             (unsigned)ev.single_tap, (unsigned)ev.double_tap, (unsigned)ev.free_fall,
             (unsigned)ev.wake_up, (unsigned)ev.orientation, (unsigned)ev.orientation_6d,
             ev.still ? 1 : 0, (unsigned)ev.still_periods);
    cdc_reply(reply);

    snprintf(reply, sizeof(reply), "IMU: tap_gate mutes=%u taps_muted=%u mute_ms_left=%ld\r\n",
             (unsigned)st.tap_mutes, (unsigned)st.taps_muted, (long)st.tap_mute_ms_left);
    cdc_reply(reply);

    auto *cfg = crosspad::CrosspadSettings::getInstance();
    if (!cfg) return;
    for (size_t i = 0; i < crosspad::kImuMidiSlots; ++i) {
        const auto &s = cfg->imuMidi.slots[i];
        snprintf(reply, sizeof(reply),
                 "IMU: slot%u en=%d src=%u tgt=%u cc=%u ch=%u inv=%d span=%d\r\n",
                 (unsigned)i, s.enabled ? 1 : 0, (unsigned)s.source, (unsigned)s.target,
                 (unsigned)s.cc, (unsigned)s.channel, s.invert ? 1 : 0,
                 (int)s.inputSpan);
        cdc_reply(reply);
    }
    /* A trailer, because a reply that grew from one line to five needs a way for
     * the reader to know it has them all — the LIBVER row spent months being
     * dropped for want of exactly this. */
    snprintf(reply, sizeof(reply), "IMU: end slots=%u\r\n",
             (unsigned)crosspad::kImuMidiSlots);
    cdc_reply(reply);
}

/* ── Touch trace ───────────────────────────────────────────────────── */

/* The coordinate stream as the panel reports it, not as LVGL interprets it.
 * Recording is off at boot: the ring is 32 kB of PSRAM and the dump is up to
 * 4096 lines, neither of which any other scenario wants to pay for. */
/* The heap watch: a walk of every heap on the worker, until it finds a broken
 * block once. It is deliberately one-shot -- the first failure is the one with
 * information in it, and a corrupt heap tends to fail every walk after. */
static bool s_heap_watch = false;
static bsp_worker_job_t *s_heap_watch_job = nullptr;

static uint32_t heap_watch_job(void *)
{
    if (!s_heap_watch) return 0;
    if (!heap_caps_check_integrity_all(true)) {
        ESP_LOGE("HEAPCHK", "heap corrupt -- first seen now, so look at what just happened");
        s_heap_watch = false;
    }
    return 0;
}

static bool heap_watch_on(void) { return s_heap_watch; }

static void heap_watch_set(bool on)
{
    s_heap_watch = on;
    if (on && !s_heap_watch_job) {
        /* Two seconds: the walk is milliseconds of holding each heap's lock,
         * and anything faster is a worker overrun for no more information. */
        s_heap_watch_job = bsp_worker_add("heap_chk", 2000, heap_watch_job, nullptr);
    }
}

/* HEAP_TRACE START [records] | STOP | DUMP [min_bytes] [ALL]
 * Standalone heap tracing (CONFIG_HEAP_TRACING_STANDALONE) in LEAKS mode:
 * what was allocated between START and STOP and is still live. DUMP keeps
 * internal-RAM records unless ALL is given, groups them by call stack and prints
 * the groups largest first; the host resolves the PCs with addr2line on the
 * matching ELF. One reply shape for all three: rows only for DUMP, always the
 * trailer. The record buffer lives in PSRAM so the trace itself does not eat
 * what it is measuring. */
static heap_trace_record_t *s_heap_trace_buf = nullptr;
static size_t s_heap_trace_n = 0;
static bool s_heap_trace_on = false;

static void handle_heap_trace(const char *arg)
{
    char reply[160];
    bool dump = false;
    bool with_psram = false;
    size_t min_bytes = 0;
    if (strncmp(arg, "START", 5) == 0) {
        size_t want = (size_t)atoi(arg + 5);
        if (want == 0) want = 768;
        if (s_heap_trace_on) heap_trace_stop();
        if (s_heap_trace_buf && s_heap_trace_n != want) {
            heap_trace_init_standalone(nullptr, 0);
            heap_caps_free(s_heap_trace_buf);
            s_heap_trace_buf = nullptr;
        }
        if (!s_heap_trace_buf) {
            s_heap_trace_buf = (heap_trace_record_t *)heap_caps_calloc(
                want, sizeof(heap_trace_record_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!s_heap_trace_buf) { cdc_reply("ERR HEAP_TRACE no_mem\r\n"); return; }
            s_heap_trace_n = want;
            heap_trace_init_standalone(s_heap_trace_buf, s_heap_trace_n);
        }
        const esp_err_t e = heap_trace_start(HEAP_TRACE_LEAKS);
        if (e != ESP_OK) {
            snprintf(reply, sizeof(reply), "ERR HEAP_TRACE start %s\r\n", esp_err_to_name(e));
            cdc_reply(reply);
            return;
        }
        s_heap_trace_on = true;
    } else if (strncmp(arg, "STOP", 4) == 0) {
        if (s_heap_trace_on) heap_trace_stop();
        s_heap_trace_on = false;
    } else if (strncmp(arg, "DUMP", 4) == 0) {
        dump = true;
        min_bytes = (size_t)atoi(arg + 4);
        with_psram = strstr(arg, "ALL") != nullptr;
    } else {
        cdc_reply("ERR HEAP_TRACE START|STOP|DUMP\r\n");
        return;
    }

    const size_t count = s_heap_trace_buf ? heap_trace_get_count() : 0;
    unsigned rows = 0;
    if (dump && count) {
        struct Group { void *pc[CONFIG_HEAP_TRACING_STACK_DEPTH]; uint32_t bytes; uint32_t n; };
        constexpr size_t kGroups = 512;
        Group *g = (Group *)heap_caps_calloc(kGroups, sizeof(Group), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!g) { cdc_reply("ERR HEAP_TRACE no_mem\r\n"); return; }
        size_t used = 0;
        uint32_t other_bytes = 0, other_n = 0;
        for (size_t i = 0; i < count; i++) {
            heap_trace_record_t r;
            if (heap_trace_get(i, &r) != ESP_OK || !r.address) continue;
            if (!with_psram && esp_ptr_external_ram(r.address)) continue;
            size_t k = 0;
            for (; k < used; k++) {
                if (memcmp(g[k].pc, r.alloced_by, sizeof(g[k].pc)) == 0) break;
            }
            if (k == used) {
                if (used == kGroups) { other_bytes += r.size; other_n++; continue; }
                memcpy(g[used].pc, r.alloced_by, sizeof(g[used].pc));
                used++;
            }
            g[k].bytes += r.size;
            g[k].n++;
        }
        /* Largest first: a selection sort over at most 512 entries. */
        for (size_t a = 0; a < used; a++) {
            size_t best = a;
            for (size_t b = a + 1; b < used; b++) if (g[b].bytes > g[best].bytes) best = b;
            if (best != a) { Group t = g[a]; g[a] = g[best]; g[best] = t; }
        }
        for (size_t a = 0; a < used; a++) {
            if (g[a].bytes < min_bytes) break;
            int len = snprintf(reply, sizeof(reply), "HEAP_TRACE: bytes=%lu n=%lu pc=",
                               (unsigned long)g[a].bytes, (unsigned long)g[a].n);
            for (size_t d = 0; d < CONFIG_HEAP_TRACING_STACK_DEPTH && len < (int)sizeof(reply) - 16; d++) {
                len += snprintf(reply + len, sizeof(reply) - len, "%s%p", d ? "," : "", g[a].pc[d]);
            }
            snprintf(reply + len, sizeof(reply) - len, "\r\n");
            cdc_reply(reply);
            rows++;
        }
        if (other_n) {
            snprintf(reply, sizeof(reply), "HEAP_TRACE: bytes=%lu n=%lu pc=other\r\n",
                     (unsigned long)other_bytes, (unsigned long)other_n);
            cdc_reply(reply);
            rows++;
        }
        heap_caps_free(g);
    }
    snprintf(reply, sizeof(reply), "HEAP_TRACE: end n=%u records=%u on=%d\r\n",
             rows, (unsigned)count, s_heap_trace_on ? 1 : 0);
    cdc_reply(reply);
}

static void handle_touch_trace(const char *arg)
{
    char reply[96];
    /* Every subcommand answers in the same shape — sample rows (DUMP only)
     * followed by the trailer. A verb that replied "OK" to one argument and a
     * multi-line block to another needed the reader to know which before it
     * could read either. */
    bool dump = false;
    if (strncmp(arg, "ON", 2) == 0 || strncmp(arg, "OFF", 3) == 0) {
        if (!touch_trace_set(arg[1] == 'N')) { cdc_reply("ERR TOUCH_TRACE no_mem\r\n"); return; }
    } else if (strncmp(arg, "CLEAR", 5) == 0) {
        touch_trace_clear();
    } else if (strncmp(arg, "DUMP", 4) == 0) {
        dump = true;
    } else {
        cdc_reply("ERR TOUCH_TRACE ON|OFF|CLEAR|DUMP\r\n");
        return;
    }

    const unsigned n = touch_trace_count();
    for (unsigned i = 0; dump && i < n; i++) {
        unsigned t = 0, pressed = 0;
        int x = 0, y = 0;
        if (!touch_trace_get(i, &t, &x, &y, &pressed)) break;
        snprintf(reply, sizeof(reply), "TOUCH_TRACE: %u %u %d %d\r\n", t, pressed, x, y);
        cdc_reply(reply);
    }
    snprintf(reply, sizeof(reply),
             "TOUCH_TRACE: end n=%u on=%d swipes=%u last=%s fwd=%s\r\n",
             n, touch_trace_is_on() ? 1 : 0,
             (unsigned)crosspad_gui::touch_gesture_count(),
             crosspad_gui::swipeDirName(crosspad_gui::touch_gesture_last()),
             crosspad_gui::touch_gesture_forward_result());
    cdc_reply(reply);
}

/* ── Encoder navigation introspection ──────────────────────────────── */

// Who owns encoder navigation. An app that made its own lv_group must hand
// the encoder back on teardown, or the launcher is unnavigable until reboot —
// and nothing else can observe that from outside.
extern lv_group_t *app_group;          // launcher group (gui.cpp)
extern lv_indev_t *indev_encoderTop;   // navigation encoder (display.h)

/* A finger the bench does not have: TOUCH_DRAG x1 y1 x2 y2 ms [rest_ms]
 * [hold_ms] presses, holds (a long press, then a drag), moves, rests and lets
 * go through the panel's own read path, so the
 * swipe recogniser, LVGL's scrolling and the trace all see an ordinary contact.
 * TOUCH_TAP x y [hold_ms] is a drag that does not move. The reply comes at
 * once; the contact takes ms + rest_ms after it. */
static void handle_touch_inject(const char *arg, bool tap)
{
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0, ms = 0, rest = 0, hold = 0;
    int n;
    if (tap) {
        n = sscanf(arg, "%d %d %d", &x1, &y1, &ms);
        if (n < 2) { cdc_reply("ERR TOUCH_TAP x y [hold_ms]\r\n"); return; }
        if (n < 3) ms = 80;
        x2 = x1; y2 = y1;
    } else {
        n = sscanf(arg, "%d %d %d %d %d %d %d", &x1, &y1, &x2, &y2, &ms, &rest, &hold);
        if (n < 5) { cdc_reply("ERR TOUCH_DRAG x1 y1 x2 y2 ms [rest_ms] [hold_ms]\r\n"); return; }
    }
    if (ms < 1 || ms > 5000 || rest < 0 || rest > 5000 || hold < 0 || hold > 5000) {
        cdc_reply("ERR TOUCH range ms 1..5000 rest 0..5000 hold 0..5000\r\n");
        return;
    }
    if (!touch_trace_inject(x1, y1, x2, y2, (unsigned)ms, (unsigned)rest, (unsigned)hold)) {
        cdc_reply(touch_trace_injecting() ? "ERR TOUCH busy\r\n" : "ERR TOUCH no_panel\r\n");
        return;
    }
    cdc_reply("OK\r\n");
}

/* UI_TREE -- every visible widget of the screen and the top layer
 * (crosspad-gui ui_tree_probe), one "UITREE:" row each, pre-order: index,
 * parent, class, LCD rectangle, interactive, hit (a press on its middle
 * reaches it, not something drawn over it), checked/disabled/focused, then the
 * text (its own label, else the first one under it) to the end of the line.
 * Then "UITREE: end n=.. running=..". What a test needs to find a control by
 * what it says and tap it where it is (TOUCH_TAP). Copied under the LVGL lock,
 * written after it: the CDC writes must not hold the UI. */
static void handle_ui_tree(void)
{
    constexpr uint16_t kMax = 512;
    constexpr size_t kText = 96;
    struct Row { crosspad_gui::UiNode n; char cls[16]; char text[kText]; bool hit; };
    /* PSRAM for the call only: a screen with an app and a dialog over it is
     * a few hundred widgets, which is no reason to hold 60 kB for good. */
    auto *nodes = static_cast<crosspad_gui::UiNode *>(
        heap_caps_malloc(sizeof(crosspad_gui::UiNode) * kMax, MALLOC_CAP_SPIRAM));
    auto *rows = static_cast<Row *>(heap_caps_malloc(sizeof(Row) * kMax, MALLOC_CAP_SPIRAM));
    if (!nodes || !rows) {
        heap_caps_free(nodes);
        heap_caps_free(rows);
        cdc_reply("ERR UI_TREE no_mem\r\n");
        return;
    }
    EXT_RAM_BSS_ATTR static char running[32];
    if (!lvgl_port_lock(1000)) {
        heap_caps_free(nodes);
        heap_caps_free(rows);
        cdc_reply("ERR UI_TREE lvgl_busy\r\n");
        return;
    }
    const uint16_t count = crosspad_gui::ui_tree_probe(nodes, kMax);
    for (uint16_t i = 0; i < count; ++i) {
        Row &r = rows[i];
        r.n = nodes[i];
        snprintf(r.cls, sizeof(r.cls), "%s", nodes[i].cls ? nodes[i].cls : "obj");
        snprintf(r.text, sizeof(r.text), "%s", nodes[i].text ? nodes[i].text : "");
        for (char *c = r.text; *c; ++c) if (*c == '\r' || *c == '\n' || *c == '\t') *c = ' ';
        /* What a finger on its middle would land on: the probe lists what is
         * drawn, also under an app's screen or a dialog; only a control the
         * press reaches (it or something inside it) is one a person can use. */
        r.hit = false;
        if (r.n.interactive) {
            lv_point_t pt = { (int32_t)(r.n.x + r.n.w / 2), (int32_t)(r.n.y + r.n.h / 2) };
            lv_obj_t *on = lv_indev_search_obj(lv_layer_top(), &pt);
            if (!on) on = lv_indev_search_obj(lv_screen_active(), &pt);
            /* It, or something inside it: an ancestor of what is hit with the
             * node's own rectangle. Inside its rectangle is not enough -- a
             * pad of the app over the map lies inside the map's button. */
            for (lv_obj_t *o = on; o && !r.hit; o = lv_obj_get_parent(o)) {
                lv_area_t a;
                lv_obj_get_coords(o, &a);
                r.hit = a.x1 == r.n.x && a.y1 == r.n.y &&
                        lv_area_get_width(&a) == r.n.w && lv_area_get_height(&a) == r.n.h;
            }
        }
    }
    auto *app = crosspad_gui::AppOrchestrator::getInstance().getRunningApp();
    snprintf(running, sizeof(running), "%s", app && app->getName() ? app->getName() : "-");
    lvgl_port_unlock();
    heap_caps_free(nodes);

    EXT_RAM_BSS_ATTR static char line[208];
    for (uint16_t i = 0; i < count; ++i) {
        const Row &r = rows[i];
        snprintf(line, sizeof(line),
                 "UITREE: i=%u p=%d cls=%s x=%ld y=%ld w=%ld h=%ld int=%d hit=%d chk=%d dis=%d foc=%d text=%s\r\n",
                 (unsigned)i, (int)r.n.parent, r.cls, (long)r.n.x, (long)r.n.y, (long)r.n.w, (long)r.n.h,
                 r.n.interactive ? 1 : 0, r.hit ? 1 : 0, r.n.checked ? 1 : 0, r.n.disabled ? 1 : 0,
                 r.n.focused ? 1 : 0, r.text);
        cdc_reply(line);
    }
    heap_caps_free(rows);
    snprintf(line, sizeof(line), "UITREE: end n=%u max=%u running=%s\r\n", (unsigned)count, (unsigned)kMax, running);
    cdc_reply(line);
}

/* The top row and what the swipes did: which bar is the page, where the row
 * rests (0 transport, 320 status), the drawer, where the encoder is, the last
 * screen-wide swipe, the MODE map and who owns the pads. */
static void handle_topbar()
{
    char reply[200];
    if (!lvgl_port_lock(1000)) { cdc_reply("ERR TOPBAR lvgl_busy\r\n"); return; }
    lv_group_t *g = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
    lv_obj_t *focused = g ? lv_group_get_focused(g) : nullptr;
    const char *focus = !focused ? "none"
                      : focused == crosspad_gui::statusbar_get_arrow() ? "arrow"
                      : crosspad_gui::transportbar_has_focus() ? "transport"
                      : "screen";
    snprintf(reply, sizeof(reply),
             "TOPBAR: transport=%d scroll=%ld drawer=%d focus=%s swipe=%s swipes=%u map=%d pads=%s\r\n",
             crosspad_gui::transportbar_shown() ? 1 : 0,
             (long)crosspad_gui::transportbar_row_scroll(),
             crosspad_gui::statusbar_drawer_is_open() ? 1 : 0,
             focus,
             crosspad_gui::swipeDirName(crosspad_gui::touch_gesture_last()),
             (unsigned)crosspad_gui::touch_gesture_count(),
             crosspad_gui::mode_overlay_is_open() ? 1 : 0,
             crosspad::getPadManager().getActivePadLogic().c_str());
    lvgl_port_unlock();
    cdc_reply(reply);
}

static void handle_enc_state(void)
{
    lv_group_t *cur = nullptr;
    if (lvgl_port_lock(1000)) {
        cur = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
        lvgl_port_unlock();
    }
    char reply[128];
    snprintf(reply, sizeof(reply),
             "ENC: group=%p launcher=%p owner=%s\r\n",
             (void *)cur, (void *)app_group,
             cur == nullptr      ? "none"
             : cur == app_group  ? "launcher"
                                 : "app");
    cdc_reply(reply);
}

// Encoder stimulation — the only way to reach an in-app Back button on a unit
// without touch.
static int s_enc_delta = 0;

static uint32_t s_enc_press_ms = 80;

static void async_enc_rotate(void *)
{
    // Through the encoder read callback, the physical encoder's own path.
    // lv_group_send_data() only sends LV_EVENT_KEY: focus never moves, and
    // widgets that gate on lv_indev_active() see no encoder at all.
    encoderTop_hil_diff().fetch_add(s_enc_delta);
}

static void async_enc_press(void *)
{
    // Hold for at least one indev read period, so LVGL forms the click (or,
    // held longer, the long press) itself.
    encoderTop_hil_press_until().store(lv_tick_get() + s_enc_press_ms);
}

extern std::atomic<uint8_t> g_hil_last_pwr_state;
extern std::atomic<uint32_t> g_hil_pwr_state_count;

// Why the pads are dark: painted black, brightness zero, an animation holding
// the strip, or no power-state callback ever arrived to repaint.
static void handle_led_state(void)
{
    uint32_t refreshes = 0;
    const uint32_t txfail = esp_led_strip_refresh_failures(&refreshes);
    const HilLedBoard board{g_hil_last_pwr_state.load(), g_hil_pwr_state_count.load(), txfail, refreshes};
    /* Static: 512 B on the drain loop's 4 kB stack, under snprintf, overflowed
     * it at the mirror window's four asks a second (task=main cause=65). */
    EXT_RAM_BSS_ATTR static char out[512];
    hil_text_led_state(out, sizeof(out), crosspad::control::readLedState(crosspad::getPadLedController()), board);
    cdc_reply(out);
}

static void handle_pad_stats(void)
{
    /* Every stage a pad hit crosses here, so a missing burst can be placed:
     * STM link → parsed presses → sampler → USB MIDI. */
    char reply[200];
    snprintf(reply, sizeof(reply),
             "PADSTATS: press=%u release=%u inj_drop=%u played=%u last=%u:%u freeslots=%d stm=%u at=%u rxmsg=%u "
             "uart_ovf=%u sbuf_drop=%u usb_tx=%u usb_drop=%u\r\n",
             (unsigned)g_hil_pad_press_count.load(),
             (unsigned)g_hil_pad_release_count.load(),
             (unsigned)g_hil_pad_inject_drop.load(),
             (unsigned)g_hil_note_on_count.load(),
             (unsigned)(g_hil_last_note_on.load() >> 8), (unsigned)(g_hil_last_note_on.load() & 0x7F),
             hil_free_slots(),
             (unsigned)g_hil_stm_press_count.load(),
             (unsigned)g_hil_stm_at_count.load(),
             (unsigned)stm32_get_rx_msg_count(false),
             (unsigned)g_stm_uart_ovf.load(),
             (unsigned)g_stm_sbuf_drop.load(),
             (unsigned)g_usb_midi_tx_ok.load(),
             (unsigned)g_usb_midi_tx_fail.load());
    cdc_reply(reply);
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

/* Whole-group readback: what is on this screen at all. Rotating blindly and
 * counting pointers cannot tell a row that was never built from one the
 * encoder has not reached. Captions live in different places per widget, so
 * the lookup walks the subtree a few levels deep, then the parent. */
static const char *first_label_in(lv_obj_t *scope, int depth)
{
    if (!scope) return nullptr;
    if (lv_obj_check_type(scope, &lv_label_class)) return lv_label_get_text(scope);
    if (depth == 0) return nullptr;
    uint32_t cnt = lv_obj_get_child_count(scope);
    for (uint32_t i = 0; i < cnt; ++i) {
        const char *t = first_label_in(lv_obj_get_child(scope, i), depth - 1);
        if (t) return t;
    }
    return nullptr;
}

static const char *widget_label(lv_obj_t *obj)
{
    if (!obj) return "";
    const char *t = first_label_in(obj, 3);
    if (!t) t = first_label_in(lv_obj_get_parent(obj), 1);
    return t ? t : "";
}

static void handle_enc_group(void)
{
    if (!lvgl_port_lock(1000)) {
        cdc_reply("ENCGROUP: ERR lock\r\n");
        return;
    }
    lv_group_t *g = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
    uint32_t n = g ? lv_group_get_obj_count(g) : 0;
    char head[64];
    snprintf(head, sizeof(head), "ENCGROUP: count=%u\r\n", (unsigned)n);
    cdc_reply(head);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *o = lv_group_get_obj_by_index(g, i);
        /* Coordinates too: a text list says what is on the screen but not
         * where, and "where" is the only way to check a layout from off the
         * device -- two widgets that overlap look fine in a list of names. */
        lv_area_t a = {0, 0, -1, -1};
        if (o) lv_obj_get_coords(o, &a);
        char line[200];
        snprintf(line, sizeof(line), "  [%u] %p x=%d y=%d w=%d h=%d %s\r\n",
                 (unsigned)i, (void *)o, (int)a.x1, (int)a.y1,
                 (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1), widget_label(o));
        cdc_reply(line);
    }
    lvgl_port_unlock();
}

/* Focused-widget readback: the object plus the first label text on it, or
 * driving the UI over ENC_ROTATE is blind. */
static void handle_enc_focus(void)
{
    char reply[192];
    if (lvgl_port_lock(1000)) {
        lv_group_t *g = indev_encoderTop ? lv_indev_get_group(indev_encoderTop) : nullptr;
        lv_obj_t *focused = g ? lv_group_get_focused(g) : nullptr;
        const char *text = widget_label(focused);
        snprintf(reply, sizeof(reply), "ENCFOCUS: obj=%p idx=%d edit=%d text=%s\r\n",
                 (void *)focused,
                 focused ? (int)lv_obj_get_index(focused) : -1,
                 g && lv_group_get_editing(g) ? 1 : 0,
                 text ? text : "");
        lvgl_port_unlock();
    } else {
        snprintf(reply, sizeof(reply), "ENCFOCUS: ERR lock\r\n");
    }
    cdc_reply(reply);
}

/* UI-affecting settings, invisible from outside otherwise — "the drawer arrow
 * is gone" is as often a setting as a bug. */
static void handle_ui_state(void)
{
    auto *cfg = crosspad::CrosspadSettings::getInstance();
    auto &orch = crosspad_gui::AppOrchestrator::getInstance();
    auto *running = orch.getRunningApp();
    char reply[192];
    snprintf(reply, sizeof(reply),
             "UI: display=%s touch=%s drawer=%d lcd=%d rgb=%d theme=%u launcher=%u bt_icon=%s app=%s\r\n",
             /* Headless: every UI command here is a no-op. Say so, rather
              * than leaving it to be inferred from an empty ENC_GROUP. */
             lv_display_get_default() ? "yes" : "none",
             /* Separate from display=: the LCD is write-only, so touch is the
              * only part of that flex that answers. "no" means the FT6x36 did
              * not ACK; the screen may still be fine. */
             bsp_display_get_input_dev() ? "yes" : "no",
             cfg && cfg->quickSettingsEnabled ? 1 : 0,
             cfg ? (int)cfg->LCDbrightness : -1,
             cfg ? (int)cfg->RGBbrightness : -1,
             cfg ? (unsigned)cfg->themeColorIndex : 0u,
             /* the launcher's look (LauncherStyle) — a mirror (the browser
              * twin) draws the same main screen */
             cfg ? (unsigned)cfg->launcherStyle : 0u,
             /* what the status-bar glyph should look like right now */
             status.bluetoothConnected ? "solid"
             : status.bluetoothActive  ? "dim"
                                       : "hidden",
             running ? running->getName() : "-");
    cdc_reply(reply);
}

/* ── Memory ────────────────────────────────────────────────────────── */

/* The note each pad answers to. A kit redefines them, so a DAW or BLE host
 * has to ask rather than assume the 36+pad default. */
static void handle_pad_notes(void)
{
    auto &pads = crosspad::getPadManager();
    char reply[160];
    int n = snprintf(reply, sizeof(reply), "PADNOTES:");
    for (uint8_t i = 0; i < 16; ++i) {
        n += snprintf(reply + n, sizeof(reply) - n, " %u:%u",
                      (unsigned)i, (unsigned)pads.getMidiNoteForPad(i));
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);
}

/* int_* counts MALLOC_CAP_INTERNAL, which includes ~8 kB of RTC fast memory
 * no DMA can reach; dma_* is the internal RAM a DMA buffer can come from --
 * the SD stage, a fopen's sector buffer, BLE's controller -- and the unit the
 * 63 kB floor is about. */
static void handle_mem(void)
{
    multi_heap_info_t iram, psram, dma;
    heap_caps_get_info(&iram, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&psram, MALLOC_CAP_SPIRAM);
    heap_caps_get_info(&dma, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    char reply[220];
    snprintf(reply, sizeof(reply),
             "MEM: int_free=%u int_largest=%u int_min=%u int_blocks=%u "
             "psram_free=%u psram_largest=%u psram_blocks=%u dma_free=%u dma_largest=%u dma_min=%u\r\n",
             (unsigned)iram.total_free_bytes, (unsigned)iram.largest_free_block,
             (unsigned)iram.minimum_free_bytes, (unsigned)iram.allocated_blocks,
             (unsigned)psram.total_free_bytes, (unsigned)psram.largest_free_block,
             (unsigned)psram.allocated_blocks, (unsigned)dma.total_free_bytes,
             (unsigned)dma.largest_free_block, (unsigned)dma.minimum_free_bytes);
    cdc_reply(reply);
}

/* Internal-heap size histogram: "18 kB free, largest block 4 kB" is a
 * fragmentation problem, not a shortage, and only this tells them apart. */

struct MemHist {
    static constexpr int kBuckets = 8;   /* <=64,128,256,512,1k,2k,8k,rest */
    uint32_t usedCount[kBuckets] = {};
    uint32_t usedBytes[kBuckets] = {};
    uint32_t freeCount[kBuckets] = {};
    uint32_t freeBytes[kBuckets] = {};
    uint32_t biggestUsed = 0;
    /* The blocks that decide whether a task can still be created. */
    static constexpr int kBigMax = 16;
    uint32_t  bigSize[kBigMax] = {};
    uintptr_t bigAddr[kBigMax] = {};
    int       bigCount = 0;
    bool      psram = false;   /* which memory this histogram is of */
};

static int mem_bucket(size_t size)
{
    if (size <= 64)   return 0;
    if (size <= 128)  return 1;
    if (size <= 256)  return 2;
    if (size <= 512)  return 3;
    if (size <= 1024) return 4;
    if (size <= 2048) return 5;
    if (size <= 8192) return 6;
    return 7;
}

static bool mem_walker(walker_heap_into_t heap, walker_block_info_t block, void *ctx)
{
    /* One memory at a time: PSRAM is mapped at 0x3C000000-0x3DFFFFFF on the
     * S3, internal DRAM sits at 0x3FC88000 and up. */
    auto *h = static_cast<MemHist *>(ctx);
    if (((intptr_t)block.ptr < 0x3F000000) != h->psram) return true;
    int b = mem_bucket(block.size);
    if (block.used) {
        h->usedCount[b]++;
        h->usedBytes[b] += block.size;
        if (block.size > h->biggestUsed) h->biggestUsed = block.size;
        if (block.size >= 8192 && h->bigCount < MemHist::kBigMax) {
            h->bigSize[h->bigCount] = block.size;
            h->bigAddr[h->bigCount] = (uintptr_t)block.ptr;
            h->bigCount++;
        }
    } else {
        h->freeCount[b]++;
        h->freeBytes[b] += block.size;
    }
    (void)heap;
    return true;
}

extern uint32_t g_padanim_worst[5];   /* main.cpp */

/* One row per registered worker job; then pad_anim's slowest run since the
 * previous WORKER, split (main.cpp g_padanim_worst; no name=, so a job-table
 * parser skips it); then an end line with the count. */
static void handle_worker(void)
{
    bsp_worker_stat_t st[BSP_WORKER_MAX_JOBS];
    const size_t n = bsp_worker_stats(st, BSP_WORKER_MAX_JOBS);
    char line[160];
    for (size_t i = 0; i < n; i++) {
        snprintf(line, sizeof(line),
                 "WORKER: name=%s period=%lu runs=%lu last_us=%lu max_us=%lu overruns=%lu\r\n",
                 st[i].name, (unsigned long)st[i].period_ms, (unsigned long)st[i].runs,
                 (unsigned long)st[i].last_us, (unsigned long)st[i].max_us,
                 (unsigned long)st[i].overruns);
        cdc_reply(line);
    }
    const uint32_t *w = g_padanim_worst;
    snprintf(line, sizeof(line),
             "WORKER: split job=pad_anim worst_us=%lu frame_us=%lu report_us=%lu stm_us=%lu at_ms=%lu\r\n",
             (unsigned long)w[0], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
             (unsigned long)w[4]);
    g_padanim_worst[0] = 0;
    cdc_reply(line);
    snprintf(line, sizeof(line), "WORKER: end n=%u\r\n", (unsigned)n);
    cdc_reply(line);
}

/* MEM_BLOCKS [PSRAM] -- internal RAM by default; PSRAM lists the big
 * blocks that are eating the UI's budget, the engine arena first. */
static void handle_mem_blocks(bool psram)
{
    static const char *kNames[MemHist::kBuckets] = {
        "64", "128", "256", "512", "1k", "2k", "8k", "big"
    };
    MemHist h;
    h.psram = psram;
    heap_caps_walk_all(mem_walker, &h);
    /* Static: 900 B is too much for the main task's stack under the heap walk. */
    EXT_RAM_BSS_ATTR static char reply[900];
    int n = snprintf(reply, sizeof(reply), "MEMBLK: biggest_used=%u",
                     (unsigned)h.biggestUsed);
    for (int i = 0; i < MemHist::kBuckets; ++i) {
        n += snprintf(reply + n, sizeof(reply) - n, " | <=%s u=%u/%uB f=%u/%uB",
                      kNames[i], (unsigned)h.usedCount[i], (unsigned)h.usedBytes[i],
                      (unsigned)h.freeCount[i], (unsigned)h.freeBytes[i]);
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);

    /* Second line: ~512 B per CDC write, and the block list would be cut. */
    n = snprintf(reply, sizeof(reply), "MEMBIG:");
    for (int i = 0; i < h.bigCount && n < (int)sizeof(reply) - 40; ++i) {
        n += snprintf(reply + n, sizeof(reply) - n, " @%08x=%u",
                      (unsigned)h.bigAddr[i], (unsigned)h.bigSize[i]);
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);
}

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

/* ── PITCHED_BENCH ─────────────────────────────────────────────────── */
/* The measurement behind the design's rejection of a per-voice phase vocoder:
 * what an FFT and an IFFT actually cost on this silicon against the 1.45 ms
 * block, beside what a Hermite voice costs. It also checks the FFT core
 * settled on (esp-dsp when its twiddle table could be built) against
 * crosspad-core's PortableFft on the same data.
 *
 * It runs on its own task, not the CDC drain loop: that stack has about a kB
 * of headroom and the one-shot table init's first ESP_LOGI trips the stack
 * canary on it. The reply is therefore asynchronous — the verb returns at once
 * and the rows arrive as they are measured. All temporaries come from PSRAM:
 * the FFT buffers because the aes3 kernel wants 16-byte alignment and nobody's
 * stack is 96 kB, the voice zone because it is a second of audio. */

/* One render block, in frames — the RT budget everything here is compared to. */
static constexpr uint32_t kBenchFrames = 64;
/* Voices are re-armed every this many blocks: at ~1.5x a 1 s zone lasts about
 * 460 blocks, so the voices must be restarted before they run out. */
static constexpr int kBenchRearmBlocks = 128;
/* Iteration bounds. The upper one is what keeps a run inside a sane host
 * timeout, given the tick this task yields per iteration. */
static constexpr int kBenchMinIters = 1;
static constexpr int kBenchMaxIters = 500;
static constexpr int kBenchDefaultIters = 20;
/* Synthetic zone: one second of C4 at root 60, played from root+7 so the read
 * increment is not 1.0. */
static constexpr double kBenchZoneHz = 261.63;
static constexpr float  kBenchZoneRoot = 60.0f;
static constexpr double kBenchZoneAmp = 12000.0;
static constexpr uint8_t kBenchFirstNote = 67;

static void *bench_alloc(size_t bytes)
{
    return heap_caps_aligned_alloc(16, (bytes + 15u) & ~(size_t)15, MALLOC_CAP_SPIRAM);
}

static void bench_free(void *p) { heap_caps_free(p); }

/* FftMemory's allocator takes the alignment esp-dsp's aes3 kernel wants; the
 * bench already allocates at 16, which is that alignment. */
static void *bench_fft_alloc(size_t bytes, size_t align)
{
    if (align < 16) align = 16;
    return heap_caps_aligned_alloc(align, (bytes + align - 1u) & ~(align - 1u), MALLOC_CAP_SPIRAM);
}

static void bench_row(const char *name, double us)
{
    char line[64];
    snprintf(line, sizeof(line), "BENCH: %s=%.2f\r\n", name, us);
    cdc_reply(line);
}

static void handle_pitched_bench(int iters)
{
    constexpr uint32_t kMaxN = 4096;                 /* largest size benched */
    const size_t floats = (size_t)kMaxN * 2;         /* interleaved [re, im] */

    float *ref  = (float *)bench_alloc(floats * sizeof(float));
    float *work = (float *)bench_alloc(floats * sizeof(float));
    float *cmp  = (float *)bench_alloc(floats * sizeof(float));
    if (!ref || !work || !cmp) {
        bench_free(ref); bench_free(work); bench_free(cmp);
        cdc_reply("BENCH: ERR no PSRAM\r\n");
        return;
    }

    /* Random input scaled by 1/sqrt(N): the spectrum then sits at unit scale,
     * so fft_maxdiff is an absolute number that means what it looks like. */
    const float amp = 1.0f / 64.0f;
    unsigned seed = 12345u;
    for (size_t i = 0; i < floats; ++i) {
        seed = seed * 1103515245u + 12345u;
        ref[i] = amp * ((float)((seed >> 16) & 0x7FFF) / 16383.5f - 1.0f);
    }

    /* The one FFT this process uses -- created by the kit port on the first
     * pitched load, so on a board that has not loaded one yet this call is what
     * builds it. Its table memory is the verb's own aligned PSRAM; core keeps
     * whichever backend it settles on for the life of the process. */
    crosspad::IFft *cur = crosspad::currentFft();
    if (!cur) {
        crosspad::FftMemory mem;
        mem.alloc = bench_fft_alloc;
        mem.free  = bench_free;
        mem.log   = nullptr;
        cur = &crosspad::createFft(mem);
    }
    crosspad::IFft      &esp = *cur;
    crosspad::PortableFft port;

    auto bench = [&](crosspad::IFft &f, uint32_t n, bool inverse) -> double {
        int64_t total = 0;
        for (int i = 0; i < iters; ++i) {
            memcpy(work, ref, (size_t)n * 2 * sizeof(float));
            const int64_t t0 = esp_timer_get_time();
            if (inverse) f.inverse(work, n); else f.forward(work, n);
            total += esp_timer_get_time() - t0;
            /* Outside the timed window, so it costs the measurement nothing.
             * This task sits one priority above idle on core 0: an unbroken run
             * starves the idle task the WDT feeds, and PITCHED_BENCH 500 is far
             * past 5 s of back-to-back transforms. The tick per iteration is
             * why a large iters needs a matching host timeout. */
            vTaskDelay(1);
        }
        return (double)total / iters;
    };

    bench_row("fft1024_fwd_us", bench(esp, 1024, false));
    bench_row("fft1024_inv_us", bench(esp, 1024, true));
    bench_row("fft2048_fwd_us", bench(esp, 2048, false));
    bench_row("fft4096_fwd_us", bench(esp, 4096, false));
    bench_row("fft4096_inv_us", bench(esp, 4096, true));
    bench_row("portable_fft4096_fwd_us", bench(port, 4096, false));

    memcpy(work, ref, floats * sizeof(float));
    memcpy(cmp,  ref, floats * sizeof(float));
    double maxdiff = 0.0;
    if (esp.forward(work, kMaxN) && port.forward(cmp, kMaxN)) {
        for (size_t i = 0; i < floats; ++i) {
            const double d = fabs((double)work[i] - (double)cmp[i]);
            if (d > maxdiff) maxdiff = d;
        }
    } else {
        maxdiff = -1.0;
    }
    char line[64];
    snprintf(line, sizeof(line), "BENCH: fft_maxdiff=%.3e\r\n", maxdiff);
    cdc_reply(line);

    bench_free(ref); bench_free(work); bench_free(cmp);

    /* One Hermite voice, one block — the real per-voice cost to compare the
     * FFT numbers against. */
    const uint32_t rate = bsp_i2s_get_sample_rate() ? bsp_i2s_get_sample_rate() : 44100;
    auto *bank = new (std::nothrow) crosspad::SampleBank(bench_alloc, bench_free);
    auto *inst = new (std::nothrow) crosspad::PitchedInstrument();
    float *out = (float *)bench_alloc(kBenchFrames * 2 * sizeof(float));
    if (!bank || !inst || !out) {
        delete bank; delete inst; bench_free(out);
        cdc_reply("BENCH: ERR no PSRAM for voice\r\n");
        return;
    }
    int16_t *pcm = bank->allocPcm(rate);
    if (!pcm) {
        delete bank; delete inst; bench_free(out);
        cdc_reply("BENCH: ERR no PSRAM for zone\r\n");
        return;
    }
    const double w = 2.0 * M_PI * kBenchZoneHz / (double)rate;
    for (uint32_t i = 0; i < rate; ++i) {
        pcm[i] = (int16_t)(kBenchZoneAmp * sin(w * (double)i));
    }
    crosspad::Zone z;
    z.pcm = pcm; z.length = rate; z.rootMidi = kBenchZoneRoot;
    bank->addZone(z);

    crosspad::InstrumentParams params;
    params.maxVoices = crosspad::PitchedInstrument::kMaxVoices;
    params.attackMs  = 2.0f;
    params.releaseMs = 1.0f;
    inst->setParams(params);
    inst->onPrepare(kBenchFrames, rate);
    inst->setBank(bank);
    for (uint8_t p = 0; p < crosspad::PitchedInstrument::kPads; ++p) {
        inst->setPadMix(p, 100, 0);
    }

    auto benchVoices = [&](int voices) -> double {
        int64_t total = 0;
        for (int i = 0; i < iters; ++i) {
            if ((i % kBenchRearmBlocks) == 0) {
                inst->allNotesOff();
                for (int v = 0; v < voices; ++v) {
                    inst->noteOn((uint8_t)(kBenchFirstNote + v), 100,
                                 (uint8_t)(v % crosspad::PitchedInstrument::kPads));
                }
            }
            memset(out, 0, kBenchFrames * 2 * sizeof(float));
            const int64_t t0 = esp_timer_get_time();
            inst->mix(out, kBenchFrames);
            total += esp_timer_get_time() - t0;
            vTaskDelay(1);   /* idle-task watchdog, as above */
        }
        inst->allNotesOff();
        return (double)total / iters;
    };

    bench_row("voice64_us", benchVoices(1));
    bench_row("voice64x16_us", benchVoices(crosspad::PitchedInstrument::kPads));

    inst->setBank(nullptr);
    delete inst;
    delete bank;
    bench_free(out);
    cdc_reply("OK\r\n");
}

static volatile bool s_bench_busy = false;
static int s_bench_iters = kBenchDefaultIters;

static void pitched_bench_task(void *)
{
    handle_pitched_bench(s_bench_iters);
    s_bench_busy = false;
    /* Created WithCaps, so it must be deleted WithCaps or the stack leaks. */
    vTaskDeleteWithCaps(nullptr);
}

/* ── Heavy verbs, off the CDC task ────────────────────────────────────
 *
 * The CDC drain loop (main task, 5 kB stack) is also where the WiFi remote,
 * BLE companion and SysEx companion commands run. It validates, hands work
 * to the owner and answers; it does not do the work. A verb whose result
 * takes longer than a few ms either answers at once and leaves the result to
 * a status verb, or -- where the reply *is* the result and a host already
 * waits for it (SCREENSHOT, STM_FW_UPDATE) -- is answered by the task that
 * does the work, as FW_LIST / FW_INSTALL / PITCHED_BENCH already are. */

/* SCREENSHOT: rendered by LVGL in 40-row strips (lv_snapshot's own layer
 * redraw, one strip at a time) into one 38 kB buffer, and each strip written
 * by this task -- never the drain loop. A whole-screen RGB888 snapshot needs
 * one 230 kB block, which a board with a kit loaded does not have (PSRAM
 * largest ~90 kB: "ERR SCREENSHOT no memory"). The BMP is top-down (negative
 * height). A UI that changes mid-capture can show a seam between strips. */
static std::atomic<bool> s_shot_busy{false};
constexpr int32_t kShotRows = 40;
static lv_draw_buf_t* s_shotBuf = nullptr;      /* LVGL thread only */
static std::atomic<int32_t> s_shotY{0};
static std::atomic<int> s_shotState{0};         /* 0 pending, 1 rendered, 2 no memory */
static int32_t s_shotW = 0, s_shotH = 0;

static void shot_render_strip(void *)
{
    lv_obj_t* scr = lv_screen_active();
    lv_area_t a;
    lv_obj_get_coords(scr, &a);
    s_shotW = lv_area_get_width(&a);
    s_shotH = lv_area_get_height(&a);
    if (!s_shotBuf) s_shotBuf = lv_draw_buf_create(s_shotW, kShotRows, LV_COLOR_FORMAT_RGB888, LV_STRIDE_AUTO);
    if (!s_shotBuf) { s_shotState.store(2); return; }
    lv_draw_buf_clear(s_shotBuf, NULL);
    const int32_t y1 = a.y1 + s_shotY.load();
    lv_area_t strip = {a.x1, y1, a.x2, LV_MIN(y1 + kShotRows - 1, a.y2)};
    lv_layer_t layer;
    lv_layer_init(&layer);
    layer.draw_buf = s_shotBuf;
    layer.buf_area = {a.x1, y1, a.x1 + s_shotW - 1, y1 + kShotRows - 1};
    layer.color_format = LV_COLOR_FORMAT_RGB888;
    layer._clip_area = strip;
    layer.phy_clip_area = strip;
    lv_display_t* disp = lv_obj_get_display(scr);
    lv_display_t* dispOld = lv_refr_get_disp_refreshing();
    lv_layer_t* headOld = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    lv_obj_redraw(&layer, scr);
    /* and what the panel shows over it (the quick settings drawer, overlays):
     * a twin compares its own screen, top layer included, with this one */
    if (lv_obj_t* top = lv_display_get_layer_top(disp)) lv_obj_redraw(&layer, top);
    while (layer.draw_task_head) { lv_draw_dispatch_wait_for_request(); lv_draw_dispatch(); }
    disp->layer_head = headOld;
    lv_refr_set_disp_refreshing(dispOld);
    s_shotState.store(1);
}

static void screenshot_task(void *)
{
    const char* err = nullptr;
    FILE* f = fopen("/sdcard/crosspad/screen.bmp", "wb");
    if (!f) err = "ERR SCREENSHOT card\r\n";
    for (int32_t y = 0; !err && (y == 0 || y < s_shotH); y += kShotRows) {
        s_shotY.store(y);
        s_shotState.store(0);
        lv_async_call_locked(shot_render_strip, nullptr);
        for (int waited = 0; s_shotState.load() == 0 && waited < 3000; waited += 10) vTaskDelay(pdMS_TO_TICKS(10));
        const int st = s_shotState.load();
        if (st != 1) { err = st == 2 ? "ERR SCREENSHOT no memory\r\n" : "ERR SCREENSHOT busy\r\n"; break; }
        const uint32_t w = (uint32_t)s_shotW, row = (w * 3 + 3) & ~3u;
        if (y == 0) {
            uint8_t hd[54] = {'B', 'M'};
            auto put32 = [&](int at, uint32_t v) { for (int k = 0; k < 4; ++k) hd[at + k] = (uint8_t)(v >> (8 * k)); };
            put32(2, 54 + row * s_shotH); put32(10, 54); put32(14, 40); put32(18, w);
            put32(22, (uint32_t)(-s_shotH));   /* top-down */
            hd[26] = 1; hd[28] = 24; put32(34, row * s_shotH);
            if (fwrite(hd, 1, 54, f) != 54) err = "ERR SCREENSHOT card\r\n";
        }
        static const uint8_t pad[3] = {0, 0, 0};
        /* LVGL's RGB888 is B, G, R in memory, as BMP wants. */
        for (int32_t r = 0; !err && r < kShotRows && y + r < s_shotH; ++r) {
            if (fwrite(s_shotBuf->data + (uint32_t)r * s_shotBuf->header.stride, 1, w * 3, f) != w * 3 ||
                (row != w * 3 && fwrite(pad, 1, row - w * 3, f) != row - w * 3)) err = "ERR SCREENSHOT card\r\n";
        }
    }
    if (f && fclose(f) != 0 && !err) err = "ERR SCREENSHOT card\r\n";
    /* The strip buffer is LVGL's to free, on its thread. */
    lv_async_call_locked([](void*) { if (s_shotBuf) { lv_draw_buf_destroy(s_shotBuf); s_shotBuf = nullptr; } }, nullptr);
    cdc_reply(err ? err : "SCREENSHOT: /sdcard/crosspad/screen.bmp\r\n");
    s_shot_busy.store(false);
    vTaskDeleteWithCaps(nullptr);   /* created WithCaps */
}

/* STM_FW_UPDATE: ~10 s on the STM link (stm32_fw_update_ex owns it for the
 * duration). Run where the update overlay runs it -- a task of its own with
 * an internal stack, because the image is read off LittleFS (flash) -- and
 * answered by that task with the same "OK STM_FW" / "ERR STM_FW" line. */
static std::atomic<bool> s_stm_upd_busy{false};
EXT_RAM_BSS_ATTR static char s_stm_upd_path[96];
static bool s_stm_upd_force = false;

static void stm_fw_update_task(void *)
{
    esp_err_t uerr = stm32_fw_update_ex(s_stm_upd_path, s_stm_upd_force, nullptr, nullptr);
    char out[96];
    if (uerr == ESP_OK) {
        stm32_link_info_t li = {};
        stm32_hal_get_link_info(&li);
        snprintf(out, sizeof(out), "OK STM_FW %u.%u/0x%04X\r\n", li.fw_major, li.fw_minor, li.proto_version);
    } else {
        snprintf(out, sizeof(out), "ERR STM_FW %s\r\n", esp_err_to_name(uerr));
    }
    cdc_reply(out);
    s_stm_upd_busy.store(false);
    vTaskDelete(nullptr);
}

/* STM_FW_INFO reads the bundled image's descriptor off LittleFS, which wants
 * more stack than the CDC loop (the 4 kB main task) has left at this depth:
 * the verb overflowed it, a reboot per call. The read runs on a short task
 * with an internal stack (a flash read) while the caller waits, so the reply
 * still leaves from the caller -- the companion captures it there. */
struct StmFwInfoJob {
    stm32_fw_image_info_t img;
    esp_err_t err;
    StaticSemaphore_t doneBuf;
    SemaphoreHandle_t done;
};

static void stm_fw_info_task(void *arg)
{
    auto *job = static_cast<StmFwInfoJob *>(arg);
    job->err = stm32_fw_image_info(STM32_FW_BUNDLED_PATH, &job->img);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

static esp_err_t stm_fw_bundled_info(stm32_fw_image_info_t *img)
{
    StmFwInfoJob job = {};
    job.done = xSemaphoreCreateBinaryStatic(&job.doneBuf);
    if (xTaskCreate(stm_fw_info_task, "stm_fw_info", 4096, &job, 5, nullptr) != pdPASS) return ESP_ERR_NO_MEM;
    xSemaphoreTake(job.done, portMAX_DELAY);
    *img = job.img;
    return job.err;
}

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

bool hil_control_handle_cdc(const char *buff, size_t len)
{
    char cmd[160];
    /* A CDC line can be longer than this (CDC_LINE_MAX, for FS_* paths): no
     * verb here takes one, and a cut-off argument must not run. */
    if (len >= sizeof(cmd)) {
        cdc_reply("ERR too_long\r\n");
        return true;
    }
    memcpy(cmd, buff, len);
    cmd[len] = '\0';

    int a = -1, b = -1;
    char name[32] = {0};

    if (strncmp(cmd, "PAD_NOTES", 9) == 0) {
        handle_pad_notes();
        return true;
    }
    if (strncmp(cmd, "USB_MSC", 7) == 0) {
        handle_usb_msc(cmd + 7);
        return true;
    }
    if (strncmp(cmd, "NET_MODE", 8) == 0 && (cmd[8] == '\0' || cmd[8] == ' ')) {
        handle_net_mode(cmd + 8);
        return true;
    }
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
    if (strncmp(cmd, "FW_SLOTS", 8) == 0) {
        handle_fw_slots();
        return true;
    }
    if (strncmp(cmd, "FW_INSTALL", 10) == 0) {
        const char *args = cmd + 10;
        while (*args == ' ') args++;
        if (*args) handle_fw_install(args);
        else       handle_fw_install_status();
        return true;
    }
    if (strncmp(cmd, "FW_SWITCH", 9) == 0) {
        handle_fw_switch();
        return true;
    }
    if (strncmp(cmd, "FW_CONFIRM", 10) == 0) {
        handle_fw_confirm();
        return true;
    }
    if (fw_updater_handle(cmd, cdc_reply)) {
        return true;
    }
    if (cp_account_handle(cmd, cdc_reply)) {
        return true;
    }
    if (cp_remote_handle(cmd, cdc_reply)) {
        return true;
    }
    if (fw_ui_handle(cmd, cdc_reply)) {
        return true;
    }
    if (host_info_handle(cmd, cdc_reply)) {
        return true;
    }
    if (wifi_verbs_handle(cmd, cdc_reply)) {
        return true;
    }
    if (kit_convert_verbs_handle(cmd, cdc_reply)) {
        return true;
    }
    if (sdr_verbs_handle && sdr_verbs_handle(cmd, cdc_reply)) {
        return true;
    }
    /* TBAR -- the transport bar's four readouts, with geometry. They are in
     * no encoder group, so ENC_GROUP cannot reach them, and SCREENSHOT wants
     * 230 kB of contiguous PSRAM a board with a kit loaded does not have. */
    if (strcmp(cmd, "TBAR") == 0) {
        crosspad_gui::TransportBarReadout r[4];
        const uint8_t n = crosspad_gui::transportbar_probe(r, 4);
        if (n == 0) { cdc_reply("TBAR: none\r\n"); return true; }
        for (uint8_t i = 0; i < n; i++) {
            char row[128];
            snprintf(row, sizeof(row), "TBAR: %s x=%d y=%d w=%d h=%d text=%s\r\n",
                     r[i].id, (int)r[i].x, (int)r[i].y, (int)r[i].w, (int)r[i].h,
                     r[i].text ? r[i].text : "");
            cdc_reply(row);
        }
        cdc_reply("TBAR: end\r\n");
        return true;
    }
    if (strncmp(cmd, "UI_STATE", 8) == 0) {
        handle_ui_state();
        return true;
    }
    /* TWIN 1|0 -- the browser twin's keepalive (twin_mirror.h): while it keeps
     * coming, the LVGL inputs go out as SysEx 09 frames. */
    if (strncmp(cmd, "TWIN ", 5) == 0) {
        twin_mirror_enable(cmd[5] == '1');
        cdc_reply("OK\r\n");
        return true;
    }
    /* MOTION 1|0 -- a host page's keepalive (motion_stream.h): while it keeps
     * coming, the IMU goes out as SysEx 0A frames. */
    if (strncmp(cmd, "MOTION ", 7) == 0) {
        motion_stream_enable(cmd[7] == '1');
        cdc_reply("OK\r\n");
        return true;
    }
    /* TWIN_INFO P|C|<row> -- what Settings shows that the board alone knows,
     * through its own ISettingsUI: P the platform name and info, C the
     * coprocessor firmware, <row> an Info page row. Tab-separated fields. */
    /* The TWIN_* verbs read the GUI: a twin polling across a reboot must not
     * reach it before it exists (getGuiPlatform() asserts). */
    if (strncmp(cmd, "TWIN_", 5) == 0 && (!status.isGuiInitialised || !app_c)) {
        cdc_reply("ERR TWIN not_ready\r\n");
        return true;
    }
    if (strncmp(cmd, "TWIN_", 5) == 0) {
        SemaphoreHandle_t lock = twin_verb_lock();
        if (!lock || xSemaphoreTake(lock, pdMS_TO_TICKS(2000)) != pdTRUE) { cdc_reply("ERR TWIN busy\r\n"); return true; }
        const bool handled = twin_verb(cmd);
        xSemaphoreGive(lock);
        if (handled) return true;
    }
    if (strncmp(cmd, "PWR_GESTURE ", 12) == 0) {
        /* The power button cannot be pressed remotely; this injects what the
         * poll would have produced. The resolved context is logged by PWR_BTN
         * (the dispatch is asynchronous, so the reply cannot carry it). */
        const char *arg = cmd + 12;
        if (strncmp(arg, "CLICK", 5) == 0)      power_button_inject(false);
        else if (strncmp(arg, "HOLD", 4) == 0)  power_button_inject(true);
        else { cdc_reply("ERR PWR_GESTURE CLICK|HOLD\r\n"); return true; }
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "TASKS", 5) == 0) {
        /* To the console, not CDC: the table is long and the console survives
         * a starved main loop. Read twice and diff to see who owns a core. */
        perf_print_task_stats();
        cdc_reply("OK\r\n");
        return true;
    }
    /* TOAST <text> -- the toast a screen shows, with this text: how a long
     * one wraps and how long it stays, seen with SCREENSHOT. */
    if (strncmp(cmd, "TOAST ", 6) == 0) {
        static char s_toastText[200];
        snprintf(s_toastText, sizeof(s_toastText), "%s", cmd + 6);
        lv_async_call_locked([](void*) { ui_toast_show(s_toastText, 0, UI_TOAST_WARNING); }, nullptr);
        cdc_reply("OK\r\n");
        return true;
    }
    /* SCREENSHOT -- the active screen as /sdcard/crosspad/screen.bmp (24 bit),
     * for looking at a UI from the host: pull it with fs_transfer. */
    if (strcmp(cmd, "SCREENSHOT") == 0) {
        /* Rendered by the LVGL task, written by a task of its own, answered
         * by that task with the same line (screenshot_task): taken here, on
         * the 4 kB main task, the render overflowed it, and the wait and the
         * card write held every other verb behind them. */
        if (s_shot_busy.exchange(true)) { cdc_reply("ERR SCREENSHOT busy\r\n"); return true; }
        if (xTaskCreatePinnedToCoreWithCaps(screenshot_task, "screenshot", 4096, nullptr, 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            s_shot_busy.store(false);
            cdc_reply("ERR SCREENSHOT no task\r\n");
        }
        return true;
    }
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
    if (strncmp(cmd, "MEM_BLOCKS", 10) == 0) {
        handle_mem_blocks(strstr(cmd, "PSRAM") != nullptr);
        return true;
    }
    if (strncmp(cmd, "WORKER", 6) == 0) {
        handle_worker();
        return true;
    }
    if (strcmp(cmd, "ULP") == 0) {
        /* What the ULP RISC-V program has counted: a heartbeat that moves
         * proves the coprocessor runs beside the main CPU. */
        bsp_ulp_status_t u;
        bsp_ulp_get_status(&u);
        char out[120];
        snprintf(out, sizeof(out), "ULP: running=%d period_ms=%d heartbeat=%lu\r\n",
                 u.running ? 1 : 0, BSP_ULP_PERIOD_MS, (unsigned long)u.heartbeat);
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "RESET_LOG CLEAR", 15) == 0) {
        reset_log_clear();
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "IMU_REGW ", 9) == 0) {
        // Raw LSM6DS3TR-C register write for bench experiments: IMU_REGW <hex addr> <hex value>.
        unsigned addr = 0, val = 0;
        if (sscanf(cmd + 9, "%x %x", &addr, &val) != 2 || addr > 0x7F || val > 0xFF) {
            cdc_reply("ERR IMU_REGW <hexaddr> <hexval>\r\n");
            return true;
        }
        cdc_reply(bsp_imu_write_register((uint8_t)addr, (uint8_t)val) == ESP_OK ? "OK\r\n" : "ERR i2c\r\n");
        return true;
    }
    if (strncmp(cmd, "IMU_REG ", 8) == 0) {
        // Raw LSM6DS3TR-C register read: IMU_REG <hex addr> [len<=64] -> hex bytes.
        unsigned addr = 0, len = 1;
        if (sscanf(cmd + 8, "%x %u", &addr, &len) < 1 || addr > 0x7F || len < 1 || len > 64) {
            cdc_reply("ERR IMU_REG <hexaddr> [len]\r\n");
            return true;
        }
        uint8_t buf[64];
        if (bsp_imu_read_registers((uint8_t)addr, buf, len) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        char out[12 + 64 * 3];
        size_t n = snprintf(out, sizeof(out), "IMUREG %02X:", addr);
        for (unsigned i = 0; i < len; i++) n += snprintf(out + n, sizeof(out) - n, " %02X", buf[i]);
        snprintf(out + n, sizeof(out) - n, "\r\n");
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "STM_REGW ", 9) == 0) {
        // Raw STM32 register-map write for bench experiments: STM_REGW <hex addr> <hex value>.
        // Not saved: the next settings push (an STM reset, Settings) restores the stored value.
        unsigned addr = 0, val = 0;
        if (sscanf(cmd + 9, "%x %x", &addr, &val) != 2 || addr > 0xFFFF || val > 0xFF) {
            cdc_reply("ERR STM_REGW <hexaddr> <hexval>\r\n");
            return true;
        }
        cdc_reply(stm32_i2c_write_u8((uint16_t)addr, (uint8_t)val) == ESP_OK ? "OK\r\n" : "ERR i2c\r\n");
        return true;
    }
    if (strncmp(cmd, "STM_REG ", 8) == 0) {
        // Raw STM32 register-map read: STM_REG <hex addr> [len<=64] → hex bytes.
        unsigned addr = 0, len = 1;
        if (sscanf(cmd + 8, "%x %u", &addr, &len) < 1 || len < 1 || len > 64) {
            cdc_reply("ERR STM_REG <hexaddr> [len]\r\n");
            return true;
        }
        uint8_t buf[64];
        if (stm32_i2c_read((uint16_t)addr, buf, len) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        char out[8 + 64 * 3];
        size_t n = snprintf(out, sizeof(out), "REG %04X:", addr);
        for (unsigned i = 0; i < len; i++) n += snprintf(out + n, sizeof(out) - n, " %02X", buf[i]);
        snprintf(out + n, sizeof(out) - n, "\r\n");
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "RESET_LOG STM", 13) == 0) {
        char out[96];
        reset_log_stm_format(out, sizeof(out));
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "RESET_LOG", 9) == 0) {
        EXT_RAM_BSS_ATTR static char out[1024];
        reset_log_format(out, sizeof(out));
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "COREDUMP_INFO", 13) == 0) {
        EXT_RAM_BSS_ATTR static char out[512];
        reset_log_coredump_format(out, sizeof(out));
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "IMU_MIDI SLOT ", 14) == 0) {
        /* One slot per call: src tgt cc ch inv span. Everything is validated
         * against the enums rather than trusted, because a bad source index
         * would otherwise silently map to Pitch and look like a wiring bug. */
        int idx = -1, src = -1, tgt = -1, cc = -1, ch = -1, inv = -1, span = -1;
        if (sscanf(cmd, "IMU_MIDI SLOT %d %d %d %d %d %d %d",
                   &idx, &src, &tgt, &cc, &ch, &inv, &span) != 7 ||
            idx < 0 || idx >= (int)crosspad::kImuMidiSlots ||
            src < 0 || src >= (int)crosspad::ImuMidiSource::Count ||
            tgt < 0 || tgt >= (int)crosspad::ImuMidiTarget::Count ||
            cc < 0 || cc > 127 || ch < 0 || ch > 15 ||
            (inv != 0 && inv != 1) || span <= 0) {
            cdc_reply("ERR IMU_MIDI SLOT idx src tgt cc ch inv span "
                      "(src 0..4 pitch/roll/gx/gy/gz, tgt 0=cc 1=bend)\r\n");
            return true;
        }
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg) { cdc_reply("ERR no settings\r\n"); return true; }
        auto &slot = cfg->imuMidi.slots[idx];
        slot.enabled   = true;
        slot.source    = (uint8_t)src;
        slot.target    = (uint8_t)tgt;
        slot.cc        = (uint8_t)cc;
        slot.channel   = (uint8_t)ch;
        slot.invert    = (inv == 1);
        slot.inputSpan = (float)span;
        imu_midi_apply_settings();
        cfg->saveTo(NvsSettingsStore::getInstance());
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "IMU_MIDI ", 9) == 0) {
        int on = -1;
        if (sscanf(cmd, "IMU_MIDI %d", &on) == 1 && (on == 0 || on == 1)) {
            auto *cfg = crosspad::CrosspadSettings::getInstance();
            if (cfg) {
                cfg->imuMidi.enabled = (on == 1);
                cfg->saveTo(NvsSettingsStore::getInstance());
            }
            imu_midi_set_enabled(on == 1);
            cdc_reply("OK\r\n");
        } else {
            cdc_reply("ERR IMU_MIDI 0|1\r\n");
        }
        return true;
    }
    if (strncmp(cmd, "HEAP_TRACE ", 11) == 0) {
        handle_heap_trace(cmd + 11);
        return true;
    }
    if (strncmp(cmd, "TOUCH_TRACE ", 12) == 0) {
        handle_touch_trace(cmd + 12);
        return true;
    }
    if (strncmp(cmd, "TOUCH_DRAG ", 11) == 0) {
        handle_touch_inject(cmd + 11, false);
        return true;
    }
    if (strncmp(cmd, "TOUCH_TAP ", 10) == 0) {
        handle_touch_inject(cmd + 10, true);
        return true;
    }
    if (strcmp(cmd, "TOPBAR") == 0) {
        handle_topbar();
        return true;
    }
    if (strcmp(cmd, "UI_TREE") == 0) {
        handle_ui_tree();
        return true;
    }

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
    if (strncmp(cmd, "CLOCK_STATUS", 12) == 0) {
        char out[200];
        midi_clock_status(out, sizeof(out), strstr(cmd + 12, "RESET") != nullptr);
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "IMU_GESTURE ", 12) == 0) {
        /* The board cannot be knocked from the bench; this posts what the IMU
         * driver would have, on the event bus, so an app's reaction to a
         * gesture (sampler Advanced, DAW Control mode) is testable. */
        static const struct { const char *name; crosspad::MotionGesture g; } kNames[] = {
            {"TAP",         crosspad::MotionGesture::SingleTap},
            {"DOUBLE_TAP",  crosspad::MotionGesture::DoubleTap},
            {"FREE_FALL",   crosspad::MotionGesture::FreeFall},
            {"WAKE_UP",     crosspad::MotionGesture::WakeUp},
            {"ORIENTATION", crosspad::MotionGesture::Orientation},
            {"STILL",       crosspad::MotionGesture::Still},
        };
        const char *arg = cmd + 12;
        for (const auto &n : kNames) {
            if (strncmp(arg, n.name, strlen(n.name)) == 0) {
                crosspad::getEventBus().postMotionGesture(n.g, 0, crosspad::EventSource::Cli);
                cdc_reply("OK\r\n");
                return true;
            }
        }
        cdc_reply("ERR IMU_GESTURE TAP|DOUBLE_TAP|FREE_FALL|WAKE_UP|ORIENTATION|STILL\r\n");
        return true;
    }
    if (strncmp(cmd, "IMU_THS ", 8) == 0) {
        /* IMU_THS <tap 0-31> <wake 0-63>: the embedded tap and wake-up thresholds,
         * for tuning on hardware (not persisted). */
        int tap = -1, wake = -1;
        if (sscanf(cmd + 8, "%d %d", &tap, &wake) != 2 || tap < 0 || wake < 0 ||
            bsp_imu_set_thresholds((uint8_t)tap, (uint8_t)wake) != ESP_OK) {
            cdc_reply("ERR IMU_THS <tap 0-31> <wake 0-63>\r\n");
        } else {
            cdc_reply("OK\r\n");
        }
        return true;
    }
    if (strncmp(cmd, "IMU", 3) == 0) {
        handle_imu();
        return true;
    }
    if (strncmp(cmd, "SETTINGS_RESET", 14) == 0) {
        if (strstr(cmd, "FORCE") == nullptr) {
            cdc_reply("ERR say FORCE — this erases every saved setting and reboots\r\n");
            return true;
        }
        handle_settings_reset();
        return true;
    }
    if (strncmp(cmd, "RESET_TEST ", 11) == 0) {
        // Kill the device three ways, to check the black box end to end.
        // Each one reboots the ESP.
        const char *how = cmd + 11;
        if (strncmp(how, "panic", 5) == 0) {
            cdc_reply("OK panic\r\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            abort();
        }
        if (strncmp(how, "wdt", 3) == 0) {
            // Starve IDLE0 until the task watchdog fires — a core dump,
            // with TASK_WDT_PANIC.
            cdc_reply("OK wdt\r\n");
            vTaskDelay(pdMS_TO_TICKS(100));
            for (;;) { }
        }
        if (strncmp(how, "stmwd", 5) == 0) {
            // Silence the heartbeat past the STM watchdog: it pulses EN and
            // the ESP returns with a POWERON reason.
            cdc_reply("OK stmwd\r\n");
            stm32_hal_heartbeat_pause_ms(15000);
            return true;
        }
        cdc_reply("ERR panic|wdt|stmwd\r\n");
        return true;
    }
    if (strncmp(cmd, "MEM", 3) == 0) {
        handle_mem();
        return true;
    }
    if (strncmp(cmd, "LVGL_PROF", 9) == 0) {
        /* LVGL_PROF 1 arms the trace, 2 arms it for the next big frame,
         * 0 stops and dumps it. */
        lvgl_stats_prof(atoi(cmd + 9), [](const char* l) { cdc_reply(l); });
        cdc_reply("LVGLPROF: end\r\n");
        return true;
    }
    if (strncmp(cmd, "LVGL_INV", 8) == 0) {
        /* Which rectangles a UI step dirtied, not just how many pixels. */
        lvgl_stats_inv_dump([](const char* l) { cdc_reply(l); });
        return true;
    }
    if (strcmp(cmd, "SPLASH") == 0) {
        /* The boot splash again, over whatever is on screen, gone by itself
         * after ~2.2 s: to look at it, and bracketed by LVGL_STATS, to time
         * its frames without a reboot. */
        /* Started on the LVGL thread: it decodes a 300 kB image, which held
         * the LVGL lock -- and this task -- for the whole decode. */
        const bool running = boot_splash_active();
        cdc_reply(running ? "ERR SPLASH already playing\r\n" : "OK\r\n");   /* reply first */
        if (!running) {
            lv_async_call_locked([](void*) {
                if (boot_splash_active()) return;
                auto* cfg = crosspad::CrosspadSettings::getInstance();
                boot_splash_start(lv_layer_top(), cfg ? cfg->LCDbrightness : 100,
                                  [](uint8_t v) { crosspad::getStm32CommandSender().sendBrightness(v); }, true);
            }, nullptr);
        }
        return true;
    }
    if (strncmp(cmd, "LVGL_STATS", 10) == 0) {
        /* Read-and-cleared like CDC_STATS: bracket a window with two reads. */
        char reply[240];
        lvgl_stats_report(reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "CDC_STATS", 9) == 0) {
        /* The three fates of a command with no reply: rx = USB reads queued,
         * cmds = complete lines dispatched, drop = lost to a full queue. */
        char reply[256];
        /* maxcmd/maxgap are read-and-cleared: bracket a window with two reads
         * and the second one is that window's worst case. maxcmd is time spent
         * inside one handler, maxgap is time a command already queued waited
         * for the drain loop — a late reply is one or the other. */
        /* stack_min= is the least stack, in bytes, any command since boot
         * left free on the main task, stack_verb= the verb that did: not
         * cleared, the worst case is the one that matters. stack_last= is
         * what the CDC command before this one left. Ahead of maxcmd=, whose
         * value can hold spaces. */
        snprintf(reply, sizeof(reply),
                 "CDCSTATS: rx=%u cmds=%u drop=%u maxcmd_us=%u maxgap_us=%u stack_min=%u stack_verb=%s "
                 "stack_last=%u maxcmd=%s\r\n",
                 (unsigned)g_cdc_rx_chunks.load(std::memory_order_relaxed),
                 (unsigned)g_cdc_commands.load(std::memory_order_relaxed),
                 (unsigned)g_cdc_dropped.load(std::memory_order_relaxed),
                 (unsigned)g_cdc_max_cmd_us.exchange(0, std::memory_order_relaxed),
                 (unsigned)g_cdc_max_gap_us.exchange(0, std::memory_order_relaxed),
                 (unsigned)s_stack_min, s_stack_verb,
                 (unsigned)g_cdc_last_stack_free.load(std::memory_order_relaxed), g_cdc_max_cmd_verb);
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "BLE_STATUS", 10) == 0) {
        handle_ble_status();
        return true;
    }
    if (strncmp(cmd, "BLE_DEVICES", 11) == 0) {
        handle_ble_devices();
        return true;
    }
    if (strncmp(cmd, "BLE_STOP", 8) == 0) {
        cdc_reply("OK queued\r\n");   /* reply first */
        ble_job_post(0, 0, false);      /* on the LVGL thread; BLE_STATUS req= */
        return true;
    }
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
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
#endif
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
    if (strncmp(cmd, "USB_GUARD", 9) == 0) {
        /* The USB serial guard (usb_serial_prompt): with it on, a host's SysEx
         * request for the CDC profile is confirmed on the device. A bench
         * board turns it off once, persisted — or every unattended
         * `usb-mode set default` stops at the prompt. */
        auto *cfg = crosspad::CrosspadSettings::getInstance();
        if (!cfg) { cdc_reply("ERR no settings\r\n"); return true; }
        if (sscanf(cmd, "USB_GUARD %d", &a) == 1) {
            if (a != 0 && a != 1) { cdc_reply("ERR usage USB_GUARD [0|1]\r\n"); return true; }
            cfg->usbSerialGuard = (a == 1);
            cfg->saveTo(NvsSettingsStore::getInstance());
        }
        char out[24];
        snprintf(out, sizeof(out), "USB_GUARD: %d\r\n", cfg->usbSerialGuard ? 1 : 0);
        cdc_reply(out);
        return true;
    }
    if (strcmp(cmd, "OTA_PREFS") == 0) {
        /* Read-only: whether an OTA over CDC will stop at the on-screen Allow
         * prompt (auto_accept=0) or start at once. Set only from the Firmware
         * app's checkbox. */
        char out[32];
        snprintf(out, sizeof(out), "OTA_PREFS: auto_accept=%d\r\n", ota_auto_accept_enabled() ? 1 : 0);
        cdc_reply(out);
        return true;
    }
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
    if (sscanf(cmd, "BLE_SCAN %d", &a) == 1) {
        crosspad::getEspBleMidi().startScan((uint16_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "BLE_SCAN", 8) == 0) {
        crosspad::getEspBleMidi().startScan(5000);
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "BLE_CONNECT", 11) == 0) {
        char addr[24] = {0};
        sscanf(cmd, "BLE_CONNECT %23s", addr);
        cdc_reply(crosspad::getEspBleMidi().connectToDevice(addr) ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    if (strncmp(cmd, "BLE_DISCONNECT", 14) == 0) {
        crosspad::getEspBleMidi().disconnect();
        cdc_reply("OK\r\n");
        return true;
    }
    if (sscanf(cmd, "BLE_TXOFF %d", &a) == 1 && a >= -127 && a <= 127) {
        /* Transport-level, not persisted — proves the shift on the wire
         * without touching the user's settings. */
        crosspad::getEspBleMidi().setTxNoteOffset((int8_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
    if (sscanf(cmd, "BLE_SEND %d %d", &a, &b) >= 1 && a >= 0 && a < 128) {
        /* Straight at the BLE transport: a dead link vs an unrouted pad. */
        auto &ble = crosspad::getEspBleMidi();
        ble.sendNoteOn((uint8_t)a, (uint8_t)(b < 0 ? 100 : b), 0);
        ble.sendNoteOff((uint8_t)a, 0, 0);
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "APP_LIST", 8) == 0) {
        handle_app_list();
        return true;
    }
    if (strncmp(cmd, "APP_VERSIONS", 12) == 0) {
        handle_app_versions();
        return true;
    }
    if (strncmp(cmd, "APP_SHA", 7) == 0) {
        handle_app_sha();
        return true;
    }
    if (sscanf(cmd, "APP_START %31s", name) == 1) {
        if (!find_app(name)) {
            cdc_reply("ERR unknown app\r\n");
            return true;
        }
        strncpy(s_pending_app, name, sizeof(s_pending_app) - 1);
        s_pending_app[sizeof(s_pending_app) - 1] = '\0';
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_start, nullptr);
        return true;
    }
    if (strncmp(cmd, "ENC_FOCUS", 9) == 0) {
        handle_enc_focus();
        return true;
    }
    if (strncmp(cmd, "ENC_GROUP", 9) == 0) {
        handle_enc_group();
        return true;
    }
    /* ENC_RAW -- the encoder bytes exactly as the STM sends them. The focus
     * moving while nobody is turning anything is either this wobbling or the
     * driver reading it wrong, and there is no telling which from the UI. */
    if (strncmp(cmd, "ENC_RAW", 7) == 0) {
        const auto& in = crosspad::getStm32MessageHandler().input;
        char reply[96];
        snprintf(reply, sizeof(reply), "ENCRAW: top=%d phase=0x%02X pressed=%d\r\n",
                 (int)in.encoderTop, (unsigned)in.encoderPhase,
                 in.encoderPressed ? 1 : 0);
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "ENC_STATE", 9) == 0) {
        handle_enc_state();
        return true;
    }
    if (strncmp(cmd, "LED_STATE", 9) == 0) {
        /* LED_STATE COALESCE 0|1: the A/B of who sends the frames (main.cpp
         * turns it on). Off, the frame marked dirty meanwhile goes out now. */
        if (sscanf(cmd + 9, " COALESCE %d", &a) == 1) {
            auto &leds = crosspad::getPadLedController();
            leds.setCoalescedRefresh(a != 0);
            if (!a) leds.updateLEDs();
        }
        handle_led_state();
        return true;
    }
    if (strncmp(cmd, "PAD_STATS_RESET", 15) == 0) {
        g_hil_pad_press_count = 0;
        g_hil_pad_release_count = 0;
        g_hil_pad_inject_drop = 0;
        g_hil_note_on_count = 0;
        g_hil_last_note_on = 0;
        g_hil_stm_press_count = 0;
        g_hil_stm_at_count = 0;
        g_stm_uart_ovf = 0;
        g_stm_sbuf_drop = 0;
        g_usb_midi_tx_ok = 0;
        g_usb_midi_tx_fail = 0;
        stm32_get_rx_msg_count(true);
        g_note_lat_last_us = 0; g_note_lat_max_us = 0; g_note_lat_count = 0; g_note_lat_sum_us = 0;
        cdc_reply("OK\r\n");
        return true;
    }
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
    if (strncmp(cmd, "STM_VPAD", 8) == 0) {
        // STM-side virtual pads: STM_VPAD <rate_hz> <count> [hold_ms] [mode]
        // | STOP | (no args = status). Hits leave the STM on the physical
        // pad's own path — note-on, pressure stream, note-off — so the whole
        // link is measured.
        if (strncmp(cmd + 8, " STOP", 5) == 0) {
            cdc_reply(stm32_i2c_write_u8(REG_CMD_VPAD, 0) == ESP_OK ? "OK\r\n" : "ERR i2c\r\n");
            return true;
        }
        unsigned rate = 0, count = 0, hold = 100, mode = 0;
        int n = sscanf(cmd + 8, "%u %u %u %u", &rate, &count, &hold, &mode);
        if (n >= 2) {
            if (rate < 1 || rate > 65535 || count < 1 || count > 65535 || hold > 255 || mode > 255) {
                cdc_reply("ERR STM_VPAD <rate_hz> <count> [hold_ms] [mode]\r\n");
                return true;
            }
            uint8_t cfg[6] = { (uint8_t)(rate >> 8), (uint8_t)rate, (uint8_t)(count >> 8), (uint8_t)count,
                               (uint8_t)hold, (uint8_t)mode };
            if (stm32_i2c_write(REG_CTL_VPAD_RATE_HI, cfg, sizeof(cfg)) != ESP_OK ||
                stm32_i2c_write_u8(REG_CMD_VPAD, 1) != ESP_OK) {
                cdc_reply("ERR i2c\r\n");
                return true;
            }
        }
        uint8_t d[36];
        if (stm32_i2c_read(REG_DEBUG_VPAD, d, sizeof(d)) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        uint32_t w[9];
        for (int i = 0; i < 9; i++)
            w[i] = d[4*i] | (d[4*i+1] << 8) | (d[4*i+2] << 16) | ((uint32_t)d[4*i+3] << 24);
        char out[160];
        snprintf(out, sizeof(out),
                 "VPAD: sent=%lu at_sent=%lu remaining=%lu short_esp=%lu short_usb=%lu esp_busy=%lu usb_busy=%lu esp_hwm=%lu usb_hwm=%lu\r\n",
                 (unsigned long)w[0], (unsigned long)w[8], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
                 (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
        cdc_reply(out);
        return true;
    }
    /* CLICK <0|1> [bpm] [bars] -- the metronome, which is also the sequencer's
     * clock. Reports where it is, so a scenario can check the grid without
     * listening. */
    if (strncmp(cmd, "CLICK", 5) == 0) {
        int on = -1, bpm = 0, bars = 0;
        const int n = sscanf(cmd + 5, "%d %d %d", &on, &bpm, &bars);
        auto &m = getMetronome();
        if (n >= 2 && bpm > 0) m.setBpm((float)bpm);
        if (n >= 3 && bars > 0) m.setBarCount((uint8_t)bars);
        if (n >= 1) { if (on) m.start(); else m.stop(); }
        char reply[128];
        snprintf(reply, sizeof(reply),
                 "CLICK: running=%d bpm=%.1f beats=%u bars=%u bar=%u beat=%u pos=%lu/%lu\r\n",
                 m.running() ? 1 : 0, m.bpm(), (unsigned)m.beatsPerBar(),
                 (unsigned)m.barCount(), (unsigned)m.currentBar(), (unsigned)m.currentBeat(),
                 (unsigned long)m.positionSamples(), (unsigned long)m.loopSamples());
        cdc_reply(reply);
        return true;
    }
    /* SEQ_DUMP -- every recorded event, so a scenario can check the grid a take
     * landed on without listening to it. len= is how long the pad was held, in
     * samples, 0 for a tap. Its own verb rather than a SEQ
     * subcommand: the reply is multi-line and a reader cannot tell the two
     * shapes apart from one prefix. */
    if (strncmp(cmd, "SEQ_DUMP", 8) == 0) {
        auto& sq = getSequencer();
        for (uint8_t s = 0; s < crosspad::PatternSequencer::kNumSequences; ++s) {
            for (uint16_t i = 0; i < sq.eventCount(s); ++i) {
                uint32_t pos = 0; uint8_t pad = 0, vel = 0;
                if (!sq.eventAt(s, i, pos, pad, vel)) break;
                char row[112];
                /* len: samples the note is held, 0 for a hit. */
                snprintf(row, sizeof(row), "SEQDUMP: seq=%u i=%u pos=%lu pad=%u vel=%u len=%lu\r\n",
                         (unsigned)s, (unsigned)i, (unsigned long)pos,
                         (unsigned)pad, (unsigned)vel, (unsigned long)sq.eventLength(s, i));
                cdc_reply(row);
            }
        }
        cdc_reply("SEQDUMP: end\r\n");
        return true;
    }
    /* SEQ [REC|OVERDUB|PLAY|STOP|CLEAR|DOUBLE|HALF|ERASE 0|1|COUNTIN 0|1|BARS n|BPM n|QUANT n] -- the loop recorder.
     * Bare SEQ reports, so a scenario can check what was captured without
     * listening to it. */
    if (strncmp(cmd, "SEQ", 3) == 0 && (cmd[3] == 0 || cmd[3] == ' ')) {
        auto& sq = getSequencer();
        const char* arg = cmd + 3;
        while (*arg == ' ') ++arg;
        int n = 0, n2 = 0;
        if      (strncmp(arg, "REC", 3) == 0)     sq.record();
        else if (strncmp(arg, "OVERDUB", 7) == 0) sq.overdub();
        else if (strncmp(arg, "PLAY", 4) == 0)    sq.playback();
        else if (strncmp(arg, "STOP", 4) == 0)    sq.stop();
        else if (strncmp(arg, "CLEAR", 5) == 0)   sq.reset();
        else if (sscanf(arg, "SCENE %d", &n) == 1 && n >= 0) sq.selectScene((uint8_t)n);
        else if (strncmp(arg, "SAVE", 4) == 0) crosspad::sequence_store_save_soon();
        else if (sscanf(arg, "BARS %d %d", &n, &n2) == 2 && n >= 0 && n2 > 0)
            sq.setSceneBars((uint8_t)n, (uint8_t)n2);
        else if (sscanf(arg, "BARS %d", &n) == 1 && n > 0)  sq.setBars((uint8_t)n);
        else if (sscanf(arg, "BPM %d", &n) == 1 && n > 0)   sq.setClickBpm((float)n);
        else if (strncmp(arg, "ADD", 3) == 0)     sq.addScene();
        else if (strncmp(arg, "DOUBLE", 6) == 0)  sq.doubleScene(sq.activeScene());
        else if (strncmp(arg, "HALF", 4) == 0)    sq.halveScene(sq.activeScene());
        else if (sscanf(arg, "ERASE %d", &n) == 1) sq.setErasing(n != 0);
        else if (sscanf(arg, "COUNTIN %d", &n) == 1) sq.setCountIn(n != 0);
        else if (sscanf(arg, "DEL %d", &n) == 1 && n >= 0) sq.removeScene((uint8_t)n);
        else if (strncmp(arg, "DEL", 3) == 0)     sq.removeScene(sq.activeScene());
        else if (sscanf(arg, "QUANT %d", &n) == 1)
            sq.setQuantization((crosspad::Quantization)n);
        else if (*arg) { cdc_reply("ERR SEQ REC|OVERDUB|PLAY|STOP|CLEAR|SAVE|SCENE n|ADD|DEL [n]|DOUBLE|HALF|ERASE 0|1|COUNTIN 0|1|BARS [s] n|BPM n|QUANT n\r\n"); return true; }

        /* Every scene the engine can hold, not four: a scenario that adds a
         * scene of its own lands past the player's, at any index up to 7. */
        char bars[48] = "", ev[64] = "";
        for (uint8_t s = 0; s < crosspad::PatternSequencer::kNumSequences; ++s) {
            const char* sep = s ? "/" : "";
            snprintf(bars + strlen(bars), sizeof(bars) - strlen(bars), "%s%u", sep, (unsigned)sq.sceneBars(s));
            snprintf(ev + strlen(ev), sizeof(ev) - strlen(ev), "%s%u", sep, (unsigned)sq.eventCount(s));
        }
        EXT_RAM_BSS_ATTR static char reply[320];   // static: the path makes it too big for the 5 kB main task
        snprintf(reply, sizeof(reply),
                 "SEQ: mode=%u state=%u scenes=%u scene=%u>%u bars=%s "
                 "ev=%s bpm=%.1f quant=%u pos=%.3f dirty=%d erase=%d countin=%d file=%s\r\n",
                 (unsigned)sq.mode(), (unsigned)sq.state(), (unsigned)sq.sceneCount(),
                 (unsigned)sq.activeScene(), (unsigned)sq.pendingScene(),
                 bars, ev,
                 sq.getClickBpm(), (unsigned)sq.quantization(),
                 (double)sq.progress(), sq.dirty() ? 1 : 0, sq.erasing() ? 1 : 0, sq.countIn() ? 1 : 0,
                 crosspad::sequence_store_current_file());
        cdc_reply(reply);
        return true;
    }
    /* SONG [ADD s r|SET i s r|DEL i|CLEAR|PLAY [i]|LOOP 0|1] -- the song
     * arrangement. Bare SONG reports the blocks and where playback is. */
    if (strncmp(cmd, "SONG", 4) == 0 && (cmd[4] == 0 || cmd[4] == ' ')) {
        auto& sq = getSequencer();
        const char* arg = cmd + 4;
        while (*arg == ' ') ++arg;
        int a = 0, b = 0, c = 0;
        if      (sscanf(arg, "ADD %d %d", &a, &b) == 2) sq.songInsert(sq.songLength(), (uint8_t)a, (uint8_t)b);
        else if (sscanf(arg, "SET %d %d %d", &a, &b, &c) == 3) sq.songSet((uint8_t)a, (uint8_t)b, (uint8_t)c);
        else if (sscanf(arg, "DEL %d", &a) == 1) sq.songRemove((uint8_t)a);
        else if (strncmp(arg, "CLEAR", 5) == 0) { while (sq.songLength()) sq.songRemove(0); }
        else if (sscanf(arg, "LOOP %d", &a) == 1) sq.setSongLoop(a != 0);
        else if (strncmp(arg, "PLAY", 4) == 0) {
            if (sscanf(arg, "PLAY %d", &a) != 1) a = 0;
            sq.setSongMode(true);
            sq.songLocate((uint8_t)a);
            sq.playback();
        }
        else if (*arg) { cdc_reply("ERR SONG ADD s r|SET i s r|DEL i|CLEAR|PLAY [i]|LOOP 0|1\r\n"); return true; }

        char reply[240];
        constexpr int kTail = 3;
        int n = snprintf(reply, sizeof(reply), "SONG: mode=%d loop=%d len=%u pos=%u pass=%u scene=%u blocks=",
                         sq.songMode() ? 1 : 0, sq.songLoop() ? 1 : 0, (unsigned)sq.songLength(),
                         (unsigned)sq.songPosition(), (unsigned)sq.songPass(), (unsigned)sq.activeScene());
        for (uint8_t i = 0; i < sq.songLength(); ++i) {
            uint8_t sc = 0, r = 0;
            sq.songBlock(i, sc, r);
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%u:%u", i ? "," : "", (unsigned)sc, (unsigned)r);
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    /* ASONG [OPEN name|BOUNCE scene track|PLAY [bar]|STOP|CANCEL|LEN bars|SYNC 0|1|TEMPO bpm|RENAME name|
     *        REGION a b|MERGE mask from to|IMPORT path a b track bar lead|EXPORT|EXPORTWAV|REPORT|
     *        REC bar [beats]|DUB bar [beats]|PUNCH R|D|N|BOUNCESEQ track|VOL track pct|MUTE track 0|1|
 *        CUT track bar|DUPREGION|SEQTRACK [kit]|PITCHTRACK instrument|DELTRACK track|
     *        DUP track block|DELBLOCK track block|MOVE track block bar|PASTE a b at|TRACKKIT track kit] -- the
     * audio song: the open project, a bounce in progress, playback. Scene and
     * track are 0-based. Every verb only asks -- the card work runs on the
     * song engine's worker, never on this drain loop -- so ok=1 means the
     * request was taken; poll bare ASONG for open=, play= and bounce=.
     * EXPORT writes the song as an Ableton Live Set into its folder (edit=
     * says when it is done); REPORT puts the last export's lines, one
     * "REPORT:" row each, before the status. REC and DUB record the pads onto
     * the seq track from a bar (Arrange's REC/DUB, beats of count-in), PUNCH
     * puts a REC or DUB in, or the take out, while the song plays; ASONG_SEQ
     * says what landed. */
    /* ASONG_SEQ [track] -- a seq track's blocks (the first seq track's
     * without one), one "ASEQ:" row each (bar, bars, from, notes, own=1 for
     * notes of their own, 0 for the scene's), then the take running, the
     * block the seq view edits, the track's route and the armed track. */
    if (strncmp(cmd, "ASONG_SEQ", 9) == 0 && (cmd[9] == 0 || cmd[9] == ' ')) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ASEQ: no engine\r\n"); return true; }
        static char row[112];
        int track = -1, route = -1, armed = -1;
        int asked = -1;
        if (sscanf(cmd + 9, " %d", &asked) != 1) asked = -1;
        {
            std::lock_guard<std::recursive_mutex> l(se->songMutex());
            const crosspad::AudioSong& song = se->song();
            for (int t = 0; t < crosspad::AudioSong::kTracks && track < 0; ++t)
                if (song.seqRoute(t) >= 0 && (asked < 0 || t == asked)) track = t;
            route = song.seqRoute(track);
            armed = song.armedSeqTrack();
            if (track >= 0) {
                for (const auto& c : song.tracks[track].clips) {
                    snprintf(row, sizeof(row), "ASEQ: bar=%u bars=%u from=%u notes=%u own=%d\r\n",
                             (unsigned)c.bar, (unsigned)c.bars, (unsigned)c.from,
                             (unsigned)(c.notes ? c.notes->notes.size() : 0), c.notes ? 1 : 0);
                    cdc_reply(row);
                }
            }
        }
        snprintf(row, sizeof(row), "ASEQ: track=%d take=%d punchable=%d edit=%d route=%d armed=%d\r\n", track,
                 (int)se->seqTake(), se->seqPunchable() ? 1 : 0, (int)se->seqEditBar(), route, armed);
        cdc_reply(row);
        return true;
    }
    /* ASONG_TRK -- every track in use, one "ATRK:" row each: its kind (a
     * audio, s seq), the route its notes take and the one they take now
     * (AudioSong::liveRoute(): -1 for a kit track the kits' engine does not
     * hold), its freeze (FreezeState: 0 none, 1 frozen, 2 stale, 3 freezing),
     * its blocks and its meter now (lvl=, the louder side, what the lane's
     * meter shows; a seq engine's peak is taken by the ask); then the armed
     * track and the live kit track (liveKit). */
    if (strcmp(cmd, "ASONG_TRK") == 0) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ATRK: no engine\r\n"); return true; }
        EXT_RAM_BSS_ATTR static char row[96];
        int armed = -1, live_kit = -1, used = 0;
        for (int t = 0; t < crosspad::AudioSong::kTracks; ++t) {
            bool inUse = false, seq = false;
            int route = -1, live = -1;
            unsigned blocks = 0;
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                const crosspad::AudioSong& song = se->song();
                const crosspad::SongTrack& tr = song.tracks[t];
                inUse = tr.inUse;
                seq = tr.kind == crosspad::TrackKind::Seq;
                route = song.seqRoute(t);
                live = song.liveRoute(t);
                blocks = (unsigned)tr.clips.size();
                armed = song.armed;
                live_kit = song.liveKit;
            }
            if (!inUse) continue;
            ++used;
            float ll = 0.0f, lr = 0.0f;
            se->trackLevels(t, ll, lr);
            snprintf(row, sizeof(row), "ATRK: t=%d kind=%c route=%d live=%d fz=%d blocks=%u lvl=%.2f\r\n", t,
                     seq ? 's' : 'a', route, live, (int)se->freezeState((uint8_t)t), blocks, std::max(ll, lr));
            cdc_reply(row);
        }
        snprintf(row, sizeof(row), "ATRK: used=%d armed=%d live_kit=%d\r\n", used, armed, live_kit);
        cdc_reply(row);
        return true;
    }
    /* Why a playing song can be silent. ASONG already fills its 300-byte
     * budget on the 5 kB main task, so this is its own line. */
    if (strcmp(cmd, "ASONG_DBG") == 0) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ASONGDBG: no engine\r\n"); return true; }
        /* Static: the line outgrew what this task's stack should carry. */
        EXT_RAM_BSS_ATTR static char row[544];
        snprintf(row, sizeof(row), "ASONGDBG: %s\r\n", se->debugLine());
        cdc_reply(row);
        return true;
    }
    if (strncmp(cmd, "ASONG", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ')) {
        auto* se = crosspad::getPlatformServices().songEngine;
        if (!se) { cdc_reply("ERR ASONG no song engine\r\n"); return true; }
        const char* arg = cmd + 5;
        while (*arg == ' ') ++arg;
        int a = 0, b = 0, c3 = 0;
        bool ok = true;
        if      (strncmp(arg, "OPEN ", 5) == 0)             ok = se->requestOpen(arg + 5);
        else if (sscanf(arg, "BOUNCESEQ %d", &a) == 1)      ok = se->bounceSeqTrack((uint8_t)a);   // as Arrange's BOUNCE
        else if (sscanf(arg, "VOL %d %d", &a, &b) == 2 || sscanf(arg, "MUTE %d %d", &a, &b) == 2) {
            /* A track's fader in percent, or its M -- as Arrange's MIX sets them. */
            ok = a >= 0 && a < crosspad::AudioSong::kTracks;
            if (ok) {
                {
                    std::lock_guard<std::recursive_mutex> l(se->songMutex());
                    auto& t = se->song().tracks[a];
                    if (arg[0] == 'V') t.volume = (float)(b < 0 ? 0 : b) / 100.0f;
                    else               t.muted = b != 0;
                }
                se->commit();
            }
        }
        else if (sscanf(arg, "FREEZE %d", &a) == 1) {
            /* Freeze track a now (Arrange does it when ARM moves on): renders
             * found on the card are taken, the rest made. */
            ok = a >= 0 && a < crosspad::AudioSong::kTracks && se->freezeTrack((uint8_t)a);
        }
        else if (sscanf(arg, "OFFLINE %d", &a) == 1) {
            /* A freeze off the clock (1, the default) or in real time (0). */
            crosspad::song_engine_set_offline(a != 0);
        }
        else if (sscanf(arg, "ARM %d", &a) == 1) {
            /* The seq track REC and DUB take onto -- Arrange's ARM. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().armed = (int8_t)a;
            }
            se->commit();
        }
        else if (strncmp(arg, "SEQTRACK ", 9) == 0 || strcmp(arg, "SEQTRACK") == 0) {
            /* A kit track on the lowest free track, armed, with a scene of
             * its own -- what Arrange's ADD TRACK > KITS does -- playing the
             * kit named (a kit.json as the kit list names it), or the song's.
             * It plays live once Arrange hands it the kits' engine. */
            const char* kit = arg[8] == ' ' ? arg + 9 : "";
            auto* sq = crosspad::getPlatformServices().sequencer;
            const bool added = sq && sq->addScene();
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                const int scene = song.sceneForNewSeq(sq ? sq->getSceneCount() : 1, added);
                const int t = song.freeTrack();
                ok = t >= 0 && scene >= 0 && song.addSeqTrack(t, crosspad::SeqSource::Kit, (uint8_t)scene, kit);
                if (ok) song.armed = (int8_t)t;
            }
            if (ok) se->commit();
        }
        else if (strncmp(arg, "PITCHTRACK ", 11) == 0) {
            /* A Pitch track on the lowest free track, armed, with a scene of
             * its own, playing the instrument named (an instrument.json, or a
             * pitched kit.json, from the card's root) -- Arrange's KITS >
             * PITCH with the instrument already picked. Beside two kit tracks
             * since core 2.6 (route 2). */
            auto* sq = crosspad::getPlatformServices().sequencer;
            const bool added = sq && sq->addScene();
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                const int scene = song.sceneForNewSeq(sq ? sq->getSceneCount() : 1, added);
                const int t = song.freeTrack();
                ok = t >= 0 && scene >= 0 && song.addSeqTrack(t, crosspad::SeqSource::Pitch, (uint8_t)scene);
                if (ok) {
                    song.armed = (int8_t)t;
                    song.instrument = arg + 11;
                }
            }
            if (ok) {
                se->commit();
                /* What a pick in Pitch Play leaves behind: Arrange keeps this
                 * instrument loaded for the track (its sound keeper). */
                crosspad::InstrumentPlayState& ps = crosspad::instrumentPlayState();
                ps.instrumentPath = arg + 11;
                crosspad::instrumentPlayStatePublished();
            }
        }
        else if (sscanf(arg, "DELTRACK %d", &a) == 1) {
            /* DEL on a track's header, confirmed: its clips leave the song,
             * the tracks under it move up. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                ok = a >= 0 && a < crosspad::AudioSong::kTracks && song.tracks[a].inUse;
                if (ok) song.removeTrack(a);
            }
            if (ok) se->commit();
        }
        else if (sscanf(arg, "DUP %d %d", &a, &b) == 2 || sscanf(arg, "DELBLOCK %d %d", &a, &b) == 2 ||
                 sscanf(arg, "MOVE %d %d %d", &a, &b, &c3) == 3) {
            /* A block's DUP (a copy straight after it), DEL, or MOVE to bar c3:
             * block b of track a, as the keys a block opens in Arrange. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                ok = a >= 0 && a < crosspad::AudioSong::kTracks && b >= 0 &&
                     b < (int)song.tracks[a].clips.size() && c3 >= 0;
                if (ok) {
                    auto& cs = song.tracks[a].clips;
                    if (arg[1] == 'U') {
                        crosspad::SongClip copy = cs[(size_t)b];
                        copy.bar = cs[(size_t)b].bar + cs[(size_t)b].bars;
                        cs.push_back(copy);
                    } else if (arg[0] == 'D') {
                        cs.erase(cs.begin() + b);
                    } else {
                        cs[(size_t)b].bar = (uint32_t)c3;
                    }
                    song.sortTrack(a);
                }
            }
            if (ok) se->commit();
        }
        else if (sscanf(arg, "PASTE %d %d %d", &a, &b, &c3) == 3) {
            /* EDIT's copy of bars [a, b) and paste at bar c3: what was there
             * moves on, cut where the paste starts if it crossed it. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                ok = a >= 0 && b > a && c3 >= 0;
                if (ok) {
                    song.seqBlocksExplicit(se->playEndBars());
                    const uint32_t spb = se->framesPerBar();
                    const auto part = song.copyRange((uint32_t)a, (uint32_t)b, spb);
                    song.insertBars((uint32_t)c3, (uint32_t)(b - a), spb);
                    song.pasteRange(part, (uint32_t)c3);
                }
            }
            if (ok) se->commit();
        }
        else if (int at = 0; sscanf(arg, "TRACKKIT %d %n", &a, &at) == 1 && at > 0 && arg[at]) {
            /* A kit track's kit picked again in Arrange's kit loader: the
             * song's too while it is the live one. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                ok = song.seqRoute(a) == 0;
                if (ok) song.setTrackKit(a, arg + at);
            }
            if (ok) se->commit();
        }
        else if (sscanf(arg, "BOUNCE %d %d", &a, &b) == 2)  ok = se->bounceScene((uint8_t)a, (uint8_t)b);
        else if (sscanf(arg, "PLAY %d", &a) == 1)           ok = se->play((uint32_t)a);
        else if (strncmp(arg, "PLAY", 4) == 0)              ok = se->play(0);
        else if (strncmp(arg, "STOP", 4) == 0)              se->stop();
        else if (strncmp(arg, "CANCEL", 6) == 0)            se->cancelBounce();
        else if (sscanf(arg, "REC %d %d", &a, &b) >= 1)     ok = se->recordSeq((uint32_t)a, crosspad::SyncTake::Record, (uint8_t)b);
        else if (sscanf(arg, "DUB %d %d", &a, &b) >= 1)     ok = se->recordSeq((uint32_t)a, crosspad::SyncTake::Overdub, (uint8_t)b);
        else if (strncmp(arg, "PUNCH ", 6) == 0)            ok = se->seqPunch(arg[6] == 'R' ? crosspad::SyncTake::Record
                                                                          : arg[6] == 'D' ? crosspad::SyncTake::Overdub
                                                                                          : crosspad::SyncTake::None);
        else if (sscanf(arg, "TEMPO %d", &a) == 1)         ok = se->requestTempo((float)a);
        else if (sscanf(arg, "MERGE %d %d %d", &a, &b, &c3) == 3) ok = se->requestMerge((uint32_t)a, (uint32_t)b, (uint32_t)c3);
        else if (strncmp(arg, "IMPORT ", 7) == 0) {
            EXT_RAM_BSS_ATTR static char src[128];
            unsigned ia = 0, ib = 0, it = 0, ibar = 0, ilead = 0;
            ok = sscanf(arg + 7, "%127s %u %u %u %u %u", src, &ia, &ib, &it, &ibar, &ilead) == 6 &&
                 se->requestImport(src, ia, ib, (uint8_t)it, ibar, ilead);
        }
        else if (sscanf(arg, "REGION %d %d", &a, &b) == 2) {
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().loopStart = a > 0 ? (uint32_t)a : 0;
                se->song().loopEnd = b > 0 ? (uint32_t)b : 0;
            }
            se->commit();
        }
        else if (sscanf(arg, "CUT %d %d", &a, &b) == 2) {
            /* EDIT's "Cut track T at bar B": the block of track a running
             * across bar b in two there (ok=0 when none does). */
            int second = -1;
            if (a >= 0 && a < crosspad::AudioSong::kTracks && b >= 0) {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                song.seqBlocksExplicit(se->playEndBars());
                second = song.cutClip(a, (uint32_t)b, se->framesPerBar());
            }
            ok = second >= 0;
            if (ok) se->commit();
        }
        else if (strcmp(arg, "DUPREGION") == 0) {
            /* EDIT's "Duplicate the region x2": the region's bars again right
             * after it, every track, and the region twice as long. */
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                auto& song = se->song();
                ok = song.hasRegion();
                if (ok) {
                    const uint32_t w = song.loopEnd - song.loopStart;
                    song.seqBlocksExplicit(se->playEndBars());
                    const uint32_t spb = se->framesPerBar();
                    const auto part = song.copyRange(song.loopStart, song.loopEnd, spb);
                    song.insertBars(song.loopEnd, w, spb);
                    song.pasteRange(part, song.loopEnd);
                    song.loopEnd += w;
                }
            }
            if (ok) se->commit();
        }
        else if (strncmp(arg, "RENAME ", 7) == 0)           ok = se->requestRename(arg + 7);
        else if (strcmp(arg, "DELETE") == 0)                 ok = se->requestDelete();   // the open project, clips and all
        else if (strcmp(arg, "EXPORT") == 0)                 ok = se->requestExportLiveSet();
        else if (strcmp(arg, "EXPORTWAV") == 0)              ok = se->requestExportWav();
        else if (strcmp(arg, "REPORT") == 0) {
            EXT_RAM_BSS_ATTR static char row[200];
            for (const auto& ln : se->editReport()) {
                snprintf(row, sizeof(row), "REPORT: %s\r\n", ln.c_str());
                cdc_reply(row);
            }
        }
        else if (sscanf(arg, "SYNC %d", &a) == 1) {
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().syncSeq = a != 0;
            }
            se->commit();
        }
        else if (sscanf(arg, "LEN %d", &a) == 1) {          // 0 = to the last block
            {
                std::lock_guard<std::recursive_mutex> l(se->songMutex());
                se->song().length = a > 0 ? (uint32_t)a : 0;
            }
            se->commit();
        }
        else if (*arg) { cdc_reply("ERR ASONG OPEN name|BOUNCE scene track|PLAY [bar]|STOP|CANCEL|LEN bars|SYNC 0|1|TEMPO bpm|RENAME name|REGION a b|MERGE mask from to|IMPORT path a b track bar lead|EXPORT|EXPORTWAV|REPORT|REC bar [beats]|DUB bar [beats]|PUNCH R|D|N|BOUNCESEQ track|VOL track pct|MUTE track 0|1|CUT track bar|DUPREGION|ARM track|SEQTRACK [kitjson]|DELTRACK t|DUP t b|DELBLOCK t b|MOVE t b bar|PASTE a b at|TRACKKIT t kitjson\r\n"); return true; }

        const crosspad::BounceStatus bs = se->bounceStatus();
        std::lock_guard<std::recursive_mutex> songLock(se->songMutex());
        const crosspad::AudioSong& song = se->song();
        /* Static and integer-only: this runs on the 5 kB main task, where a
         * 300-byte frame plus printf's float path is enough to overflow it. */
        EXT_RAM_BSS_ATTR static char reply[448];   /* PSRAM: the bounce fields stand before file=/err= */
        constexpr int kTail = 3;   // "\r\n" and the terminator always fit
        const int posCenti = (int)(se->positionBars() * 100.0);
        /* Names last: a project may have spaces and an error always does, so
         * everything before them is key=value a host splits on spaces, and
         * "project=" / " err=" take the rest of the line. */
        int n = snprintf(reply, sizeof(reply) - kTail,
                         "ASONG: ok=%d open=%d edit=%d eprog=%u rgn=%u-%u bpm=%d len=%u sync=%d play=%d pos=%d.%02d under=%u "
                         "openfail=%u bounce=%d done=%u total=%u clips=",
                         ok ? 1 : 0, (int)se->openState(), (int)se->editState(), (unsigned)se->editProgress(),
                         (unsigned)song.loopStart, (unsigned)song.loopEnd, (int)lroundf(song.bpm),
                         (unsigned)song.playEnd(), song.syncSeq ? 1 : 0, se->playing() ? 1 : 0,
                         posCenti / 100, posCenti % 100, (unsigned)se->underruns(),
                         (unsigned)se->openFailures(), (int)bs.state,
                         (unsigned)bs.framesDone, (unsigned)bs.framesTotal);
        /* snprintf answers what it would have written: a long name or error
         * makes that more than the buffer, and every offset after it must be
         * clamped or the next write lands past the end. */
        n = std::min(n, (int)sizeof(reply) - kTail);
        for (int t = 0; t < std::max(song.usedTracks(), crosspad::AudioSong::kTracksV5); ++t) {
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%u", t ? "/" : "",
                          (unsigned)song.tracks[t].clips.size());
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        /* How the card kept up with the last bounce: what it could take is
         * 4096000 / chunk_avg_us kB/s; drain_kbps is the bus's own rate
         * while it keeps up. io_fb: pieces through stdio, since boot. */
        n += snprintf(reply + n, sizeof(reply) - kTail - n,
                      " chunks=%u chunk_avg_us=%u chunk_max_us=%u drain_kbps=%u io_fb=%u pieces=%u/%u",
                      (unsigned)bs.chunks, (unsigned)bs.chunkAvgUs, (unsigned)bs.chunkMaxUs,
                      (unsigned)bs.drainKBps, (unsigned)crosspad::song_engine_io_fallbacks(),
                      (unsigned)bs.piecesDone, (unsigned)bs.pieces);
        n = std::min(n, (int)sizeof(reply) - kTail);
        n += snprintf(reply + n, sizeof(reply) - kTail - n, " file=%s project=%s%s%s",
                      bs.file.empty() ? "-" : bs.file.c_str(), song.name.c_str(),
                      bs.error.empty() ? "" : " err=", bs.error.c_str());
        n = std::min(n, (int)sizeof(reply) - kTail);
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
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
    /* STOOL [IN MIC|LINE|PADS|USB|REC|SYNC bar [beats]|DIR [path]|STOP|RELEASE|LIST|OV path [a b]|ONSETS sens|EQ n|
     *        PLAY path a b loop|SLICE path a b|ATK ms|PITCH st|PSTOP|SAVE path a b|EXPORT name|HANDOFF path a b|
     *        FX path out st hp lp rev [keep]] --
     * the sample tools (Recorder, Slicer). SYNC is a take with the song from
     * that bar (0-based), after beats of count-in; song=/wait= say so. DIR
     * lists a folder of the card (none: the recordings); dir=/n=/folders= and
     * the names after "names=". Slices from ONSETS/EQ are kept here
     * for EXPORT; ATK and PITCH (semitones, varispeed) are what EXPORT and
     * SLICE (a slice's audition) use. FX's keep=1 transposes and keeps the
     * length (the Recorder's KEEP), 0 or none varispeeds. Bare STOOL reports;
     * names last, like ASONG. */
    /* HEAP_CHECK [WATCH 0|1] -- walk every heap and say whether its blocks are
     * still whole. Light poisoning only notices a broken block when something
     * frees it, which can be minutes and two apps away from the write that
     * broke it: that is how an overrun somewhere else comes out as an assert
     * inside lv_label_set_text_fmt. WATCH 1 puts the walk on a worker and logs
     * the first failure, which pins the corruption to whatever was happening
     * seconds earlier instead of to the innocent free that noticed. */
    if (strncmp(cmd, "HEAP_CHECK", 10) == 0) {
        int on = -1;
        if (sscanf(cmd, "HEAP_CHECK WATCH %d", &on) == 1) heap_watch_set(on != 0);
        const bool ok = heap_caps_check_integrity_all(true);
        EXT_RAM_BSS_ATTR static char reply[96];
        snprintf(reply, sizeof(reply), "HEAPCHK: ok=%d watch=%d\r\n",
                 ok ? 1 : 0, heap_watch_on() ? 1 : 0);
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "STOOL", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ')) {
        auto* st = crosspad::getPlatformServices().sampleTools;
        if (!st) { cdc_reply("ERR STOOL no sample tools\r\n"); return true; }
        static std::vector<uint32_t> s_slices;
        static uint32_t s_sliceEnd = 0;
        static std::string s_sliceFile;
        static uint16_t s_sliceAtk = 0;
        static int s_sliceSemis = 0;
        EXT_RAM_BSS_ATTR static char path[128];
        const char* arg = cmd + 5;
        while (*arg == ' ') ++arg;
        unsigned a = 0, b = 0, c = 0;
        bool ok = true;
        if      (strcmp(arg, "IN MIC") == 0)   ok = st->selectInput(crosspad::RecInput::Mic);
        else if (strcmp(arg, "IN LINE") == 0)  ok = st->selectInput(crosspad::RecInput::Line);
        else if (strcmp(arg, "IN PADS") == 0)  ok = st->selectInput(crosspad::RecInput::Pads);
        else if (strcmp(arg, "IN USB") == 0)   ok = st->selectInput(crosspad::RecInput::Usb);
        else if (strcmp(arg, "REC") == 0)      ok = st->startRecording();
        else if (sscanf(arg, "SYNC %u %u", &a, &b) == 2) ok = st->startRecordingWithSong(a, (uint8_t)b);
        else if (sscanf(arg, "SYNC %u", &a) == 1) ok = st->startRecordingWithSong(a);
        else if (strncmp(arg, "DIR", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
            const char* d = arg + 3;
            while (*d == ' ') ++d;
            ok = st->requestFolder(d);
        }
        else if (strcmp(arg, "STOP") == 0)     st->stopRecording();
        else if (strcmp(arg, "RELEASE") == 0)  st->releaseInput();
        else if (strcmp(arg, "LIST") == 0)     ok = st->requestRecordings();
        else if (strcmp(arg, "PSTOP") == 0)    st->stopPreview();
        else if (sscanf(arg, "OV %127s %u %u", path, &a, &b) >= 1) ok = st->requestOverview(path, a, b, 300);
        else if (sscanf(arg, "PLAY %127s %u %u %u", path, &a, &b, &c) == 4) ok = st->preview(path, a, b, c != 0);
        else if (sscanf(arg, "SLICE %127s %u %u", path, &a, &b) == 3) {
            crosspad::PreviewSound snd;
            snd.slice = true;
            snd.attackMs = s_sliceAtk;
            snd.semitones = (int8_t)s_sliceSemis;
            ok = st->previewAs(path, a, b, false, snd);
        }
        else if (sscanf(arg, "ATK %u", &a) == 1) s_sliceAtk = (uint16_t)a;
        else if (sscanf(arg, "PITCH %d", &s_sliceSemis) == 1) {}
        else if (strncmp(arg, "FX ", 3) == 0) {
            /* A sampler pad's copy: path out semitones hp lp reverse(0|1) [keep(0|1)]. */
            EXT_RAM_BSS_ATTR static char out[128];
            int semis = 0;
            unsigned hp = 0, lp = 0, rev = 0, keep = 0;
            crosspad::SampleFx fx;
            ok = sscanf(arg + 3, "%127s %127s %d %u %u %u %u", path, out, &semis, &hp, &lp, &rev, &keep) >= 6;
            fx.semitones = semis;
            fx.hpHz = (uint16_t)hp;
            fx.lpHz = (uint16_t)lp;
            fx.reverse = rev != 0;
            fx.keepLength = keep != 0;
            ok = ok && st->requestRendered(path, out, fx);
        }
        else if (sscanf(arg, "SAVE %127s %u %u", path, &a, &b) == 3) ok = st->requestSaveRegion(path, a, b);
        else if (sscanf(arg, "HANDOFF %127s %u %u", path, &a, &b) == 3) st->setHandoff(path, a, b);
        else if (sscanf(arg, "ONSETS %u", &a) == 1) {
            const crosspad::WaveOverview ov = st->overview();
            s_slices = crosspad::SliceMath::onsets(ov.envelope, ov.hop, ov.start, ov.start, ov.end,
                                                   (int)a, ov.sampleRate / 20);
            s_sliceEnd = ov.end;
            s_sliceFile = ov.file;
        }
        else if (sscanf(arg, "EQ %u", &a) == 1) {
            const crosspad::WaveOverview ov = st->overview();
            s_slices = crosspad::SliceMath::equal(ov.start, ov.end, (uint8_t)a);
            s_sliceEnd = ov.end;
            s_sliceFile = ov.file;
        }
        else if (strncmp(arg, "EXPORT ", 7) == 0) {
            crosspad::SliceKit kit;
            kit.source = s_sliceFile;
            kit.name = arg + 7;
            kit.starts = s_slices;
            kit.end = s_sliceEnd;
            kit.attackMs = s_sliceAtk;
            kit.semitones = (int8_t)s_sliceSemis;
            ok = st->requestExportKit(kit);
        }
        else if (*arg) { cdc_reply("ERR STOOL IN MIC|LINE|PADS|USB|REC|SYNC bar [beats]|DIR [path]|STOP|RELEASE|LIST|OV path [a b]|ONSETS sens|EQ n|PLAY path a b loop|SLICE path a b|ATK ms|PITCH st|PSTOP|SAVE path a b|EXPORT name|HANDOFF path a b|FX path out st hp lp rev [keep]\r\n"); return true; }

        float pl = 0, pr = 0;
        st->peaks(pl, pr);
        const crosspad::RecStatus rs = st->recStatus();
        const crosspad::WaveOverview ov = st->overview();
        const std::vector<std::string> recs = st->recordings();
        EXT_RAM_BSS_ATTR static char reply[480];
        constexpr int kTail = 3;
        int n = snprintf(reply, sizeof(reply) - kTail,
                         "STOOL: ok=%d in=%d pk=%d/%d rec=%d song=%d wait=%d from=%u frames=%u sr=%u job=%d prog=%u prev=%d at=%u "
                         "ov=%u..%u/%u cols=%u hops=%u recs=%u slices=%u:",
                         ok ? 1 : 0, (int)st->input(), (int)(pl * 1000), (int)(pr * 1000),
                         rs.recording ? 1 : 0, rs.withSong ? 1 : 0, rs.waiting ? 1 : 0, (unsigned)rs.fromBar,
                         (unsigned)rs.frames, (unsigned)rs.sampleRate,
                         (int)st->job(), (unsigned)st->jobProgress(), st->previewing() ? 1 : 0,
                         (unsigned)st->previewFrame(), (unsigned)ov.start, (unsigned)ov.end,
                         (unsigned)ov.frames, (unsigned)ov.lo.size(), (unsigned)ov.envelope.size(),
                         (unsigned)recs.size(), (unsigned)s_slices.size());
        n = std::min(n, (int)sizeof(reply) - kTail);
        for (size_t i = 0; i < s_slices.size() && n < (int)sizeof(reply) - 40; ++i) {
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%u", i ? "," : "", (unsigned)s_slices[i]);
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        const std::string result = st->jobResult();
        const crosspad::CardFolder cf = st->folder();
        n += snprintf(reply + n, sizeof(reply) - kTail - n, " n=%u folders=%u more=%d file=%s err=%s result=%s first=%s dir=%s names=",
                      (unsigned)cf.size(), (unsigned)cf.folders, cf.truncated ? 1 : 0,
                      rs.file.c_str(), rs.error.c_str(), result.c_str(), recs.empty() ? "-" : recs.front().c_str(),
                      cf.path.empty() ? "-" : cf.path.c_str());
        n = std::min(n, (int)sizeof(reply) - kTail);
        for (size_t i = 0; i < cf.size() && n < (int)sizeof(reply) - 24; ++i) {
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%s%s", i ? "|" : "", cf.name(i), cf.isFolder(i) ? "/" : "");
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "AUDIO_LEVEL", 11) == 0) {
        handle_audio_level();
        return true;
    }
#ifdef HIL_HAS_MIXER
    if (strncmp(cmd, "MIX_LVL", 7) == 0) {
        handle_mix_lvl();
        return true;
    }
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
#endif
    if (strncmp(cmd, "PAD_STATS", 9) == 0) {
        handle_pad_stats();
        return true;
    }
    if (sscanf(cmd, "ENC_ROTATE %d", &a) == 1 && a != 0) {
        s_enc_delta = a > 16 ? 16 : (a < -16 ? -16 : a);
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_enc_rotate, nullptr);
        return true;
    }
    if (sscanf(cmd, "ENC_PRESS %d", &a) == 1 && a > 0) {
        s_enc_press_ms = (uint32_t)(a > 5000 ? 5000 : a);
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_enc_press, nullptr);
        return true;
    }
    if (strncmp(cmd, "ENC_PRESS", 9) == 0) {
        s_enc_press_ms = 80;
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_enc_press, nullptr);
        return true;
    }
    if (strncmp(cmd, "APP_SELF_CLOSE", 14) == 0) {
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_self_close, nullptr);
        return true;
    }
    if (strncmp(cmd, "APP_DESTROY", 11) == 0) {
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_destroy, nullptr);
        return true;
    }
    if (strncmp(cmd, "APP_STOP", 8) == 0) {
        if (!launcher_up()) {
            cdc_reply("ERR no_launcher\r\n");
            return true;
        }
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_app_stop, nullptr);
        return true;
    }
    if (sscanf(cmd, "PAD_PRESS %d %d", &a, &b) >= 1 && a >= 0 && a < 16) {
        const uint8_t vel = (b >= 0 && b <= 127) ? (uint8_t)b : 127;
        pad_inject_reply_first(HilPadInject::Press, (uint8_t)a, vel, cdc_reply);
        return true;
    }
    if (sscanf(cmd, "PAD_RELEASE %d", &a) == 1 && a >= 0 && a < 16) {
        pad_inject_reply_first(HilPadInject::Release, (uint8_t)a, 0, cdc_reply);
        return true;
    }
    if (sscanf(cmd, "PAD_PRESSURE %d %d", &a, &b) == 2 &&
        a >= 0 && a < 16 && b >= 0 && b <= 255) {
        pad_inject_reply_first(HilPadInject::Pressure, (uint8_t)a, (uint8_t)b, cdc_reply);
        return true;
    }
    if (sscanf(cmd, "PAD_PAN %d %d", &a, &b) == 2 &&
        a >= 0 && a < 16 && b >= 0 && b <= 127) {
        /* Left/right balance of a held pad as the STM would stream it:
         * 0 left .. 64 centre .. 127 right, to the active pad logic. */
        pad_inject_reply_first(HilPadInject::Pan, (uint8_t)a, (uint8_t)b, cdc_reply);
        return true;
    }
    if (strncmp(cmd, "KIT_LIST", 8) == 0) {
        handle_kit_list();
        return true;
    }
    if (strncmp(cmd, "KIT_STATUS", 10) == 0) {
        handle_kit_status();
        return true;
    }
    if (strncmp(cmd, "SETTINGS_EXPORT", 15) == 0 || strncmp(cmd, "SETTINGS_IMPORT", 15) == 0) {
        char path[96] = {};
        const char *arg = cmd + 15;
        while (*arg == ' ') arg++;
        size_t n = 0;
        while (n < sizeof(path) - 1 && arg[n] && arg[n] != '\r' && arg[n] != '\n') {
            path[n] = arg[n];
            ++n;
        }
        if (!settings_backup_start(cmd[9] == 'E' ? 'e' : 'i', path)) {
            cdc_reply(s_settings_busy.load() ? "ERR busy\r\n" : "ERR path\r\n");
            return true;
        }
        char out[128];
        snprintf(out, sizeof(out), "OK %s\r\n", s_settings_path);
        cdc_reply(out);   /* reply first */
        lv_async_call_locked(async_settings_backup, nullptr);
        return true;
    }
    if (strncmp(cmd, "SETTINGS_STATUS", 15) == 0) {
        handle_settings_status();
        return true;
    }
    if (strncmp(cmd, "KIT_SWAP_LOG", 12) == 0) {
        handle_kit_swap_log();
        return true;
    }
    if (strncmp(cmd, "WAVE_STATUS", 11) == 0) {
        handle_wave_status();
        return true;
    }
    if (strncmp(cmd, "SMPL_PACE", 9) == 0) {
        /* SMPL_PACE [ms]: how long the kit loader and the other bulk readers
         * wait for a native streaming pass to end; 0 = they do not give way.
         * Until reboot; the A/B for it. */
        extern std::atomic<uint32_t> g_port_pace_ms;   /* sample_player_port.cpp */
        if (sscanf(cmd + 9, " %d", &a) == 1 && a >= 0 && a <= 1000) g_port_pace_ms.store((uint32_t)a);
        char out[32];
        snprintf(out, sizeof(out), "SMPLPACE: ms=%u\r\n", (unsigned)g_port_pace_ms.load());
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "SMPL_STALL", 10) == 0) {
        /* SMPL_STALL [ON|OFF]: the slowest native note-on and STM pad path,
         * and who had the CPU meanwhile. */
        EXT_RAM_BSS_ATTR static char out[320];
        const char *arg = cmd + 10;
        while (*arg == ' ') arg++;
        native_sampler_stall_report(out, sizeof(out),
                                    strncmp(arg, "ON", 2) == 0 ? 1 : strncmp(arg, "OFF", 3) == 0 ? 0 : -1);
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "SMPL_LAST", 9) == 0) {
        handle_smpl_last();
        return true;
    }
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (strncmp(cmd, "SMPL_POOL", 9) == 0) {
        /* No prebuilt streamer in this image; the native engine's pool is in
         * SMPL_ENGINE (pool_blocks=, norings=). */
        cdc_reply("ERR only_native\r\n");
        return true;
    }
#endif
    if (sscanf(cmd, "KIT_LOAD %d", &a) == 1 && a >= 0) {
        if (!g_kit_scan_done.load()) {
            /* getKitCount() below reads the vector the scan is growing, and the
             * id a caller quotes came from a listing that is not final yet. */
            cdc_reply("ERR scanning\r\n");
            return true;
        }
        if (!crosspad::control::kitIdLoadable(a)) {
            cdc_reply("ERR bad kit id\r\n");
            return true;
        }
        s_pending_kit = a;
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_kit_load, nullptr);
        return true;
    }
    /* KIT_DELETE id name -- a kit of the user's own (USER: the Slicer's)
     * off the card and out of the list, as the kit browser's "delete this
     * kit". The name must be the id's, so an id from an old listing never
     * takes another kit; a LEGACY kit and the one playing are refused. The
     * removal runs on the LVGL thread, as the browser's; KIT_LIST says when
     * it is gone. */
    {
        EXT_RAM_BSS_ATTR static char name[64];
        if (sscanf(cmd, "KIT_DELETE %d %63[^\r\n]", &a, name) == 2) {
            auto *mgr = crosspad::getKitManager();
            const crosspad::KitInfo *kit = (mgr && g_kit_scan_done.load() && a >= 0 && a < mgr->getKitCount())
                                               ? mgr->getKit(a) : nullptr;
            const char *err = nullptr;
            if (!kit || kit->path.empty())           err = "ERR bad kit id\r\n";
            else if (kit->name != name)              err = "ERR name\r\n";
            else if (!kit->isUser())                 err = "ERR not_user\r\n";
            else if (a == mgr->getCurrentKitId())    err = "ERR playing\r\n";
            if (err) { cdc_reply(err); return true; }
            s_delete_kit = a;
            cdc_reply("OK\r\n");
            lv_async_call_locked(async_kit_delete, nullptr);
            return true;
        }
    }
    if (sscanf(cmd, "PAD_INFO %d", &a) == 1 && a >= 0 && a < 16) {
        /* What the kit model holds for a pad: a wrong stored path vs an
         * engine that refused it. */
        auto *mgr = crosspad::getKitManager();
        auto *kit = mgr ? mgr->getCurrentKit() : nullptr;
        if (!kit) {
            cdc_reply("ERR no kit\r\n");
            return true;
        }
        auto &pad = kit->pads[a];
        char reply[512];
        int n = snprintf(reply, sizeof(reply),
                         "PADINFO: idx=%d num=%u kit=%s vol=%d pan=%d mode=%u dirty=%d count=%u",
                         a, (unsigned)pad.number, kit->path.c_str(), pad.volume, pad.pan,
                         (unsigned)pad.playbackMode, pad.dirty ? 1 : 0,
                         (unsigned)pad.sampleCount);
        for (uint8_t i = 0; i < pad.sampleCount && n < (int)sizeof(reply) - 80; ++i) {
            n += snprintf(reply + n, sizeof(reply) - n, " | [%u] '%s' %u..%u",
                          (unsigned)i, pad.samples[i].sample.c_str(),
                          (unsigned)pad.samples[i].start, (unsigned)pad.samples[i].end);
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    /* PAD_MIX: the current kit's sixteen volumes, "PADMIX: kit=… max=…
     * loading=… name=…", a row per pad, "PADMIX: end count=16". PAD_MIX pad
     * vol: that pad's volume, in the kit's units (0..127, a pitched kit
     * 0..100), into the kit and straight to the engine -- the Pad Mixer
     * app's fader, not saved until KIT_SAVE. */
    if (strncmp(cmd, "PAD_MIX", 7) == 0 && (cmd[7] == 0 || cmd[7] == ' ')) {
        auto *mgr = crosspad::getKitManager();
        if (!g_kit_scan_done.load()) { cdc_reply("ERR scanning\r\n"); return true; }
        crosspad::KitInfo *kit = mgr ? mgr->getCurrentKit() : nullptr;
        if (!kit) { cdc_reply("ERR no kit\r\n"); return true; }
        const int kitId = mgr->getCurrentKitId();
        const int max = kit->engine == crosspad::KitEngine::Pitched ? 100 : 127;
        if (cmd[7] == ' ') {
            if (sscanf(cmd + 8, "%d %d", &a, &b) != 2 || a < 0 || a >= KIT_PADS || b < 0 || b > max) {
                cdc_reply("ERR args\r\n");
                return true;
            }
            if (crosspad_sampler::kit_load_task_is_loading()) { cdc_reply("ERR loading\r\n"); return true; }
            if (s_mix_kit.exchange(kitId) != kitId) {
                for (auto &p : s_mix_pending) p.store(0);   /* parked for a kit that is gone */
            }
            s_mix_pending[a].store(b + 1);
            cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
            if (!s_mix_posted.exchange(true)) lv_async_call_locked(async_pad_mix, nullptr);
            return true;
        }
        EXT_RAM_BSS_ATTR static char line[192];
        snprintf(line, sizeof(line), "PADMIX: kit=%d max=%d loading=%d name=%s\r\n", kitId, max,
                 crosspad_sampler::kit_load_task_is_loading() ? 1 : 0, kit->name.c_str());
        cdc_reply(line);
        for (int i = 0; i < KIT_PADS; ++i) {
            const crosspad::Pad &pad = kit->pads[i];
            const int pending = s_mix_kit.load() == kitId ? s_mix_pending[i].load() : 0;
            const char *sample = pad.sampleCount && !pad.samples.empty() ? pad.samples[0].sample.c_str() : "-";
            const char *slash = strrchr(sample, '/');
            snprintf(line, sizeof(line), "PADMIX: pad=%d vol=%d pan=%d sample=%s\r\n", i,
                     pending ? pending - 1 : pad.volume, pad.pan, slash ? slash + 1 : sample);
            cdc_reply(line);
        }
        cdc_reply("PADMIX: end count=16\r\n");
        return true;
    }
    /* KIT_SAVE: the current kit, pads and all, into its kit.json -- the
     * Sampler's save, on the LVGL thread. OK = queued; KIT_SAVE_STATUS says
     * "KITSAVE: busy=0|1 ok=-1|0|1 kit=N" once it is written. */
    if (strcmp(cmd, "KIT_SAVE_STATUS") == 0) {
        char reply[64];
        snprintf(reply, sizeof(reply), "KITSAVE: busy=%d ok=%d kit=%d\r\n", s_kit_save_busy.load() ? 1 : 0,
                 s_kit_save_ok.load(), s_kit_save_kit.load());
        cdc_reply(reply);
        return true;
    }
    if (strcmp(cmd, "KIT_SAVE") == 0) {
        auto *mgr = crosspad::getKitManager();
        if (!g_kit_scan_done.load()) { cdc_reply("ERR scanning\r\n"); return true; }
        if (!mgr || !mgr->getCurrentKit()) { cdc_reply("ERR no kit\r\n"); return true; }
        if (crosspad_sampler::kit_load_task_is_loading()) { cdc_reply("ERR loading\r\n"); return true; }
        if (s_kit_save_busy.exchange(true)) { cdc_reply("ERR busy\r\n"); return true; }
        s_kit_save_kit.store(mgr->getCurrentKitId());
        s_kit_save_ok.store(-1);
        cdc_reply("OK\r\n");
        lv_async_call_locked(async_kit_save, nullptr);
        return true;
    }
    if (strncmp(cmd, "ARENA", 5) == 0) {
        /* The two sample engines share one PSRAM block, so MEM's PSRAM figures
         * no longer say who holds what — everything inside the arena looks
         * allocated to the general heap whichever engine is up. This is the
         * only view of it. */
        /* native: the engine holds a kit, or (without the prebuilt streamer)
         * the arena is simply its own and empty until the next take. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        const char *owner = marcel_engine_running() ? "marcel"
                          : crosspad::pitchedKitActive() ? "pitched"
                          : native_sampler_active() ? "native" : "none";
#else
        const char *owner = crosspad::pitchedKitActive() ? "pitched" : "native";
#endif
        char reply[80];
        snprintf(reply, sizeof(reply), "ARENA: cap_kb=%u used_kb=%u owner=%s\r\n",
                 (unsigned)(engine_arena_capacity() / 1024),
                 (unsigned)(engine_arena_used() / 1024), owner);
        cdc_reply(reply);
        return true;
    }
    /* The instrument host (instrument_host.h), driven without Pitch Play's
     * screen: INST_LOAD <logical wav> builds a one-zone instrument, as picking
     * a sample from its list does; INST_STATUS says where the host is and how
     * long its last request took; INST_RELEASE hands the engine back. */
    if (strncmp(cmd, "INST_LOAD ", 10) == 0) {
        auto* h = crosspad::getPlatformServices().instrumentHost;
        /* On the heap: a KitInfo is over a kilobyte, and this frame is under every verb. */
        std::unique_ptr<crosspad::KitInfo> spec(new (std::nothrow) crosspad::KitInfo());
        if (!spec) { cdc_reply("INST: ERR no_mem\r\n"); return true; }
        spec->name = "hil";
        spec->engine = crosspad::KitEngine::Pitched;
        crosspad::InstrumentZoneSpec z;
        z.sample = cmd + 10;
        spec->instrument.zones.push_back(z);
        cdc_reply(h && h->requestLoad(*spec) ? "INST: OK\r\n" : "INST: ERR no_host\r\n");
        return true;
    }
    if (strncmp(cmd, "INST_RELEASE", 12) == 0) {
        auto* h = crosspad::getPlatformServices().instrumentHost;
        if (h) h->release();
        cdc_reply(h ? "INST: OK\r\n" : "INST: ERR no_host\r\n");
        return true;
    }
    if (strncmp(cmd, "INST_STATUS", 11) == 0) {
        auto* h = crosspad::getPlatformServices().instrumentHost;
        if (!h) { cdc_reply("INST: ERR no_host\r\n"); return true; }
        static const char* kStates[] = {"idle", "busy", "ready", "failed"};
        std::string msg = h->message();
        for (auto& c : msg) if (c == ' ') c = '_';
        EXT_RAM_BSS_ATTR static char reply[192];
        snprintf(reply, sizeof(reply), "INST: state=%s owned=%d beside=%d last_ms=%u msg=%s\r\n",
                 kStates[(int)h->state()], instrument_host_owns_engine() ? 1 : 0,
                 instrument_host_beside() ? 1 : 0,
                 (unsigned)instrument_host_last_ms(), msg.empty() ? "-" : msg.c_str());
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "PITCHED_STATUS", 14) == 0) {
        /* rt_max_us resets on read, like SMPL_PEAK's — a scenario samples it
         * per phase and wants the worst block of that phase, not of the boot.
         * Static, as the reply: the CDC loop's stack is not the place for the
         * status and its roots. */
        EXT_RAM_BSS_ATTR static crosspad::PitchedKitStatus st;
        crosspad::pitchedKitGetStatus(st);
        /* voices= is what is in force now -- the editor re-applies it -- cap=
         * the board's ceiling, shed= the steals an overloaded render forced
         * below it. The error text is already free of spaces (PitchedKitPort
         * sanitises it), so it stays one field. */
        const HilPitchedBoard board{
            crosspad::pitchedSampleRtMaxUs().exchange(0), st.lastError,
            audio_rt_busy_max(AUDIO_RT_BUSY_PITCHED, nullptr), g_rt_overruns.load(), st.kitMaxVoices};
        EXT_RAM_BSS_ATTR static char reply[kHilPitchedLineChars];
        hil_text_pitched_status(reply, sizeof(reply), st, board);
        cdc_reply(reply);
        return true;
    }
    if (sscanf(cmd, "PITCHED_CAP %d", &a) == 1 && a >= 1 && a <= 28) {
        /* Bench: the pitched voice cap (PitchedSampleNode::kVoiceCap), from
         * the next setParams on -- a load, or an edit in the kit editor.
         * Until the next restart. */
        if (auto *inst = crosspad::getPlatformServices().pitchedInstrument) inst->setVoiceCap((uint8_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
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
    if (sd_bench_verb(cmd, cdc_reply)) return true;
    /* SD_CACHE [0|1] -- the card layer's cache of single sectors (directory
     * and FAT reads, sd_sector_io.cpp), for an A/B; emptied either way. */
    if (strncmp(cmd, "SD_CACHE", 8) == 0 && (cmd[8] == 0 || cmd[8] == ' ')) {
        if (cmd[8] == ' ') sd_cache_enable(cmd[9] == '1');
        cdc_reply(sd_cache_on() ? "SDCACHE: on=1\r\n" : "SDCACHE: on=0\r\n");
        return true;
    }
    if (strcmp(cmd, "SD_TRACE") == 0) {
        /* Every card transaction since the last call, by task (sd_sector_io.cpp). */
        EXT_RAM_BSS_ATTR static char reply[1024];
        sd_trace_cmd(reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "SD_BOUNCE ", 10) == 0) {
        /* The sector bounce off (0) or on (1), for an A/B. */
        sd_bounce_enable(cmd[10] == '1');
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "PITCHED_BENCH", 13) == 0) {
        int iters = kBenchDefaultIters;
        sscanf(cmd, "PITCHED_BENCH %d", &iters);
        if (iters < kBenchMinIters) iters = kBenchMinIters;
        if (iters > kBenchMaxIters) iters = kBenchMaxIters;
        if (s_bench_busy) { cdc_reply("BENCH: ERR busy\r\n"); return true; }
        s_bench_iters = iters;
        s_bench_busy = true;
        /* Stack from PSRAM: 8 kB of internal RAM is more than the largest
         * free internal block once a kit is loaded, and this task never runs
         * with the flash cache disabled. */
        if (xTaskCreatePinnedToCoreWithCaps(pitched_bench_task, "pitched_bench", 8192,
                                            nullptr, 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            s_bench_busy = false;
            cdc_reply("BENCH: ERR no task\r\n");
        }
        return true;
    }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (sscanf(cmd, "SMPL_POLY %d", &a) == 1 && a >= 0 && a <= 17 && marcel_engine_running()) {
        /* The prebuilt streamer holds the kit: its voice cap
         * (SampleStreamPlayer_SetMaxPolyphony()). Bench knob: it holds over
         * the port's cap of 16, across kit loads, until SMPL_POLY 0. Above
         * the native engine's SMPL_POLY, which would match first. */
        sample_player_port_set_poly_cap((uint8_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
#endif
    if (strncmp(cmd, "SMPL_POLY", 9) == 0) {
        /* The voice cap, so a polyphony sweep can see past the default 16 --
         * the engine's own ceiling is higher. `SMPL_POLY` alone reports it. */
        /* `SMPL_POLY <total> [streaming]` -- the second is the ceiling on
         * voices being fed off the card, which is the one that limits long
         * samples and has to be movable to find out why. */
        int tot = 0, strm = 0;
        char reply[80];
        const int n = sscanf(cmd + 9, "%d %d", &tot, &strm);
        if (n >= 1) {
            /* `SMPL_POLY 0`: both back to the engine's defaults (16/10), the
             * way 0 meant "back to the kit's budget" on the prebuilt one. A
             * take resets them too: a raise is for the kit it was made on. */
            /* The streaming budget first: it takes the loader's lock, and a
             * take holding it answers busy before anything has changed. */
            const uint8_t gotS = (n >= 2)   ? native_sampler_set_max_streaming((uint8_t)strm)
                                 : tot == 0 ? native_sampler_set_max_streaming(0)
                                            : native_sampler_max_streaming();
            if (gotS == NATIVE_SAMPLER_BUSY) { cdc_reply("ERR busy\r\n"); return true; }
            const uint8_t gotT = native_sampler_set_max_voices((uint8_t)tot);
            snprintf(reply, sizeof(reply), "SMPLPOLY: max_voices=%u max_streaming=%u\r\n",
                     (unsigned)gotT, (unsigned)gotS);
        } else {
            snprintf(reply, sizeof(reply), "SMPLPOLY: max_voices=%u max_streaming=%u\r\n",
                     (unsigned)native_sampler_max_voices(),
                     (unsigned)native_sampler_max_streaming());
        }
        cdc_reply(reply);
        return true;
    }
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
    if (strncmp(cmd, "SMPL_PEAK", 9) == 0) {
        /* Peak rendered since the last read (int16 scale) plus free WAV
         * slots: a silent engine vs a bus that carried nothing out. The louder
         * of the two sample engines, so a pitched kit — where the prebuilt
         * engine is stopped and its node renders nothing — is not reported as silence.
         * Both are read-and-cleared, whichever wins. */
        int32_t nativePeak = 0;
        uint32_t nativeRtMaxUs = 0, nativeUnderruns = 0;
        native_sampler_peak_take(&nativePeak, &nativeRtMaxUs, &nativeUnderruns);
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        const int32_t marcelPeak = crosspad::marcelSamplePeak().exchange(0);
        const uint32_t marcelRtMaxUs = crosspad::marcelSampleRtMaxUs().exchange(0);
#else
        const int32_t marcelPeak = 0;
        const uint32_t marcelRtMaxUs = 0;
#endif
        const int32_t pitchedPeak = crosspad::pitchedSamplePeak().exchange(0);
        int32_t peak = marcelPeak > pitchedPeak ? marcelPeak : pitchedPeak;
        if (nativePeak > peak) peak = nativePeak;
        /* free=/poly=/rt_max_us= of the engine that holds the kit: the native
         * one when it does (poly= its voice cap), else the prebuilt streamer. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        const bool nat = native_sampler_active();
        const unsigned poly = nat ? native_sampler_max_voices() : marcel_engine_poly();
#else
        const bool nat = true;
        const unsigned poly = native_sampler_max_voices();
#endif
        const unsigned rtMax = nat ? nativeRtMaxUs : marcelRtMaxUs;
        const unsigned underruns = nat ? nativeUnderruns : g_engine_underrun.load();
        /* The library's GetLoad and GetMaxc accessors are not reported: they
         * read nothing useful here (`nan`), and on an engine
         * marcel_engine_stop() has stopped -- a board with no kit loaded or
         * on a pitched kit -- they read a null pointer.
         *
         * What is on this line is measured by the port: peak,
         * free slots, the voice cap, rt_max_us (MarcelSampleNode times
         * SampleStreamPly_Process alone), rxskip, noblock and underrun. */
        /* rt_busy_max_us: the whole render pass, I2S waits excluded (read and
         * cleared); overruns: passes longer than a block, since boot (main). */
        char reply[256];
        /* `unpaced` is the one that matters when a board "glitches": a render
         * pass whose block no output took means nothing was slowing the RT
         * loop down, and it then runs as fast as the core allows. Anything
         * but 0 is a fault, and it is read here because this verb is one of
         * the few the USB-audio profile can answer at all. */
        snprintf(reply, sizeof(reply), "SMPLPEAK: %d free=%d poly=%u rt_max_us=%u rxskip=%u noblock=%u underrun=%u unpaced=%u"
                 " rt_busy_max_us=%u overruns=%u\r\n",
                 (int)peak, hil_free_slots(), poly, rtMax,
                 (unsigned)audio_rx_skips(),
                 (unsigned)g_engine_noblock.load(), underruns,
                 (unsigned)audio_rt_unpaced_passes(),
                 (unsigned)audio_rt_busy_max(AUDIO_RT_BUSY_SMPL_PEAK, nullptr), (unsigned)g_rt_overruns.load());
        cdc_reply(reply);
        return true;
    }
    if (sscanf(cmd, "SMPL_PITCH %d %d", &a, &b) == 2 &&
        a >= 0 && a < 16 && b >= -24 && b <= 24) {
        /* A pad's step, from the bench, through the same door the pad editor
         * uses: write the kit model and ask for that one pad back.
         *
         * It exists because a realtime step cannot be heard over a serial
         * cable and no kit on a card sets `pitch` at all -- so without it the
         * one feature this engine was built for has no way of being shown to
         * work on real hardware. On the prebuilt engine it does what it always
         * did: renders a transposed copy and reloads, which is the comparison. */
        crosspad::KitInfo* kit = crosspad::getKitManager()
                                     ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (!kit || !kit->parsed) { cdc_reply("ERR no_kit\r\n"); return true; }
        kit->pads[a].pitch = b;
        kit->pads[a].dirty = true;
        /* The editor's own path: one number where the engine can take it, a
         * pad rebuild only where it cannot. Measuring anything else would
         * measure a route the instrument does not use. */
        const bool stepped = native_sampler_set_pad_pitch((uint8_t)a, b);
        char reply[64];
        snprintf(reply, sizeof(reply), "SMPLPITCH: pad=%d semi=%d engine=%s\r\n",
                 a, b, native_sampler_active() ? "native"
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
                       : "marcel");
#else
                       : "none");
#endif
        cdc_reply(reply);   /* reply first, then the hand-off */
        if (!stepped) {
            /* The rebuild is the kit loader's (the pad is marked modified
             * above), never this task's: a pad reload reads the card, may
             * render a copy, and on a pitched kit builds the whole bank.
             * KIT_STATUS / SMPL_ENGINE say when it is done. */
            lv_async_call_locked([](void*) { crosspad_sampler::kit_load_task_start(true, -1); }, nullptr);
        }
        return true;
    }
    if (strcmp(cmd, "SMPL_LOOPS") == 0) {
        static char reply[96];
        native_sampler_loops(reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    if (strncmp(cmd, "SMPL_ENGINE", 11) == 0) {
        /* Which of the two sample engines holds the kit. They share the PSRAM
         * arena, so this is a handover, not a fan-out -- and it is a runtime
         * choice so the same kit can be heard through both on one board.
         *
         * Reloads the kit on the way in: the native engine reads every
         * sample's header and its resident head off the card, which is the
         * same work a kit load does and takes about as long. */
        const char *arg = cmd + 11;
        while (*arg == ' ') ++arg;
        if (*arg == '\0' || *arg == '\r' || *arg == '\n') {
            /* Static, and room to spare: ~440 B on a busy kit, ~500 at the
             * counters' widest, and a truncated line loses its CRLF, which the
             * host reads as no reply at all. The main task's 4 kB stack is no
             * place for it. */
            EXT_RAM_BSS_ATTR static char reply[640];
            native_sampler_status(reply, sizeof(reply));
            cdc_reply(reply);
            return true;
        }
        if (strncmp(arg, "native", 6) == 0) {
            native_sampler_set_preferred(true);
            /* Answers at once; the take runs on its own task because it reads
             * the whole kit off the card. SMPL_ENGINE with no argument says
             * where it got to. */
            if (!native_sampler_take_kit()) { cdc_reply("ERR no_kit\r\n"); return true; }
            cdc_reply("OK loading\r\n");
            return true;
        }
        if (strncmp(arg, "marcel", 6) == 0) {
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
            /* And stop preferring it, or the next kit load takes it straight
             * back and the choice looks like it did not stick. */
            native_sampler_set_preferred(false);
            native_sampler_release();
            if (!marcel_engine_running()) marcel_engine_start();
            cdc_reply("OK marcel\r\n");
#else
            /* Not built (Kconfig CROSSPAD_SAMPLER_MARCEL): the native engine
             * is the only one; SMPL_ENGINE's engines= says so too. */
            cdc_reply("ERR only_native\r\n");
#endif
            return true;
        }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        cdc_reply("ERR usage SMPL_ENGINE [native|marcel]\r\n");
#else
        cdc_reply("ERR usage SMPL_ENGINE [native]\r\n");
#endif
        return true;
    }
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (sscanf(cmd, "SMPL_BKG %d", &a) == 1 && (a == 0 || a == 1)) {
        /* No prebuilt streamer in this image. */
        cdc_reply("ERR only_native\r\n");
        return true;
    }
#endif
    if (strncmp(cmd, "SMPL_PREVIEW ", 13) == 0) {
        /* The preview voice directly: a silent preview vs a UI path that
         * never fires. */
        char clean[128];
        strncpy(clean, cmd + 13, sizeof(clean) - 1);
        clean[sizeof(clean) - 1] = '\0';
        for (char *q = clean; *q; ++q) {
            if (*q == '\r' || *q == '\n') { *q = '\0'; break; }
        }
        char *path = clean[0] == '/' ? clean + 1 : clean;
        /* The native engine's audition voice (the file browser's path), on
         * the Preview channel; SMPL_PREVIEW_STOP ends it. Started on the LVGL
         * thread, the browser's: the engine's request is a sequence-locked
         * record with one writer, and this verb was a second one. */
        if (native_sampler_active()) {
            char* fs = static_cast<char*>(malloc(160));
            if (!fs) { cdc_reply("ERR no_mem\r\n"); return true; }
            snprintf(fs, 160, "/sdcard/%s", path);
            cdc_reply("OK\r\n");   /* reply first */
            lv_async_call_locked([](void* p) {
                char* f = static_cast<char*>(p);
                if (!native_sampler_preview(f, 0, 0, 100, 0)) ESP_LOGW(TAG, "SMPL_PREVIEW %s refused", f);
                free(f);
            }, fs);
            return true;
        }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        if (!marcel_engine_running()) { cdc_reply("ERR stopped\r\n"); return true; }
        SampleStreamPlayer_Preview(path);
        cdc_reply("OK\r\n");
#else
        /* The native engine holds no kit (a pitched bank has the arena). */
        cdc_reply("ERR no_kit\r\n");
#endif
        return true;
    }
    if (strncmp(cmd, "SMPL_PREVIEW_STOP", 17) == 0) {
        /* Through the same owner as the start, so a start and a stop sent in
         * that order happen in that order. */
        cdc_reply("OK\r\n");   /* reply first */
        lv_async_call_locked([](void*) { native_sampler_preview_stop(); }, nullptr);
        return true;
    }
    if (strncmp(cmd, "STM_FW_INFO", 11) == 0) {
        // Running STM firmware vs the image bundled in LittleFS.
        stm32_link_info_t li = {};
        stm32_hal_get_link_info(&li);
        stm32_fw_image_info_t img = {};
        esp_err_t ierr = stm_fw_bundled_info(&img);
        char out[160];
        if (ierr == ESP_OK) {
            snprintf(out, sizeof(out), "STM_FW running=%u.%u/0x%04X%s board=r%u fw_pcb=r%u bundled=%u.%u/0x%04X pcb=r%u size=%u\r\n",
                     li.fw_major, li.fw_minor, li.proto_version, li.linked ? "" : "(down)",
                     li.pcb_rev, li.fw_pcb, img.major, img.minor, img.proto, img.pcb, (unsigned)img.size);
        } else {
            snprintf(out, sizeof(out), "STM_FW running=%u.%u/0x%04X%s bundled=none(%s)\r\n",
                     li.fw_major, li.fw_minor, li.proto_version, li.linked ? "" : "(down)",
                     esp_err_to_name(ierr));
        }
        cdc_reply(out);
        return true;
    }
    if (strncmp(cmd, "STM_FW_UPDATE", 13) == 0) {
        // Reflash the STM from a file (default: the bundled image). ~10 s,
        // on its own task, which sends the reply. A trailing FORCE overrides
        // the board-revision gate.
        char path[96];
        const char *arg = cmd + 13;
        while (*arg == ' ') arg++;
        strncpy(path, *arg ? arg : STM32_FW_BUNDLED_PATH, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
        for (char *q = path; *q; ++q) {
            if (*q == '\r' || *q == '\n') { *q = '\0'; break; }
        }
        bool force = false;
        char *sp = strrchr(path, ' ');
        if (sp && strcmp(sp + 1, "FORCE") == 0) { force = true; *sp = '\0'; }
        if (strcmp(path, "FORCE") == 0) { force = true; strcpy(path, STM32_FW_BUNDLED_PATH); }
        /* ~10 s: on a task of its own, which answers (stm_fw_update_task). */
        if (s_stm_upd_busy.exchange(true)) { cdc_reply("ERR STM_FW busy\r\n"); return true; }
        strncpy(s_stm_upd_path, path, sizeof(s_stm_upd_path) - 1);
        s_stm_upd_path[sizeof(s_stm_upd_path) - 1] = '\0';
        s_stm_upd_force = force;
        if (xTaskCreate(stm_fw_update_task, "stm_fw_cdc", 6144, nullptr, 5, nullptr) != pdPASS) {
            s_stm_upd_busy.store(false);
            cdc_reply("ERR STM_FW no task\r\n");
        }
        return true;
    }
    if (strncmp(cmd, "STM_RESET", 9) == 0) {
        /* Hardware reset of the STM through the ESP's reset line, a 100 ms
         * pulse. An ST-LINK without an NRST wire cannot reset it, so when the STM sits in
         * STOP1 with debug off (a release build after a dirty jump out of the ROM DFU),
         * this is the only reset a host can reach. The STM console and the
         * link go away for a moment; the ESP stays up. */
        gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level((gpio_num_t)BSP_STM_RST_PIN, 0);
        cdc_reply("OK\r\n");
        return true;
    }
    if (strncmp(cmd, "STM_DFU", 7) == 0) {
        // Drop the STM into USB DFU; the bridge is gone until it reboots.
        cdc_reply(crosspad::getStm32CommandSender().sendDfuRequest(true)
                      ? "OK\r\n" : "ERR\r\n");
        return true;
    }
    if (sscanf(cmd, "AUDIO_TASKS %d", &a) == 1 && (a == 0 || a == 1)) {
        ESP_LOGW(TAG, "AUDIO_TASKS override: %s", a ? "resume" : "suspend");
        AudioTasks_SetSuspended(a == 0);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

/* "c reg" and nothing after it: CODEC_REG's read form. */
static bool codec_reg_is_read(const char *args)
{
    unsigned c = 0, reg = 0;
    char extra[2] = {0};
    return sscanf(args, "%u %x %1s", &c, &reg, extra) == 2;
}

void hil_control_companion_handle(const char *command, char *reply, size_t capacity) {
    if (strcmp(command, "CP_CAPS") == 0) {
        snprintf(reply, capacity, "CP_CAPS 1 control=1 files=0\r\n"); return;
    }
    /* Remote control and read-only state. Nothing that grants more than the
     * caller already has: USB_GUARD is readable, never settable, from here. */
    const bool allowed = strcmp(command, "STM_FW_INFO") == 0 || strcmp(command, "UI_STATE") == 0 ||
        strcmp(command, "APP_LIST") == 0 || strcmp(command, "LED_STATE") == 0 || strcmp(command, "PAD_NOTES") == 0 ||
        strcmp(command, "ENC_PRESS") == 0 || strncmp(command, "ENC_PRESS ", 10) == 0 ||
        strncmp(command, "ENC_ROTATE ", 11) == 0 ||
        strcmp(command, "APP_STOP") == 0 || strncmp(command, "APP_START ", 10) == 0 ||
        strncmp(command, "PAD_PRESS ", 10) == 0 || strncmp(command, "PAD_RELEASE ", 12) == 0 ||
        strncmp(command, "KIT_LOAD ", 9) == 0 || strcmp(command, "KIT_STATUS") == 0 ||
        strcmp(command, "SETTINGS_STATUS") == 0 || strcmp(command, "USB_GUARD") == 0 ||
        /* The card's transactions by task, read and cleared (sd_sector_io.cpp). */
        strcmp(command, "SD_TRACE") == 0 ||
        /* Read-only diagnostics. The MIDI+UAC2 profile has no CDC, so without
         * these the one profile where the render is under the most pressure is
         * also the one that cannot be measured -- which is how a render peaking
         * at 7.7 ms against a 1.45 ms block went unseen. They only report, and
         * SMPL_PEAK's reset-on-read of its own maxima is what a diagnostic is
         * for. Nothing here grants the caller anything it did not have. */
        strncmp(command, "SMPL_PEAK", 9) == 0 || strcmp(command, "MEM") == 0 ||
        strcmp(command, "PAD_STATS") == 0 || strcmp(command, "CLOCK_STATUS") == 0 ||
        strcmp(command, "AUDIO_STATUS") == 0 || strcmp(command, "UAC_STATS") == 0 || strcmp(command, "UAC_FLOW") == 0 || strcmp(command, "UAC_PKT") == 0 || strcmp(command, "UAC_LATE") == 0 ||
        /* The host USB audio is tuned for: a board in this profile is the one
         * that needs it, and it has no CDC to set it from. */
        strcmp(command, "USB_HOST") == 0 || strcmp(command, "USB_HOST AUTO") == 0 ||
        strcmp(command, "USB_HOST MAC") == 0 || strcmp(command, "USB_HOST LINUX") == 0 ||
        strncmp(command, "USB_ENUM", 8) == 0 ||
        /* Per-channel peaks, routes and mutes: which channel carries what is
         * on an output, in the USB audio profile (over the WiFi remote or
         * BLE -- the listing is longer than a SysEx reply). Read-only. */
        strcmp(command, "MIX_LVL") == 0 ||
        /* The same argument, for the three the first pass left out. A report
         * of "everything stutters in the audio profile" cannot be chased with
         * SMPL_PEAK alone: which engine holds the kit and what it dropped
         * (SMPL_ENGINE), how long a frame of UI takes (LVGL_STATS) and where
         * the CPU went (TASKS, whose table goes to the STM console -- a
         * separate USB device that this profile does not take away) are the
         * three that say whether the render, the UI or the scheduler is at
         * fault. All read-only; TASKS and LVGL_STATS reset their own maxima,
         * which is what a diagnostic is for. */
        strcmp(command, "SMPL_ENGINE") == 0 || strcmp(command, "LVGL_STATS") == 0 ||
        strcmp(command, "TASKS") == 0 || strcmp(command, "CDC_STATS") == 0 ||
        /* A song played and stopped -- nothing about it changes: what a
         * performance run needs in the one profile that records the board
         * (crosspad-hil play_perf). Matched whole. */
        strcmp(command, "ASONG") == 0 || strcmp(command, "ASONG PLAY") == 0 ||
        strcmp(command, "ASONG STOP") == 0 ||
        /* The Recorder's state (the report: input, peaks, take, error), and
         * a take from the USB input: that input exists only in this profile,
         * so a take from it can be neither made nor read anywhere else. Only
         * these -- the input, start, stop, let go; the rest of STOOL (files,
         * slices, exports) stays CDC-only. */
        strcmp(command, "STOOL") == 0 || strcmp(command, "STOOL IN USB") == 0 ||
        strcmp(command, "STOOL REC") == 0 || strcmp(command, "STOOL STOP") == 0 ||
        strcmp(command, "STOOL RELEASE") == 0 ||
        /* Which image is running. A board that cannot be switched out of this
         * profile without someone pressing Allow on it can still be reflashed
         * over UART through the STM bridge -- and then there is no way to
         * prove what landed, because every verb that says so needed CDC. */
        strcmp(command, "APP_SHA") == 0 || strcmp(command, "APP_VERSIONS") == 0 ||
        /* The read-only forms only, matched whole: `RT_PROF 1` starts the
         * render profiler and `SMPL_POLY n` changes the engine's voice caps,
         * and a prefix match let both through from BLE, SysEx and the WiFi
         * remote. */
        strcmp(command, "RT_PROF") == 0 || strcmp(command, "SMPL_POLY") == 0 ||
        /* Settings -> Audio -> Buffer, read or chosen: the latency a DAW on
         * the USB-audio profile hears is the one thing it would want to set,
         * and that profile has no CDC. The `Q` form, unsaved and for a bench,
         * stays CDC-only. */
        strcmp(command, "AUDIO_BUF") == 0 ||
        (strncmp(command, "AUDIO_BUF ", 10) == 0 && command[10] >= '0' && command[10] <= '9') ||
        strcmp(command, "RESET_LOG") == 0 || strcmp(command, "COREDUMP_INFO") == 0 ||
        strncmp(command, "TWIN ", 5) == 0 || strcmp(command, "TWIN_STATE") == 0 || strcmp(command, "TWIN_HELLO") == 0 ||
        strncmp(command, "MOTION ", 7) == 0 ||
        strncmp(command, "TWIN_SETTINGS ", 14) == 0 || strncmp(command, "TWIN_INFO ", 10) == 0 ||
        strncmp(command, "TWIN_FW ", 8) == 0 ||
        strncmp(command, "HOST_INFO", 9) == 0 || strncmp(command, "FW_LATEST", 9) == 0 ||
        strcmp(command, "FW_UPD_STATUS") == 0 || strcmp(command, "FW_CATALOG") == 0 ||
        /* Start a catalog check: answers OK at once and leases WiFi on the job
         * task, so it is safe from SysEx/BLE/the WiFi remote. Exact
         * match: it takes no arguments. FW_DOWNLOAD stays CDC-only. */
        strcmp(command, "FW_CHECK") == 0 ||
        strncmp(command, "FW_CATALOG ", 11) == 0 || strcmp(command, "ACCT_STATUS") == 0 ||
        strcmp(command, "REMOTE_STATUS") == 0 || strncmp(command, "WIFI_", 5) == 0 ||
        /* A codec register, read only (two arguments; the write form has a
         * third): the MIDI+UAC2 profile is where the codecs are set up for
         * the capture loop, and without CDC nothing could read them there. */
        (strncmp(command, "CODEC_REG ", 10) == 0 && codec_reg_is_read(command + 10));
    if (!allowed) { snprintf(reply, capacity, "ERR unsupported_companion_command\r\n"); return; }
    /* WiFi verbs CDC serves alone. SCAN and PING answer from a task of their
     * own, and the companion mailbox is filled before this handler returns.
     * UP and DOWN are the host's lease: over BLE the radio stops BLE right
     * after the reply, so the peer could never send the DOWN, and the radio
     * would stay up with BLE off until a reboot. Matched by verb name, the
     * way wifi_verbs_handle() reads it, so arguments do not slip past. */
    static const char *const kCdcOnlyWifi[] = {"WIFI_SCAN", "WIFI_PING", "WIFI_UP", "WIFI_DOWN"};
    for (const char *verb : kCdcOnlyWifi) {
        const size_t n = strlen(verb);
        if (strncmp(command, verb, n) == 0 && (command[n] == '\0' || command[n] == ' ')) {
            snprintf(reply, capacity, "ERR use_cdc\r\n"); return;
        }
    }
    /* The guard prompt is answered by a hand on the encoder, never by the
     * host that raised it. */
    if (strncmp(command, "ENC_", 4) == 0 && usb_serial_prompt_active()) {
        snprintf(reply, capacity, "ERR guard_prompt_active\r\n"); return;
    }
    companionReplyBuffer = reply; companionReplyCapacity = capacity;
    companionReplyTask.store(xTaskGetCurrentTaskHandle());
    hil_stack_mark();
    const bool handled = hil_control_handle_cdc(command, strlen(command));
    hil_stack_note(command, strlen(command));
    companionReplyTask.store(nullptr);
    companionReplyBuffer = nullptr;
    if (!handled || !reply[0]) snprintf(reply, capacity, "ERR unsupported_companion_command\r\n");
}

/* The browser twin mirrors touch and pads to whoever asked, so it is a USB
 * host's to start: BLE peers get no TWIN verbs. The motion stream goes out on
 * USB MIDI only, so a BLE peer has nothing to keep alive. */
static void ble_companion_handle(const char *command, char *reply, size_t capacity) {
    if (strncmp(command, "TWIN", 4) == 0 || strncmp(command, "MOTION ", 7) == 0) {
        snprintf(reply, capacity, "ERR twin_usb_only\r\n"); return;
    }
    hil_control_companion_handle(command, reply, capacity);
}

void hil_control_ble_poll(void) {
    crosspad::bleCompanionPoll(ble_companion_handle);
}
