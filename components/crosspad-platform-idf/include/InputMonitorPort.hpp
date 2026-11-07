// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "InputPairPort.hpp"

#include <cstdint>

class AudioMixerEngine;

namespace crosspad {

/// The mixer's IN channels, heard: an input the mix sends anywhere holds the
/// ADC of the codec the input pair puts it on on the input itself (the mics,
/// a jack) instead of the DAC loop a capture preset may have left there, and
/// the render reads it in the USB audio profile too. Watches the routes, so
/// the Mixer, HIL and the desk a boot loads all count. @p chIn: the channel
/// of each InputSource, kMixerInvalidChannelId where the board has none.
void input_monitor_start(AudioMixerEngine& mixer, const uint8_t (&chIn)[kInputSources]);

/// RT: whether the mix wants codec @p codec's input this block.
bool input_monitor_wants_codec(int codec);

} // namespace crosspad
