// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PitchedKitPort.hpp
 * @brief Turns a `KitEngine::Pitched` kit.json into a live SampleBank.
 *
 * Glue, not an engine: crosspad-core's `pitchedBuildBank()` reads the zone
 * WAVs, converts anything that is not at the engine rate and detects the roots
 * kit.json left as "auto", and `PitchedInstrument::installBank()` performs the
 * swap under its own render gate. What this file supplies is the board's half
 * of `PitchedLoadEnv` -- arena memory, the SD mount prefix, the I2S rate, an
 * ESP_LOG sink -- plus the status the CDC verbs report.
 *
 * Everything here is loader-thread code: it does SD I/O and allocates
 * hundreds of kB. In this firmware that thread is crosspad-sampler's
 * `kit_load` worker (8 kB stack, priority 5, core 0), reached through
 * `platform_reloadPad()`.
 */

#include "crosspad/control/PitchedStatus.hpp"
#include "crosspad/instrument/PitchedKitLoader.hpp"
#include "crosspad/instrument/SampleBank.hpp"
#include "crosspad/kit/KitInfo.hpp"

#include <cstddef>
#include <cstdint>

namespace crosspad {

/// Ceiling on one bank's resident PCM, checked before the first allocation.
///
/// The whole engine arena minus the reserve the resampler's transient needs
/// (source copy + overlap-add scratch, live at the same time as the zones
/// already built). The arena is what the prebuilt streamer occupies while
/// a sampler kit is current, so a pitched load stops it first and inherits
/// all of it.
size_t pitchedMaxBankBytes();

/// Everything PITCHED_STATUS reports about the installed bank, in one read:
/// core's portable status (the engine's counters through
/// control::readPitchedEngine(), the bank's shape from what install() kept)
/// plus what only this port knows.
struct PitchedKitStatus : control::PitchedStatus {
    /// Per zone, what the bank actually holds: the length of its PCM and the
    /// rate it ended up at (the loader converts when the engine's rate and
    /// the file's differ). Pitch Play sets loop points against these.
    uint32_t frames[SampleBank::kMaxZones] = {};
    uint32_t rates[SampleBank::kMaxZones]  = {};
    uint8_t  kitMaxVoices = 0;   ///< what the kit asked for (24 when silent)
    /// Why the last load failed, as one CDC field (spaces already replaced by
    /// underscores); empty once a load succeeds. Never null.
    const char* lastError = "";
};

/// How a bank is built beyond what the kit says.
struct PitchedLoadOptions {
    /// A zone longer than this is cut to it; 0 leaves the bank budget the limit.
    float maxZoneSeconds = 0.0f;
    /// Convert zones at another rate when loading (PitchedLoadEnv::convertRate):
    /// on this board ~2 s of CPU per second of audio. Off, a zone keeps its
    /// file's rate and plays stepped by the ratio.
    bool convertRate = true;
    /// Bring every zone's peak to -1 dBFS (PitchedLoadEnv::normalize).
    bool normalize = false;
    /// Built beside the kits' engines instead of in their arena: from the
    /// general PSRAM heap, budgeted against what is free there, and left
    /// alone by arena resets and kit loads. A song's Pitch track playing
    /// beside its kit tracks (instrument host).
    bool besideKits = false;
};

/// Build and install the bank described by @p kit. @p err carries a short
/// reason on failure (never null-terminated past @p errLen).
///
/// The build gives the CPU away every few milliseconds of work: a rate
/// conversion is seconds of CPU, and the thread it runs on may outrank the UI.
///
/// A failed load leaves **nothing** installed: whatever was playing is
/// unloaded too. The caller has already moved on to the new kit's pad_notes,
/// so a surviving previous bank would sound its zones under the wrong note
/// map — silence is the only honest answer.
bool pitchedKitLoad(const KitInfo& kit, char* err, size_t errLen,
                    const PitchedLoadOptions& options = PitchedLoadOptions());

/// What the last pitchedKitLoad() did beyond succeeding — zone count, and what
/// the bank budget cut short. Zeroed at the start of every load, so a caller
/// reading it after a load reads that load.
const PitchedLoadReport& pitchedKitLastReport();

/// Silence the engine and free the bank. Safe when nothing is loaded.
void pitchedKitUnload();

/// True while a bank is installed.
bool pitchedKitActive();

/// True while a bank is installed in the engine arena -- one the kits'
/// engines take turns with. A bank built besideKits is not: a kit load or an
/// arena reset leaves it playing.
bool pitchedBankInArena();

/// Re-apply per-pad volume/pan from @p kit without rebuilding the bank.
void pitchedKitApplyPadMix(const KitInfo& kit);

void pitchedKitGetStatus(PitchedKitStatus& out);


} // namespace crosspad
