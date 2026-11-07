// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/kit/KitInfo.hpp"

#include <functional>

/**
 * @file song_sounds.h
 * @brief The open song's sounds on the board: core's SongSounds
 *        (crosspad/song/SongSounds.hpp) over the kit loader, the kits'
 *        engine and the instrument host.
 */

/// LVGL thread, once the kit loader's callbacks are in: every song that opens
/// from now on is followed, and the one open now is the first.
void song_sounds_start();

/// LVGL thread, from the kit load's finish: the open song takes the kit.
void song_sounds_kit_loaded();

/// The instrument host's task, after a request that succeeded: the open
/// song's Pitch track keeps @p spec -- one nobody saved goes to the song's
/// folder through @p save first (SongSounds::keepInstrument). @p changed:
/// built or saved from edits, so the Pitch track's freeze is of the old sound.
void song_sounds_keep_instrument(const crosspad::KitInfo& spec,
                                 const std::function<bool(const crosspad::KitInfo&)>& save,
                                 bool changed);
