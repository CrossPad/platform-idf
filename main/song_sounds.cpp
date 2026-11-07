// SPDX-License-Identifier: GPL-3.0-or-later
//
// The open song's sounds on the board: core's SongSounds (the kit and the
// Pitch instrument a song is made with, song_sounds.h) over the board's kit
// loader, kits' engine, instrument host and toast, stepped from an LVGL timer.

#include "song_sounds.h"

#include "SamplerEnginePort.hpp"
#include "SongEnginePort.hpp"
#include "engine_handoff.h"
#include "fw_updater.h"
#include "instrument_host.h"

#include "crosspad-gui/components/app_orchestrator.h"
#include "crosspad-gui/components/ui_toast.h"
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/logic/pad_color_manager.hpp"
#include "crosspad-sampler/ui/sampler_panel.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/SongSounds.hpp"

#include "bsp_sdmmc.h"
#include "esp_log.h"
#include "lvgl.h"

#include <atomic>
#include <cstring>

extern std::atomic<bool> g_kit_scan_done;   // main.cpp: the kit list is read

namespace {

constexpr const char* TAG = "song_sounds";
constexpr uint32_t kToastMs = 4000;

class BoardSongSoundsHost final : public crosspad::ISongSoundsHost {
public:
    std::string cardRoot() override { return BSP_SD_MOUNT_POINT; }

    /* Nor while the update check runs on WiFi at boot (Check at startup) or
     * on its daily round: a kit nobody asked for beside its TLS session was
     * more PSRAM than the board has. The app that opened the song abandons
     * the check, so this is the time it takes to wind down. */
    bool kitsSettled() override
    {
        return g_kit_scan_done.load() && !crosspad_sampler::kit_load_task_is_loading() &&
               !fw_updater_background_busy();
    }

    void loadKit(int kitId) override { crosspad_sampler::kit_load_task_start(false, kitId); }

    void unloadKit() override
    {
        native_sampler_release();
        if (!instrument_host_owns_engine()) {
            /* The bank, the heads and the rings back, the arena reset, and the
             * prebuilt streamer up again where the image has one. */
            kit_engines_idle();
        }
        if (crosspad::IKitManager* kits = crosspad::getKitManager()) kits->clearCurrentKit();
        crosspad_sampler::getPadColorManager().loadColorsFromKit(crosspad::KitInfo{});
    }

    /* Beside the kits counts too: it is what a song with no Pitch track
     * any more gives back (release()). */
    bool instrumentOwnsEngine() override { return instrument_host_owns_engine() || instrument_host_beside(); }

    const char* foregroundApp() override
    {
        crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
        return fg ? fg->getName() : nullptr;
    }

    void warn(const char* msg) override { ui_toast_show(msg, kToastMs, UI_TOAST_WARNING); }

    void log(bool warning, const char* msg) override
    {
        if (warning) ESP_LOGW(TAG, "%s", msg);
        else         ESP_LOGI(TAG, "%s", msg);
    }
};

crosspad::SongSounds& sounds()
{
    static BoardSongSoundsHost host;
    static crosspad::SongSounds s(host);
    return s;
}

lv_timer_t* s_timer = nullptr;

} // namespace

void song_sounds_start()
{
    if (s_timer) return;
    crosspad::getPlatformServices().songSounds = &sounds();
    crosspad::song_engine_set_open_handler([](const crosspad::AudioSong&) { sounds().songOpened(); });
    s_timer = lv_timer_create([](lv_timer_t*) {
        /* Pitch Play and the Sequencer borrow the engine themselves. */
        crosspad_gui::ILvglApp* fg = crosspad_gui::AppOrchestrator::getInstance().getForegroundApp();
        const char* name = fg ? fg->getName() : "";
        if (std::strcmp(name, "PitchPlay") != 0 && std::strcmp(name, "Sequencer") != 0) instrument_host_move_beside();
        sounds().step();
    }, crosspad::SongSounds::kStepMs, nullptr);
    sounds().start();
}

void song_sounds_kit_loaded() { sounds().kitLoaded(); }

void song_sounds_keep_instrument(const crosspad::KitInfo& spec,
                                 const std::function<bool(const crosspad::KitInfo&)>& save,
                                 bool changed)
{
    sounds().keepInstrument(spec, save, changed);
}
