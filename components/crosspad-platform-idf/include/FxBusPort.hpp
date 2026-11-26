// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "InputPairPort.hpp"

#include <cstdint>

class AudioMixerEngine;

namespace crosspad {

/// The effects bus (crosspad::IFxBus) over the mixer's FX bus: registers
/// PlatformServices::fxBus. @p chIn: each InputSource's channel,
/// kMixerInvalidChannelId where the board has none; @p chUsb the host's
/// playback channel.
void fx_bus_init(AudioMixerEngine& mixer, const uint8_t (&chIn)[kInputSources], uint8_t chUsb);

} // namespace crosspad
