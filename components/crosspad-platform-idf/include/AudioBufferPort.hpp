// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

class AudioMixerEngine;

namespace crosspad {

class IdfAudioModule;
class IdfI2sInput;

/// The render's block and output queue as Settings -> Audio -> Buffer
/// (IAudioBuffer), applied live; once at boot, after the mixer is set up,
/// with the inputs whose buffers follow the block. Registers itself in
/// PlatformServices and applies the saved choice's queue.
void audio_buffer_init(IdfAudioModule& module, AudioMixerEngine& mixer,
                       IdfI2sInput* const* inputs, int inputCount);

} // namespace crosspad
