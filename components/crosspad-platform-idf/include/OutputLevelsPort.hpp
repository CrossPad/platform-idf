// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// The board's physical outputs, as crosspad::IOutputLevels names them.
enum class BoardOutput : uint8_t { Speakers, Jack12, Jack34 };

/// The speakers' and jacks' analog levels (crosspad::IOutputLevels): each the
/// volume of the codec output pairs it is wired to (pinout BSP_OUT_*), kept in
/// CrosspadSettings::masterFX.outVolumeCodec. Registers
/// PlatformServices::outputLevels.
void output_levels_init();

/// Once per board, right after CrosspadSettings::loadFrom(): when
/// @p previousImage (the other slot's app version) is the Founders firmware,
/// Line out's kept level is rescaled from its x56/100 codec scale to 1:1, so
/// the jack stays as loud as it was. Behind an NVS marker.
void output_levels_migrate(const char* previousImage);

/// Every output to its kept level -- once the codecs are up.
void output_levels_apply();

/// Output @p out to @p level (0..100) in the hardware and in the settings
/// field, without a store write: the caller that owns saving saves.
void output_levels_set(BoardOutput out, uint8_t level);

} // namespace crosspad
