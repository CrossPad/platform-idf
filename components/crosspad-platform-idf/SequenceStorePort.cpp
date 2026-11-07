// SPDX-License-Identifier: GPL-3.0-or-later
//
// The card side of the sequencer: core's SongScenes keeps the scenes as the
// open song's (crosspad/song/SongScenes.hpp: which file, and how a song from
// before its own scenes is given the old shared ones); this sweeps it.
//
// Why a task rather than doing it where the events happen: the kit-load
// callback runs on the LVGL thread, and nothing slow belongs there; the poll
// that fires the pattern runs every 2 ms, and a card write would stall the
// beat. So both directions are requests this task picks up.

/* The sequencer rides the metronome, which is a mixer channel: no mixer, no
 * scenes and nothing to store. Same detection CrosspadPlatform.cpp uses. */
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)

#include "crosspad/song/SongScenes.hpp"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "bsp_sdmmc.h"

#include <cstdio>

crosspad::PatternSequencer& getSequencer();

namespace crosspad {

static const char* TAG = "seq_store";

/// What sequence_store_current_file() hands out, as long as it ever was.
static constexpr size_t kShownChars = 160;

static void scenes_log(bool warning, const char* msg)
{
    if (warning) ESP_LOGW(TAG, "%s", msg);
    else         ESP_LOGI(TAG, "%s", msg);
}

static SongScenes& scenes()
{
    static SongScenes s(getSequencer(), scenes_log);
    return s;
}

void sequence_store_kit_changed(const char* kitJsonPath)
{
    scenes().kitChanged(kitJsonPath ? kitJsonPath : "");
}

void sequence_store_save_soon() { scenes().saveSoon(); }

const char* sequence_store_current_file()
{
    static char copy[kShownChars];
    std::snprintf(copy, sizeof(copy), "%s", scenes().currentFile().c_str());
    return copy;
}

static void store_task(void*)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        /* No card, nothing to read or write. Without this the sweep reopens a
         * path on a mount that is gone twice a second and logs a warning each
         * time, for as long as the card is out. A take stays dirty and lands
         * the moment the card is back. */
        if (bsp_sdcard_get_handle() == nullptr) continue;
        scenes().sweep(BSP_SD_MOUNT_POINT);
    }
}

void sequence_store_start()
{
    static bool started = false;
    if (started) return;
    started = true;
    /* Priority 2 -- below the UI: a card write must never be what makes the
     * screen stutter. PSRAM stack, like the other storage helpers. */
    xTaskCreatePinnedToCoreWithCaps(store_task, "seq_store", 5000, nullptr, 2,
                                    nullptr, 0, MALLOC_CAP_SPIRAM);
}

} // namespace crosspad

#endif // __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
