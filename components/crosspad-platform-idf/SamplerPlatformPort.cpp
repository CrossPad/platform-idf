// SPDX-License-Identifier: GPL-3.0-or-later
//
// The sampler's platform callbacks on the board (SamplerPlatformPort.hpp).

#include "SamplerPlatformPort.hpp"

#include "crosspad-sampler/SamplerPlatformCallbacks.hpp"
#include "crosspad-sampler/SamplerApp.hpp"
#include "crosspad-sampler/SamplerConfig.hpp"
#include "crosspad-sampler/ui/sampler_panel.hpp"
#include "crosspad-gui/components/file_explorer.h"
#include "crosspad-gui/components/status_bar.h"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"
#include <crosspad/event/IEventBus.hpp>
#include <crosspad/event/EventData.hpp>
#include <crosspad/instrument/WavPcm.hpp>
#include <crosspad/kit/IKitManager.hpp>
#include <crosspad/kit/KitPathUtils.hpp>
#include <crosspad/platform/PlatformServices.hpp>
#include <crosspad/sampletools/SampleFx.hpp>
#include <crosspad/sampletools/SliceMath.hpp>

#include "engine_arena.hpp"
#include "PitchedKitPort.hpp"
#include "PitchedSampleNode.hpp"
#include "SamplePlayerAccess.hpp"
#include "SamplerEnginePort.hpp"
#include "SongEnginePort.hpp"
#include "lat_trace.h"
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "CrosspadPlatform.hpp"
#include "MarcelSampleNode.hpp"
#endif
/* Scenes are stored beside the kit; without the mixer there is no sequencer to
 * store. Same detection the rest of the platform uses. */
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include "SequenceStorePort.hpp"
#define SAMPLER_PORT_HAS_SEQUENCER 1
#endif

#include "bsp_sdmmc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>

static const char * TAG = "GUI";

/* Defined in main/: the engine handoff (engine_handoff.h), the instrument host
 * (instrument_host.h), the locked lv_async_call (lv_async_safe.h), the HIL
 * counters (hil_control.h), the board's kit rate conversion
 * (kit_convert_port.h) and the open song's sounds (song_sounds.h). */
extern "C" void kit_engines_stop(void);
extern bool instrument_host_owns_engine();
namespace crosspad { class IKitConvertHost; }
extern crosspad::IKitConvertHost *kit_convert_host(void);
extern void song_sounds_kit_loaded();
extern "C" void lv_async_call_locked(lv_async_cb_t cb, void *user_data);
extern std::atomic<uint32_t> g_hil_note_on_count;
extern std::atomic<uint16_t> g_hil_last_note_on;

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include <ml_smpl_stream_ply.h>
extern "C" bool marcel_engine_start(void);
extern "C" int marcel_engine_free_slots(void);
/* True only while SampleStreamPlayer_Init() stands: false before boot finishes
 * setup, and false again for as long as a pitched kit owns the engine's PSRAM.
 * Every call into the prebuilt library below is gated on it — the pad paths
 * fire before setup, and after a DeInit the library's handlers dereference
 * memory that has gone back to the arena. */
extern bool smpl_stream_ply_ready(void);
#endif

/* A pitched bank is built only on the kit_load worker. platform_reloadPad()
 * is also reached inline from the LVGL thread (pad editor) and the CDC main
 * task (SMPL_PITCH, 4 kB stack): there a build is 0.5-8 s of card reads and
 * FFT with the UI frozen, after an unbounded wait on the arena owner lock
 * behind a native take -- and a stack overflow risk. Off the worker, pad 0 is
 * marked modified and the worker is asked to reload it (the route
 * pitched_editor's rebuildBank() takes); false means the caller is the worker
 * and builds now. */
static bool defer_pitched_build_to_worker(crosspad::KitInfo* kit)
{
    if (crosspad_sampler::kit_load_task_is_worker()) return false;
    kit->pads[0].dirty = true;
    ESP_LOGI(TAG, "pitched bank: build handed to the kit loader (pad 0 reload off its task)");
    lv_async_call_locked([](void*) { crosspad_sampler::kit_load_task_start(true, -1); }, nullptr);
    return true;
}

// Queue every uncached layer of a freshly-loaded kit. The loader task is
// long-lived, so "already running" is not a reason to skip the preload —
// treating it as one left kits loaded over CDC with no waveforms at all.
static void platform_topUpWaveformCache() {
    auto* mgr = crosspad::getKitManager();
    auto* kit = mgr ? mgr->getCurrentKit() : nullptr;
    if (!kit) return;

    // The cache belongs to one kit. The selector switches it before the load;
    // every other entry point (boot, CDC, the sampler) lands here instead.
    crosspad_sampler::waveform_cache_clear_if_kit_changed(kit, kit->path.c_str());

    /* By what the cache holds, not by asking the card which layout each
     * sample is in: this is the LVGL thread, right as a kit lands under play,
     * and 16-32 stat()s here kept the streamer off the card for ~200 ms. The
     * loader resolves the missing ones on its own thread. */
    int total   = 0;
    int missing = 0;
    for (int i = 0; i < crosspad_sampler::PAD_COUNT; ++i) {
        for (const auto& sample : kit->pads[i].samples) {
            if (sample.sample.empty()) continue;
            ++total;
            if (!crosspad_sampler::waveform_cache_find_kit_sample(kit->path.c_str(), sample.sample.c_str())) ++missing;
        }
    }
    if (missing == 0) return;
    crosspad_sampler::totalWaveformsToCache = total;
    crosspad_sampler::waveformsCached       = total - missing;
    crosspad_sampler::waveform_loader_sweep_now();
}

static void platform_startLoadKit(int kitId, bool onlyModified) {
    // Parsing a kit off the SD card takes seconds; on the LVGL thread that
    // froze the UI into the task watchdog. The load task owns it now.
    crosspad_sampler::kit_load_task_start(onlyModified, kitId);
}

extern void sample_player_port_flush_files(void);
extern std::atomic<uint32_t> g_engine_noblock, g_engine_underrun;   /* sample_player_port.cpp */
extern bool sample_player_pad_sounding(uint8_t pad);
extern std::atomic<uint32_t> g_rt_gap_max_us;                       /* audio_task.cpp */
extern std::atomic<uint32_t> g_bg_pass_max_us;                      /* audio_module.cpp */
extern std::atomic<uint32_t> g_port_stream_fopen;                   /* sample_player_port.cpp */
extern std::atomic<uint32_t> g_port_fopen_max_us;                   /* sample_player_port.cpp */
extern std::atomic<uint32_t> g_port_stream_bytes, g_port_mutator_bytes; /* sample_player_port.cpp */
extern std::atomic<bool> g_port_loader_hurry;                          /* sample_player_port.cpp */
extern void sample_player_pad_starting(uint8_t pad);
extern void sample_player_pad_forget_voices(uint8_t pad);
extern void sample_player_port_yield_to_streamer(uint32_t weight);

/* Hits that arrived while their pad was being restructured: held, then
 * played once the pad is back — late by what was left of the reload rather
 * than lost. One per pad; a second hit in that window replaces the first. */
static std::atomic<uint32_t> s_lateHits{0};
static std::atomic<uint32_t> s_droppedHits{0};
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
static std::atomic<uint8_t>  s_heldVelocity[KIT_PADS];
/* When each held hit came in, and what holding cost since SMPLLAST last read:
 * the latest hit's delay (ms) and the hits a newer one on the same pad
 * replaced before the pad was back -- those never sound. */
EXT_RAM_BSS_ATTR static uint32_t s_heldMs[KIT_PADS];
#endif
EXT_RAM_BSS_ATTR uint32_t g_late_max_ms, g_late_lost;
EXT_RAM_BSS_ATTR uint32_t g_reload_win_max_us;   /* longest a pad was marked */
/* The pad platform_reloadPad() is inside, or -1; the SamplePlayback bridge
 * refuses a hit on it. Set here rather than in the loader so the pad editor's
 * single-pad reload is covered too. */
static std::atomic<int> s_reloadingPad{-1};

/* Restructuring a pad the renderer has no voice in needs no render gate: the
 * gate is what turned every SetupSample() — 30..200 ms of SD reads — into a
 * hole in every other pad. A pad that is still sounding (the loader forces
 * the last one) keeps the gate, so the renderer is never inside a voice
 * whose buffers are being freed. Either way the streamer is held out (its
 * BkgProc() reads the files of the samples this changes) and the file slot is held;
 * the streamer first, then the gate -- see SamplePlayerMutationGuard. */
class PadMutationGuard {
public:
    explicit PadMutationGuard(uint8_t pad) : gated_(sample_player_pad_sounding(pad)) {
        crosspad::samplePlayerStreamExcludeLock();
        if (gated_) crosspad::samplePlayerGateEnter();
        crosspad::samplePlayerFileLock();
    }
    ~PadMutationGuard() {
        crosspad::samplePlayerFileUnlock();
        if (gated_) crosspad::samplePlayerGateExit();
        crosspad::samplePlayerStreamExcludeUnlock();
    }
    PadMutationGuard(const PadMutationGuard&) = delete;
    PadMutationGuard& operator=(const PadMutationGuard&) = delete;
private:
    bool gated_;
};


static void platform_onKitLoadFinished(bool success) {
    /* The engine the kit ends up in. A kit load the native engine does not
     * already hold lands in the prebuilt one -- reload_pad_impl() starts it --
     * so whoever prefers the native engine has to ask for it here, once the
     * load is done and the pads are in the model. Without this the choice is
     * reachable only over CDC and does not survive a reboot, which is no use
     * to someone playing.
     *
     * Not when it already holds the kit: the loader has just swapped every
     * pad into it while it played (native_sampler_swap_pad), and a take on
     * top silenced the whole kit and read it off the card a second time --
     * the gap a kit change used to leave in the music. */
    if (success && native_sampler_preferred() && !native_sampler_active()) native_sampler_take_kit();

    // The port's handle cache is left alone: voices of the outgoing kit may
    // still be sounding, and closing their handles here made the streamer
    // reopen every one of them in a single refill pass — a hole in all of
    // them at once. The cache recycles the old kit's handles by itself.
    if (!success) {
        // The orchestrator cannot know the card is gone; replace its generic
        // message once we can check.
        if (bsp_sdcard_get_handle() == nullptr) {
            crosspad_gui::statusbar_process_loader_status(100, true, true, "No SD card");
        }
        return;
    }
    // The open song is made with this kit from now on.
    song_sounds_kit_loaded();
    // A pitched zone that did not fit the bank budget still plays; how much of
    // it was dropped is only visible here.
    if (crosspad::pitchedKitActive()) {
        const auto& report = crosspad::pitchedKitLastReport();
        if (report.truncatedZones > 0 && report.warning[0]) {
            crosspad_gui::statusbar_show_loader_message("Warning", report.warning, false);
        }
    }
    platform_topUpWaveformCache();

    /* The kit's scenes live beside it on the card. Only a request is left here
     * -- this is the LVGL thread, and reading the card on it is exactly what
     * the kit loader was moved off it to avoid. */
#ifdef SAMPLER_PORT_HAS_SEQUENCER
    crosspad::KitInfo* kit = crosspad::getKitManager()
                                ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    crosspad::sequence_store_kit_changed(kit ? kit->path.c_str() : "");
#endif
}

// ── Player callbacks (low-latency fingerdrumming) ──────────────────────────

static void platform_noteOn(uint8_t padIdx, uint8_t velocity) {
    /* Whichever sample engine holds the kit. Only one of the two ever does --
     * they share the arena -- so this is a choice, not a fan-out. */
    if (native_sampler_active()) { native_sampler_note_on(padIdx, velocity); return; }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (!smpl_stream_ply_ready()) return;
    SampleStreamPlayer_NoteOn(padIdx, velocity);
#endif
}

static void platform_setVolume(uint8_t padIdx, int volume) {
    if (native_sampler_active()) { native_sampler_set_pad_volume(padIdx, volume); return; }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (!smpl_stream_ply_ready()) return;
    SampleStreamPlayer_SetVolume(padIdx, static_cast<uint8_t>(volume));
#endif
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
// Kit files store pan as a signed offset (0 = centre); the player API uses the
// MIDI convention (0..127, 64 = centre). Convert at the boundary.
static uint8_t toPlayerPan(int kitPan) {
    const int v = kitPan + 64;
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 127 ? 127 : v));
}
#endif

static void platform_setPan(uint8_t padIdx, int pan) {
    if (native_sampler_active()) { native_sampler_set_pad_pan(padIdx, pan); return; }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (!smpl_stream_ply_ready()) return;
    SampleStreamPlayer_SetPan(padIdx, toPlayerPan(pan));
#endif
}

/* The editor's loop button writes playbackMode into the kit model and then
 * tells the engine. The native engine reads the mode out of that model, so
 * both calls are the same question to it: what does the pad say now. */
static void platform_setLoopEnd(uint8_t padIdx, uint32_t endPos) {
    if (native_sampler_sync_pad_mode(padIdx)) return;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (!smpl_stream_ply_ready()) return;
    SampleStreamPlayer_SetLoopEnd(padIdx, endPos);
#endif
}

static void platform_clearLoop(uint8_t padIdx) {
    if (native_sampler_sync_pad_mode(padIdx)) return;
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (!smpl_stream_ply_ready()) return;
    SampleStreamPlayer_SetLoopClear(padIdx);
#endif
}

static void platform_wipeOut(uint8_t padIdx) {
    if (native_sampler_active()) { native_sampler_stop_pad(padIdx); return; }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (!smpl_stream_ply_ready()) return;
    crosspad::SamplePlayerMutationGuard guard;
    SampleStreamPlayer_WipeOut(padIdx);
#endif
}

/* The editor refuses to add a sample to a pad when this is not positive, and
 * the prebuilt engine answers -1 while it is stopped -- which is exactly when
 * the native one is playing. Asking the engine that holds the kit. */
static int platform_getFreeSlotCount() {
    const int n = native_sampler_free_slots();
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    if (n >= 0) return n;
    return marcel_engine_free_slots();
#else
    return n;
#endif
}

// Auditions run through a scratch note above the pad range, on the same
// setup+NoteOn path pads use: SampleStreamPlayer_Preview() renders a flat zero
// on this build. The slot is wiped first so repeated previews reuse one WAV
// handle. Note 16 is the only spare of the player's 17 slots (pads take 0..15);
// SetupSample() rejects anything above it and the preview stays silent.
static constexpr uint8_t kPreviewNote = 16;

static void platform_previewSample(const char* path, uint32_t start, uint32_t end,
                                   int volume, int pan) {
    /* No path: the browser closed (file_explorer), the audition stops. */
    if (!path || !path[0]) { native_sampler_preview_stop(); return; }
    /* The native engine's audition voice: its own ring, streamed on audio_bg,
     * heard on the Preview channel. The next one replaces it. Nothing here
     * reads the card -- the header is read on audio_bg too. */
    if (native_sampler_preview(crosspad::kitFsPath(path).c_str(), start, end, volume, pan)) return;
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    /* No kit in the native engine (a pitched bank holds the arena): no
     * audition, as the prebuilt streamer had none while it was stopped. */
    return;
#else
    if (!smpl_stream_ply_ready()) return;
    const char* p = (path[0] == '/') ? path + 1 : path;

    {
        crosspad::SamplePlayerMutationGuard guard;
        SampleStreamPlayer_WipeOut(kPreviewNote);
    }

    // Runs on the LVGL thread while audio_bg streams — the same collision the
    // kit reload hits, from a third thread.
    if (end == 0 || end == 0xFFFFFFFFu) {
        struct sample_info_from_file info;
        crosspad::SamplePlayerFileGuard guard;
        if (!SampleStreamPlayer_GetInfo(&info, p)) return;
        end = info.sampleCount;
    }
    if (start >= end) start = 0;

    {
        crosspad::SamplePlayerMutationGuard guard;
        /* The start and a length (skipSamples, lenSamples), not the end. */
        if (!SampleStreamPlayer_SetupSample(kPreviewNote, p,
                                            /*group=*/0, /*wavIdx=*/0, start, end - start)) {
            return;
        }
    }
    SampleStreamPlayer_SetVolume(kPreviewNote,
                                 static_cast<uint8_t>(volume > 0 ? volume : 100));
    SampleStreamPlayer_SetPan(kPreviewNote, static_cast<uint8_t>(pan));
    SampleStreamPlayer_SetLoopClear(kPreviewNote);
    SampleStreamPlayer_NoteOn(kPreviewNote, 127);
#endif
}

/* HIL: audition a pad's first sample the way the kit browser does — the only
 * preview path reachable in the USB-audio profile (SysEx 0x1D 0x0A). */
extern "C" void hil_preview_pad(unsigned char padIdx) {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit || padIdx >= KIT_PADS || kit->pads[padIdx].sampleCount == 0) return;
    /* The native engine has the pad's sample resident already, so an audition
     * is the pad's own note -- no scratch slot, no second copy of the file. */
    if (native_sampler_active()) { native_sampler_note_on(padIdx, 100); return; }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    std::string resolved = crosspad::resolveKitSamplePath(kit->path, kit->pads[padIdx].samples[0].sample);
    platform_previewSample(resolved.c_str(), 0, 0xFFFFFFFFu, 100, 0);
#endif
}

static bool platform_getSampleInfo(const char* path, crosspad_sampler::SampleInfo* outInfo) {
    if (native_sampler_active()) {
        /* The native engine's own reader, which is the one that decides
         * whether the file is playable at all -- so the panel's length and
         * rate come from the same header the voice will read. */
        uint32_t frames = 0, rate = 0;
        uint8_t  ch = 0;
        const std::string fs = crosspad::kitFsPath(path ? path : "");
        if (!native_sampler_describe(fs.c_str(), &frames, &ch, &rate)) return false;
        outInfo->sampleCount      = frames;
        outInfo->sampleRate       = rate;
        outInfo->numberOfChannels = ch;
        outInfo->bitsPerSample    = 16;   // read() always hands back int16
        return true;
    }
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    return false;
#else
    if (!smpl_stream_ply_ready()) return false;
    const char* p = (path && path[0] == '/') ? path + 1 : path;
    struct sample_info_from_file info;
    // Called from the LVGL thread while the panel draws. A kit load holds the
    // file slot for up to half a second per pad (its reads yield to the
    // streamer); waiting here froze the UI for the length of the load.
    constexpr uint32_t kUiFileWaitMs = 30;
    crosspad::SamplePlayerFileTryGuard guard(kUiFileWaitMs);
    if (!guard.ok()) return false;
    if (!SampleStreamPlayer_GetInfo(&info, p)) return false;
    outInfo->sampleCount = info.sampleCount;
    outInfo->sampleRate = info.sampleRate;
    outInfo->numberOfChannels = info.numberOfChannels;
    outInfo->bitsPerSample = info.bitsPerSample;
    return true;
#endif
}

/// The layout of the WAV open in @p f. Declared sizes are trusted: clamping
/// them costs a seek to the end, which on FAT walks the file's cluster chain.
static bool readWavLayout(FILE* f, crosspad::WavLayout& out) {
    return crosspad::wavReadLayout(f, out, /*clampToFile=*/false) && out.blockAlign != 0;
}

// ── Audio reload callbacks ─────────────────────────────────────────────────

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/// What layer @p i of a pad plays. The player can neither transpose, filter
/// nor play backwards, so a pad with any of it plays a copy of its sample
/// rendered once (crosspad::renderedSamplePath; the pad editor and the kit
/// load task render it) with the trim mapped onto it. Until that copy exists
/// the pad plays the sample as it is.
struct PadFile {
    std::string path;          // logical, as the player takes it
    uint32_t    start = 0;
    uint32_t    end = 0;       // 0 = to the end
};

static PadFile pad_file(const crosspad::KitInfo& kit, const crosspad::Pad& pad, uint8_t i) {
    PadFile f;
    f.path = crosspad::resolveKitSamplePath(kit.path, pad.samples[i].sample);
    f.start = pad.samples[i].start;
    f.end = pad.samples[i].end;
    const crosspad::SampleFx fx{pad.pitch, pad.hp, pad.lp, pad.reverse};
    if (!fx.any()) return f;
    const std::string copy = crosspad::renderedSamplePath(f.path, fx);
    struct stat st;
    if (stat(crosspad::kitFsPath(copy).c_str(), &st) != 0) return f;
    uint32_t start = f.start, end = f.end;
    if (fx.reverse) {
        /* The copy runs backwards: the trim's end is where it starts. */
        uint32_t n = 0;
        {
            const char* p = f.path.c_str();
            if (p[0] == '/') p++;
            struct sample_info_from_file info;
            crosspad::SamplePlayerFileGuard guard;
            if (SampleStreamPlayer_GetInfo(&info, p)) n = info.sampleCount;
        }
        if (!n) return f;
        const uint32_t e = end && end < n ? end : n;
        const uint32_t s = std::min(start, e);
        start = n - e;
        end = s ? n - s : 0;
    }
    const double ratio = crosspad::SliceMath::pitchRatio(pad.pitch);
    f.path = copy;
    f.start = (uint32_t)((double)start / ratio);
    f.end = end ? (uint32_t)std::ceil((double)end / ratio) : 0;
    return f;
}

/// SetLoopEnd()'s end for a pad that loops its whole sample.
static constexpr uint32_t kLoopToEnd = 0xFFFFFFFFu;

static bool reload_pad_impl(uint8_t padIdx, bool onlyIfIdle) {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit || padIdx >= KIT_PADS) return true;
    // An app has the engine (instrument_host.h): the kit is parsed and current,
    // and the app's release() reloads it into the engine.
    if (instrument_host_owns_engine()) return true;

    // The first layer's length is asked before the pad is taken apart, so
    // the window in which a hit on this pad is held need not ask for it.
    uint32_t firstCount = 0;
    if (smpl_stream_ply_ready() && kit->engine != crosspad::KitEngine::Pitched) {
        auto& pad = kit->pads[padIdx];
        if (pad.sampleCount > 0) {
            const PadFile f = pad_file(*kit, pad, 0);
            const char* p = f.path.c_str();
            if (p[0] == '/') p++;
            struct sample_info_from_file info;
            crosspad::SamplePlayerFileGuard guard;
            if (SampleStreamPlayer_GetInfo(&info, p)) firstCount = info.sampleCount;
        }
    }

    // A hit that landed during the warm-up plays the old sample, which is
    // still intact — and it plays it to the end: the loader is told to come
    // back rather than cut the voice (a wipe under a live voice let it carry
    // on into the new file: old tone, a hole, new tone).
    if (onlyIfIdle && sample_player_pad_sounding(padIdx)) return false;

    // From here until the pad is back, a hit on it is held (see the
    // SamplePlayback bridge) and played by this marker's destructor.
    struct Marker {
        uint8_t pad;
        int64_t t0 = esp_timer_get_time();
        explicit Marker(uint8_t p) : pad(p) {
            s_heldVelocity[p].store(0, std::memory_order_relaxed);
            s_reloadingPad.store(p, std::memory_order_release);
        }
        ~Marker() {
            s_reloadingPad.store(-1, std::memory_order_release);
            const uint32_t win = (uint32_t)(esp_timer_get_time() - t0);
            if (win > g_reload_win_max_us) g_reload_win_max_us = win;
            g_port_loader_hurry.store(false, std::memory_order_relaxed);
            const uint8_t vel = s_heldVelocity[pad].exchange(0, std::memory_order_acq_rel);
            if (vel && smpl_stream_ply_ready()) {
                s_lateHits.fetch_add(1, std::memory_order_relaxed);
                const uint32_t late = (uint32_t)(esp_timer_get_time() / 1000) - s_heldMs[pad];
                if (late > g_late_max_ms) g_late_max_ms = late;
                sample_player_pad_starting(pad);
                SampleStreamPlayer_NoteOn(pad, vel);
            }
        }
    } marker(padIdx);

    if (smpl_stream_ply_ready()) {
        PadMutationGuard guard(padIdx);
        SampleStreamPlayer_WipeOut(padIdx);
        // Whatever voice the wipe cut off will not report its end.
        sample_player_pad_forget_voices(padIdx);
    }

    if (kit->engine == crosspad::KitEngine::Pitched) {
        // The WipeOut above already removed whatever the previous kit left on
        // this pad, which is all the prebuilt engine has to do for a pitched kit. The whole
        // bank is built once, on pad 0; the other fifteen carry no per-pad
        // engine state (pad volume/pan come from the port).
        if (padIdx == 0) {
            // The bank is built out of the engine arena, and the prebuilt engine
            // holds all of it. Stop it first: the pitched kit's PSRAM *is* its,
            // which is what raises the bank budget from 256 kB to megabytes.
            // The native engine too, when it holds the kit: marcel_engine_stop()
            // alone left its heads in the arena and its voices rendering.
            // The arena's owner across the handoff and the build: no native
            // take or pad rebuild runs between them (engine_arena.hpp).
            // Built on the kit loader only.
            if (defer_pitched_build_to_worker(kit)) return true;
            EngineArenaOwner owner;
            kit_engines_stop();
            char err[96];
            if (!crosspad::pitchedKitLoad(*kit, err, sizeof(err))) {
                // SamplerAudioCallbacks has no error channel out of reloadPad
                // (onKitLoadFinished only reports the parse), so the log is it.
                ESP_LOGE(TAG, "pitched kit '%s' failed to load: %s",
                         kit->name.c_str(), err);
            }
        }
        return true;
    }

    // Back on a sampler kit: the pitched bank goes first, then the arena is
    // wiped and the prebuilt engine takes it back. The reset is what makes the swap
    // symmetric — it reclaims a bank the render gate forced us to leak just as
    // it reclaims anything DeInit kept.
    /* Everything below writes into the prebuilt engine, so it has to be the
     * one holding the arena first.
     *
     * This used to be `if (padIdx == 0)`, which made starting that engine a
     * *side effect of pad 0 being reloaded* -- and `onlyModified` skips a pad
     * that is not dirty. A kit whose pad 0 was unchanged therefore reached
     * `SampleStreamPlayer_GetInfo()` with the engine stopped, which divides
     * by zero inside the library: `task=kit_load cause=6`, found by swapping
     * kits with pads firing through the loads. The condition is readiness,
     * not which pad happens to come first. */
    if (padIdx == 0 || !smpl_stream_ply_ready()) {
        /* A kit load resets the arena, and the native engine's resident heads
         * are in it. It gives them back first and the kit then loads into the
         * prebuilt engine as usual; taking it into the native one again is a
         * deliberate act (SMPL_ENGINE native), not something a kit change
         * should decide. Without this the reset below runs under the heads
         * every voice is reading from.
         *
         * Not gated on native_sampler_active(): releasing never needs the
         * engine to be current, and a take that is mid-flight has already
         * cleared that flag while still holding every head. */
        EngineArenaOwner owner;   /* the handoff below, then the reset */
        /* A bank in the arena goes with it; one beside the kits is a song's
         * Pitch track's, and plays on. */
        if (crosspad::pitchedBankInArena()) crosspad::pitchedKitUnload();
        native_sampler_release();
        if (!smpl_stream_ply_ready()) {
            crosspad::song_engine_release_arena();   // its rings may sit in the arena
            engine_arena_reset();
            marcel_engine_start();
        }
    }
    /* And if it still is not ready, there is nothing here to write into. A
     * divide by zero inside a library we cannot step through is not a risk
     * worth taking on a start that did not come up. */
    if (!smpl_stream_ply_ready()) {
        ESP_LOGW(TAG, "pad %u: the prebuilt engine is not up, nothing reloaded",
                 (unsigned)padIdx);
        return false;
    }
    auto& pad = kit->pads[padIdx];
    for (uint8_t i = 0; i < pad.sampleCount; ++i) {
        const PadFile f = pad_file(*kit, pad, i);
        const char* p = f.path.c_str();
        if (p[0] == '/') p++;

        // Resolve start/end: end=0 means "whole file" — query actual sample count
        uint32_t start = f.start;
        uint32_t end   = f.end;
        if (end == 0 && i == 0) end = firstCount;
        if (end == 0) {
            // Guarded one call at a time, not across the loop, like every
            // mutator call.
            struct sample_info_from_file info;
            crosspad::SamplePlayerFileGuard guard;
            if (SampleStreamPlayer_GetInfo(&info, p)) {
                end = info.sampleCount;
            }
        }
        if (start >= end) start = 0;

        {
            PadMutationGuard guard(padIdx);
            // The start and a length (skipSamples, lenSamples), not the end.
            SampleStreamPlayer_SetupSample(padIdx, p, pad.choke_group, i, start, end - start);
        }
    }
    SampleStreamPlayer_SetVolume(padIdx, pad.volume);
    SampleStreamPlayer_SetPan(padIdx, toPlayerPan(pad.pan));
    if (pad.playbackMode == 2) {
        SampleStreamPlayer_SetLoopEnd(padIdx, kLoopToEnd);
    } else {
        SampleStreamPlayer_SetLoopClear(padIdx);
    }
    return true;
}
#else
static bool reload_pad_impl(uint8_t padIdx, bool onlyIfIdle) {
    (void)onlyIfIdle;   /* no voice of this engine is rebuilt under a load */
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit || padIdx >= KIT_PADS) return true;
    // An app has the engine (instrument_host.h): the kit is parsed and current,
    // and the app's release() reloads it into the engine.
    if (instrument_host_owns_engine()) return true;

    if (kit->engine == crosspad::KitEngine::Pitched) {
        // The whole bank is built once, on pad 0; the other fifteen carry no
        // per-pad engine state (pad volume/pan come from the port).
        if (padIdx == 0) {
            // The bank is built out of the engine arena, which the native
            // engine's resident heads -- or the previous pitched bank -- may
            // be holding: each lets go first, into the heap it came from, and
            // the arena is reset and empty for the bank (kit_engines_stop).
            // The owner lock across the handoff *and* the build: a native take
            // still running for a kit the burst has passed (smpl_take) ends
            // before this starts, and none starts until the bank is in --
            // where it then finds a pitched kit current and drops.
            // Built on the kit loader only (defer_pitched_build_to_worker).
            if (defer_pitched_build_to_worker(kit)) return true;
            EngineArenaOwner owner;
            kit_engines_stop();
            char err[96];
            if (!crosspad::pitchedKitLoad(*kit, err, sizeof(err))) {
                ESP_LOGE(TAG, "pitched kit '%s' failed to load: %s",
                         kit->name.c_str(), err);
            }
        }
        return true;
    }

    /* A sampler kit. The native engine takes the whole kit once the load is
     * done (platform_onKitLoadFinished -> native_sampler_take_kit()), which
     * releases the previous kit and resets the arena itself; there is nothing
     * per pad to do here. Only a pitched bank goes now, so a hit during the
     * load does not sound a zone of a kit that is no longer current -- a
     * pitched kit's, in the arena; a song's Pitch track's beside the kits
     * plays on. */
    if (crosspad::pitchedBankInArena()) crosspad::pitchedKitUnload();
    /* On the kit loader -- the way back from a pitched kit -- the native
     * engine takes the kit now and the pads swap into it, as the first kit
     * after boot does. Left to platform_onKitLoadFinished(), the load ended
     * with no engine holding the kit, and the loader, asking each pad whether
     * it plays, called a kit of good samples unreadable. */
    if (crosspad_sampler::kit_load_task_is_worker() && !instrument_host_owns_engine() &&
        native_sampler_swap_pad(padIdx, true)) {
        return true;
    }
    /* A pad changed outside a kit load (the pad editor) with no kit in the
     * engine -- a take that failed, or the first pad given a sample: the
     * engine takes the kit as it is now. While it holds one,
     * platform_reloadPad() has already rebuilt the pad in place. */
    if (!crosspad_sampler::kit_load_task_is_loading() && !native_sampler_active() &&
        native_sampler_state() != 1) {
        native_sampler_take_kit();
    }
    return true;
}

#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */

/* The native engine transposes in the voice, so a pitch-only pad wants the
 * original file and a step -- not a copy on the card. Filters and reverse it
 * does not do, so those keep the rendered copy they have always had. */
static bool platform_engineHandlesFx(int semitones, uint16_t hpHz, uint16_t lpHz, bool reverse) {
    (void)semitones;
    if (hpHz || lpHz || reverse) return false;
    return native_sampler_active();
}

static void platform_reloadPad(uint8_t padIdx) {
    /* While the native engine holds the kit it rebuilds the pad itself, in
     * place -- the alternative is reload_pad_impl() below, which resets the
     * arena and hands the kit to the prebuilt engine, and a pitch knob that
     * does that is a pitch knob that cannot be heard. */
    if (native_sampler_reload_pad(padIdx)) return;
    reload_pad_impl(padIdx, false);
}

/* The kit loader's door, on its own task. The native engine swaps the pad
 * there and then, under whatever is playing -- a sounding voice plays its old
 * sample out -- so a kit change is pads changing one by one, never a stop.
 * It takes the kit first when it is preferred and not holding one, unless an
 * app has the engine (reload_pad_impl() then only parses). */
static bool platform_reloadPadIfIdle(uint8_t padIdx) {
    if (native_sampler_swap_pad(padIdx, !instrument_host_owns_engine())) return true;
    return reload_pad_impl(padIdx, true);
}

/* Whether the loader must wait for this pad. The native engine never takes a
 * pad apart under a voice, so with it there is nothing to wait for -- and the
 * prebuilt engine's bits, which are all this could ask otherwise, still hold
 * whatever it was playing when it was stopped. */
static bool platform_padSounding(uint8_t padIdx) {
    if (native_sampler_active()) return false;
    return sample_player_pad_sounding(padIdx);
}

/* Whether a hit on the pad sounds, for its LED and the load's verdict. An app
 * holding the engine (instrument_host) plays the kit its own way, so the pad
 * is taken at its word. */
static bool platform_padPlayable(uint8_t padIdx) {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit || padIdx >= KIT_PADS) return false;
    if (instrument_host_owns_engine()) return true;
    if (kit->engine == crosspad::KitEngine::Pitched) return crosspad::pitchedKitActive();
    if (native_sampler_active()) return native_sampler_pad_ready(padIdx);
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    return smpl_stream_ply_ready() && kit->pads[padIdx].sampleCount > 0;
#else
    return false;
#endif
}

// What a kit load cost the engine, for the load task's swap log. Cumulative
// except the gate peak, which SamplePlayerAccess clears on every read.
static void platform_engineLoadStats(crosspad_sampler::EngineLoadStats* out) {
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    out->gatedBlocks = crosspad::marcelGatedBlocks().load(std::memory_order_relaxed);
#else
    out->gatedBlocks = 0;   /* the native engine's render is never gated */
#endif
    crosspad::samplePlayerGateStats(&out->gateMaxUs, &out->gateTotalUs, &out->gateCount);
    /* Both engines' -- only one holds a kit, and each counts its own. */
    out->underruns   = g_engine_underrun.load() + native_sampler_underruns();
    out->noFreeBlock = g_engine_noblock.load();
    out->droppedHits = s_droppedHits.load(std::memory_order_relaxed);
    out->rtGapMaxUs  = g_rt_gap_max_us.exchange(0, std::memory_order_relaxed);
    out->lateHits    = s_lateHits.load(std::memory_order_relaxed);
    out->streamFopens = g_port_stream_fopen.load(std::memory_order_relaxed);
    out->bgPassMaxUs = g_bg_pass_max_us.exchange(0, std::memory_order_relaxed);
    out->fopenMaxUs  = g_port_fopen_max_us.exchange(0, std::memory_order_relaxed);
    out->streamKb    = g_port_stream_bytes.load(std::memory_order_relaxed) / 1024;
    out->loaderKb    = g_port_mutator_bytes.load(std::memory_order_relaxed) / 1024;
}

static void platform_reloadPads() {
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (!kit) return;
    if (kit->engine == crosspad::KitEngine::Pitched) {
        // Nothing per-pad to rebuild — the dirty flags a pad edit sets can only
        // mean volume/pan here, and those go straight to the engine.
        crosspad::pitchedKitApplyPadMix(*kit);
        for (uint8_t i = 0; i < KIT_PADS; ++i) kit->pads[i].dirty = false;
        return;
    }
    for (uint8_t i = 0; i < KIT_PADS; ++i) {
        if (!kit->pads[i].dirty) continue;
        platform_reloadPad(i);
        kit->pads[i].dirty = false;
    }
}

// ── Waveform platform callbacks ────────────────────────────────────────────
// Own fopen() handle, apart from the port's handles the streamers read
// through.

static uint32_t platform_liveScope(float* outData, uint32_t maxSamples,
                                   float* fundamentalHz, uint32_t* sampleRate) {
    if (fundamentalHz) *fundamentalHz = 0.0f; // a drum mix has no single note
    /* The engine that holds the kit: the native one's tap is its own channel. */
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
    return native_sampler_scope_snapshot(outData, maxSamples, sampleRate);
#else
    if (native_sampler_active()) return native_sampler_scope_snapshot(outData, maxSamples, sampleRate);
    auto* node = crosspad::getMarcelSampleNode();
    if (!node) {
        if (sampleRate) *sampleRate = 0;
        return 0;
    }
    if (sampleRate) *sampleRate = node->scopeTap().sampleRate();
    return node->scopeTap().snapshot(outData, maxSamples);
#endif
}

static void platform_setLiveScopeEnabled(bool enabled) {
    native_sampler_scope_enable(enabled);
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    auto* node = crosspad::getMarcelSampleNode();
    if (node) node->setScopeEnabled(enabled);
#endif
}

static FILE* openWavOnSd(const char* path) {
    char fullPath[300];
    const char* p = (path && path[0] == '/') ? path : path;
    snprintf(fullPath, sizeof(fullPath), "%s%s", BSP_SD_MOUNT_POINT, p);
    return fopen(fullPath, "rb");
}

static bool platform_wf_getFileInfo(const char* path, crosspad_sampler::AudioFileInfo* outInfo) {
    FILE* f = openWavOnSd(path);
    if (!f) return false;
    crosspad::WavLayout wav;
    bool ok = readWavLayout(f, wav);
    fclose(f);
    if (!ok) return false;
    outInfo->sampleCount = wav.dataBytes / wav.blockAlign;
    outInfo->sampleRate  = wav.rate;
    outInfo->channels    = static_cast<uint8_t>(wav.channels);
    outInfo->bitDepth    = static_cast<uint8_t>(wav.bits);
    return true;
}

static bool platform_wf_getWaveformSamples(const char* path, int16_t* outBuf,
                                            uint32_t outSamples,
                                            uint32_t startSample, uint32_t endSample) {
    FILE* f = openWavOnSd(path);
    if (!f) return false;
    crosspad::WavLayout wav;
    if (!readWavLayout(f, wav)) { fclose(f); return false; }
    const uint32_t sampleCount = wav.dataBytes / wav.blockAlign;

    if (endSample == 0 || endSample > sampleCount) endSample = sampleCount;
    if (startSample >= endSample) startSample = 0;
    uint32_t rangeLen = endSample - startSample;
    if (rangeLen == 0 || outSamples == 0) { fclose(f); return false; }

    // Read peak amplitude per bin for waveform display. This walks the whole
    // file, sixteen files per kit change, and it used to do so flat out — the
    // streamer starved for the length of it and every sounding voice had a
    // hole of up to 0.8 s right after a swap. It now yields to the streamer
    // the way the kit loader does, every few kilobytes.
    const uint32_t YIELD_EVERY_BYTES = 4096;
    uint8_t readBuf[512];
    // Frames per read, so a 24-bit stereo frame (6 B) fits the same buffer.
    const uint32_t READ_CHUNK = sizeof(readBuf) / wav.blockAlign;
    if (READ_CHUNK == 0) { fclose(f); return false; }
    uint32_t sinceYield = 0;

    for (uint32_t i = 0; i < outSamples; i++) {
        uint32_t binStart = startSample + (uint64_t)i * rangeLen / outSamples;
        uint32_t binEnd   = startSample + (uint64_t)(i + 1) * rangeLen / outSamples;
        if (binEnd > endSample) binEnd = endSample;

        /* A kit load that started after this read did is the one thing on the
         * card that must not wait: its pad comes back silent when this walk of
         * a whole file wins the descriptor. The loader checks before the read;
         * this catches a load that begins in the middle of one. */
        if (crosspad_sampler::kit_load_task_is_loading()) { fclose(f); return false; }

        fseek(f, wav.dataOffset + binStart * wav.blockAlign, SEEK_SET);

        int16_t peak = 0;
        uint32_t remaining = binEnd - binStart;

        while (remaining > 0) {
            uint32_t toRead = (remaining < READ_CHUNK) ? remaining : READ_CHUNK;
            size_t got = fread(readBuf, wav.blockAlign, toRead, f);
            /* A short read used to leave this bin at peak 0 and carry on: the
             * waveform was cached with flat gaps where the card had lost the
             * race, and being cached it was never read again. Fail instead —
             * the loader retries a request up to four times. */
            if (got == 0) { fclose(f); return false; }
            sinceYield += got * wav.blockAlign;
            if (sinceYield >= YIELD_EVERY_BYTES) {
                sinceYield = 0;
                sample_player_port_yield_to_streamer(3);
            }

            for (uint32_t s = 0; s < got; s++) {
                int16_t sample;
                if (wav.bits == 16) {
                    sample = static_cast<int16_t>(readBuf[s * wav.blockAlign] |
                                                  (readBuf[s * wav.blockAlign + 1] << 8));
                } else if (wav.bits == 24) {
                    sample = static_cast<int16_t>(readBuf[s * wav.blockAlign + 1] |
                                                  (readBuf[s * wav.blockAlign + 2] << 8));
                } else {
                    sample = static_cast<int16_t>((readBuf[s * wav.blockAlign] - 128) << 8);
                }
                if (abs(sample) > abs(peak)) peak = sample;
            }
            remaining -= got;
        }
        outBuf[i] = peak;
    }

    fclose(f);
    return true;
}

static bool platform_wf_writeFile(const char* path, const uint8_t* data, uint32_t size) {
    char fullPath[300];
    snprintf(fullPath, sizeof(fullPath), "%s%s", BSP_SD_MOUNT_POINT, path);
    FILE* f = fopen(fullPath, "wb");
    if (!f) return false;
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    return written == size;
}

static uint32_t platform_wf_readFile(const char* path, uint8_t* outBuf, uint32_t maxSize) {
    char fullPath[300];
    snprintf(fullPath, sizeof(fullPath), "%s%s", BSP_SD_MOUNT_POINT, path);
    FILE* f = fopen(fullPath, "rb");
    if (!f) return 0;
    size_t bytesRead = fread(outBuf, 1, maxSize, f);
    fclose(f);
    return static_cast<uint32_t>(bytesRead);
}

static bool platform_hasCacheFile(int kitId) {
    auto* mgr = crosspad::getKitManager();
    if (!mgr) return false;
    auto* kit = mgr->getKit(kitId);
    if (!kit || kit->path.empty()) return false;

    // kit->path is ".../MyKit/kit.json"; the cache sits beside it.
    const char* kitPath = kit->path.c_str();
    const char* lastSlash = strrchr(kitPath, '/');
    if (!lastSlash) return false;

    char cachePath[300];
    int dirLen = (int)(lastSlash - kitPath);
    snprintf(cachePath, sizeof(cachePath), "%s%.*s/waveform.cache",
             BSP_SD_MOUNT_POINT, dirLen, kitPath);

    FILE* f = fopen(cachePath, "rb");
    if (f) { fclose(f); return true; }
    return false;
}

static void* platform_wf_alloc(uint32_t bytes) {
    void* ptr = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!ptr) ptr = malloc(bytes);
    return ptr;
}

static void platform_wf_free(void* ptr) {
    free(ptr);
}

// ── EventBus → SampleStreamPlayer bridge ──────────────────────────────────
// SamplerPadLogic posts SamplePlayback on pad press; the platform turns that
// into an engine call.

static void* s_samplePlaybackSub = nullptr;
extern std::atomic<uint32_t> g_note_req_us[128];  /* sample_player_port.cpp */
/* The low 32 bits of esp_timer, never 0 (0 = no request outstanding). */
static inline uint32_t note_req_stamp()
{
    const uint32_t t = static_cast<uint32_t>(esp_timer_get_time());
    return t ? t : 1u;
}

static void onSamplePlaybackEvent(const void* event_data, crosspad::EventType, void*) {
    auto* data = static_cast<const crosspad::SamplePlaybackData*>(event_data);
    // Only react to playback requests, not engine feedback
    if (data->source == crosspad::EventSource::SamplePlayback) return;

    if (data->isOn) {
        ++g_hil_note_on_count;
        g_hil_last_note_on = (uint16_t)((data->note & 0x7F) << 8 | (data->velocity & 0x7F));
    }

    /* An app plays its own instrument on the engine; the kits are silent. */
    if (instrument_host_owns_engine()) return;

    /* Pitched kits: the pad index is the note field (SamplerPadLogic posts the
     * pad), and the kit's pad_notes map decides what it sounds. The prebuilt engine is
     * not involved and need not be up. */
    auto* kit = crosspad::getKitManager() ? crosspad::getKitManager()->getCurrentKit() : nullptr;
    if (kit && kit->engine == crosspad::KitEngine::Pitched) {
        /* No bank: the load failed, or a failed load dropped the previous one.
         * Falling through to the prebuilt engine is not a fallback — reloadPad() wiped every
         * slot for this kit and put nothing back, so the engine holds whatever
         * an earlier kit left where the wipe did not reach, keyed by a note map
         * that belongs to neither kit. Silence is the only correct sound.
         * PITCHED_STATUS's err= says why the bank is missing. */
        if (!crosspad::pitchedKitActive()) return;
        const uint8_t pad = data->note;
        if (pad >= KIT_PADS) return;
        auto& inst = crosspad::getPitchedSampleNode().instrument();
        /* 16 levels of pitch: the pad's note, moved. */
        const int moved = (int)kit->instrument.noteForPad(pad) + data->transpose;
        const uint8_t note = (uint8_t)(moved < 0 ? 0 : (moved > 127 ? 127 : moved));
        if (data->isOn) {
            g_note_req_us[data->note & 0x7F] = note_req_stamp();   // SMPL_LAST latency
            inst.noteOn(note, data->velocity ? data->velocity : 1, pad);
        } else {
            inst.noteOff(note);
        }
        return;
    }

    /* The native streaming engine, when it is the one holding the kit. Above
     * the readiness gate below, not under it: taking the kit into this engine
     * *stops* the prebuilt one, so `smpl_stream_ply_ready()` is false by
     * definition and the gate would swallow every note. Nothing below this
     * applies either -- the reload marker, the held hit and the per-pad voice
     * bitmap all belong to the prebuilt engine's bookkeeping. */
    if (native_sampler_active()) {
        if (data->isOn) {
            g_note_req_us[data->note & 0x7F] = note_req_stamp();   // SMPL_LAST latency
            lat_trace_note_handler(data->note);
            native_sampler_note_on(data->note, data->velocity ? data->velocity : 1, data->transpose, data->key);
            lat_trace_note_engine(data->note);
        } else {
            native_sampler_note_off(data->note, data->key);
        }
        return;
    }

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    /* The engine may not be up yet. Count the event regardless, so a bench
     * test still sees the pad arrive. */
    if (!smpl_stream_ply_ready()) return;

    /* The loader is rebuilding this pad without the render gate, which only
     * holds while no voice of it starts: a hit on it now would play a
     * half-built sample. Held instead, and played when the pad is back — or
     * counted as lost if the reload finished between the check and the hold. */
    if (data->isOn && data->note < KIT_PADS &&
        data->note == s_reloadingPad.load(std::memory_order_acquire)) {
        s_heldMs[data->note] = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_heldVelocity[data->note].exchange(data->velocity ? data->velocity : 1,
                                                std::memory_order_acq_rel)) ++g_late_lost;
        g_port_loader_hurry.store(true, std::memory_order_relaxed);
        if (data->note != s_reloadingPad.load(std::memory_order_acquire)) {
            // Reload ended under us; the marker will not fire it. Play now.
            const uint8_t vel = s_heldVelocity[data->note].exchange(0, std::memory_order_acq_rel);
            if (vel) { sample_player_pad_starting(data->note); SampleStreamPlayer_NoteOn(data->note, vel); }
        }
        return;
    }

    if (data->isOn) {
        // Velocity 0 is note-off to the engine, but a zero-velocity pad hit
        // must still sound.
        g_note_req_us[data->note & 0x7F] = note_req_stamp();   // SMPL_LAST latency
        if (data->note < KIT_PADS) sample_player_pad_starting(data->note);
        SampleStreamPlayer_NoteOn(data->note, data->velocity ? data->velocity : 1);
    } else {
        SampleStreamPlayer_NoteOff(data->note);
    }
#endif
}

/* A finger's hit straight from the pad logic (player.playPad), off the event
 * queue: what onSamplePlaybackEvent() does for the native engine, on the
 * caller's thread. Every other engine, and an app holding the engine, keeps
 * the event. */
static bool native_engine_plays_pads()
{
    if (instrument_host_owns_engine()) return false;
    auto* km = crosspad::getKitManager();
    auto* kit = km ? km->getCurrentKit() : nullptr;
    if (kit && kit->engine == crosspad::KitEngine::Pitched) return false;
    return native_sampler_active();
}

static bool platform_armPad(uint8_t touchPad, uint8_t soundPad, int8_t transpose, uint8_t key)
{
    return native_engine_plays_pads() && native_sampler_arm(touchPad, soundPad, transpose, key);
}

static bool platform_playPad(uint8_t touchPad, uint8_t soundPad, uint8_t velocity, int8_t transpose, uint8_t key)
{
    if (!native_engine_plays_pads()) return false;
    ++g_hil_note_on_count;
    g_hil_last_note_on = (uint16_t)((soundPad & 0x7F) << 8 | (velocity & 0x7F));
    g_note_req_us[soundPad & 0x7F] = note_req_stamp();   // SMPL_LAST latency
    native_sampler_play(touchPad, soundPad, velocity ? velocity : 1, transpose, key);
    return true;
}

void sampler_platform_port_init(const crosspad_sampler::KitSelectorAppCallbacks& kitSelector) {
    // Bridge EventBus SamplePlayback → SampleStreamPlayer
    s_samplePlaybackSub = crosspad::getEventBus().subscribe(
        crosspad::EventType::SamplePlayback, onSamplePlaybackEvent, nullptr);

    crosspad_sampler::SamplerPlatformCallbacks cb{};

    // Kit selector
    cb.kitSelector = kitSelector;

    // Audio loading
    cb.audio.startLoadKit  = platform_startLoadKit;
    cb.audio.onKitLoadFinished = platform_onKitLoadFinished;
    cb.player.engineHandlesFx  = platform_engineHandlesFx;
    cb.player.setPadPitch      = native_sampler_set_pad_pitch;
    cb.audio.reloadPad     = platform_reloadPad;
    cb.audio.reloadPadIfIdle = platform_reloadPadIfIdle;
    cb.audio.engineLoadStats = platform_engineLoadStats;
    cb.audio.padSounding   = platform_padSounding;
    cb.audio.padPlayable   = platform_padPlayable;
    cb.audio.reloadPads    = platform_reloadPads;
    cb.audio.hasCacheFile  = platform_hasCacheFile;
    cb.audio.kitRateReport = [](crosspad_sampler::KitRateReport* r) {
        return native_sampler_rate_report(&r->samples, &r->offRate, &r->rate, &r->engineRate);
    };
    cb.audio.convertHost   = kit_convert_host();

    // Player
    cb.player.noteOn         = platform_noteOn;
    cb.player.armPad         = platform_armPad;
    cb.player.playPad        = platform_playPad;
    cb.player.disarmPad      = native_sampler_disarm;
    cb.player.previewSample  = platform_previewSample;
    cb.player.getSampleInfo  = platform_getSampleInfo;
    cb.player.setVolume      = platform_setVolume;
    cb.player.setPan         = platform_setPan;
    cb.player.setPadMetering = native_sampler_set_pad_metering;
    cb.player.takePadPeak    = native_sampler_take_pad_peak;
    cb.player.setLoopEnd     = platform_setLoopEnd;
    cb.player.clearLoop      = platform_clearLoop;
    cb.player.wipeOut        = platform_wipeOut;
    cb.player.getFreeSlotCount = platform_getFreeSlotCount;
    cb.player.getLiveScopeData = platform_liveScope;
    cb.player.setLiveScopeEnabled = platform_setLiveScopeEnabled;

    // Waveform
    cb.waveform.getFileInfo        = platform_wf_getFileInfo;
    cb.waveform.getWaveformSamples = platform_wf_getWaveformSamples;
    cb.waveform.writeFile          = platform_wf_writeFile;
    cb.waveform.readFile           = platform_wf_readFile;
    cb.waveform.allocWaveformMem   = platform_wf_alloc;
    cb.waveform.freeWaveformMem    = platform_wf_free;

    crosspad_sampler::sampler_set_platform_callbacks(cb);

    // Audition while browsing: the explorer debounces the encoder, so this
    // plays whatever the selection rests on, and calls it with no path when
    // it closes. Nothing else registers a preview callback on this platform.
    file_explorer_set_preview_callback([](const char* path) {
        platform_previewSample(path, 0, 0xFFFFFFFFu, 100, 0);
    });

    // The sampler is the system instrument: pads play samples outside its app
    // too. Other instruments preempt it and hand it back on close.
    crosspad_sampler::sampler_service_init();
}
