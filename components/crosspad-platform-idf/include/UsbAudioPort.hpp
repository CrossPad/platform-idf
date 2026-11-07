// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
/**
 * @file UsbAudioPort.hpp
 * @brief Render-thread side of the USB audio port.
 *
 * Playback: `UsbPlaybackInput` is an IAudioInput over the host→board ring,
 * so host audio enters the mixer as an ordinary channel ("USB") and shares
 * the I2S buses with the sampler. Capture: `usbAudioPortCaptureTap()` is
 * called once per render block with the freshly read ADC blocks of both
 * codecs and feeds the board→host ring according to the capture source.
 *
 * Both rings are SPSC: playback is written by the UAC speaker task and read
 * here; capture is written here and read by the UAC mic task.
 */
#include "crosspad/synth/IAudioInput.hpp"
#include "usb_audio_port.h"
#include <atomic>
#include <cstdint>

namespace crosspad {

class UsbPlaybackInput : public IAudioInput {
public:
    uint32_t read(int16_t* interleaved, uint32_t frameCount) override;
    uint32_t getSampleRate() const override;
    uint32_t getBufferSize() const override;
    void getInputLevel(int16_t& left, int16_t& right) const override {
        left  = peakL_.exchange(0, std::memory_order_relaxed);
        right = peakR_.exchange(0, std::memory_order_relaxed);
    }

private:
    mutable std::atomic<int16_t> peakL_{0};
    mutable std::atomic<int16_t> peakR_{0};
};

UsbPlaybackInput& usbPlaybackInput();

/** Feed the board→host ring from this block's ADC reads. `rx0`/`rx1` may be
 *  null when that codec was not read; the tap then treats it as silence. */
void usbAudioPortCaptureTap(const int16_t* rx0, const int16_t* rx1, uint32_t frames);

/** Which codecs the tap needs this block, so the render loop reads only those.
 *  Answers false for USB_AUDIO_PORT_CAPTURE_SAMPLER: that source is digital and
 *  reads no ADC at all. */
bool usbAudioPortWantsCodec(int codec);

/** Feed the board->host ring from an already rendered mixer bus, for
 *  USB_AUDIO_PORT_CAPTURE_SAMPLER. The bus carries the pads only, so unlike the
 *  speaker DAC loop nothing the host plays -- a DAW's metronome, monitoring --
 *  can come back into the recording. Ignored unless that source is selected. */
void usbAudioPortCaptureBus(const int16_t* stereo, uint32_t frames);

} // namespace crosspad
