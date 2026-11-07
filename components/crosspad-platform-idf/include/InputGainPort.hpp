// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// The inputs' gain (crosspad::IInputGain) on this board, one per input: the
/// MicAmp PGA of the codec the input is on up to +24 dB, a digital gain above
/// it (the mics) or below 0 dB (the lines), both kept in NVS. Registers
/// PlatformServices::inputGain and applies the saved gains.
void input_gain_init();

/// The input pair moved: each codec's MicAmp takes the analog part of the
/// input now on it.
void input_gain_follow_pair();

/// RT: the digital part of the gain -- boost or cut -- of the input on codec @p codec onto one
/// block of its ADC, in place, and Line 3/4's board correction when that is the input. Every place a block is read calls it once,
/// before handing it on.
void input_gain_apply(int codec, int16_t* block, uint32_t frames);

} // namespace crosspad
