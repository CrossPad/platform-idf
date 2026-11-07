// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace crosspad {

/// Someone else listening to the input the sample tools have selected --
/// the looper. One tap, set once at init: the block arrives whatever the
/// input is (mics, line, pads, USB) and whether or not a take is being
/// written, which is what makes the Recorder's input selector the looper's
/// too.
using CaptureTap = void (*)(void* ctx, const int16_t* stereo, uint32_t frames);
void sample_tools_set_capture_tap(CaptureTap fn, void* ctx);

} // namespace crosspad
