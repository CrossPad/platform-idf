// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file IdfI2sStream.hpp
 * @brief IAudioStream over a BSP I2S TX channel. ES8388 is 16-bit stereo at
 *        boot, so Int16 is the only supported format.
 */

#include "crosspad/audio/IAudioModule.hpp"
#include "crosspad/audio/AudioFormat.hpp"

extern "C" {
#include "i2s_driver.h"
}

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"

/// audio_rt only: microseconds the current pass spent waiting on I2S -- for
/// room in the TX DMA, the call that paces the render, and for the codec reads
/// of the USB audio profile (IdfAudioModule.cpp). audio_task.cpp subtracts it
/// from the pass.
extern uint32_t g_rt_io_wait_us;
#include <atomic>

namespace crosspad {

class IdfI2sStream : public IAudioStream {
public:
    static constexpr uint8_t kChannels = 2;       // ES8388 is configured stereo
    static constexpr AudioFormat kFormat = AudioFormat::Int16;

    IdfI2sStream(bsp_i2s_interface_t iface, uint32_t sampleRate)
        : iface_(iface), sampleRate_(sampleRate) {}

    uint32_t supportedFormats() const override { return audioFormatMask(kFormat); }

    /**
     * @brief One block to the codec. **This call is what paces the RT task.**
     *
     * Blocks for room in the TX DMA; that wait goes to g_rt_io_wait_us. Out
     * of line (IdfAudioModule.cpp): the render calls it from two places. A
     * refused write (ESP_ERR_INVALID_STATE on a channel being re-inited)
     * returns at once and is counted in writeErrors(), never swallowed: a
     * loop that is not being paced has to be visible (the engine branch).
     */
    uint32_t pushSamples(const void* samples, AudioFormat fmt, uint32_t frames) override;

    /// Writes the channel refused, and the last reason. For the diagnostics:
    /// a loop that is not being paced has to be visible, not inferred.
    uint32_t writeErrors() const { return writeErrors_.load(std::memory_order_relaxed); }
    int      lastError()  const { return lastError_.load(std::memory_order_relaxed); }

    bool isOpen() const override { return txHandle() != nullptr; }

    uint32_t getSampleRate() const override { return sampleRate_; }
    void setSampleRate(uint32_t sr) { sampleRate_ = sr; }

private:
    i2s_chan_handle_t txHandle() const {
        if (!cachedTx_) cachedTx_ = bsp_i2s_get_tx_handle(iface_);
        return cachedTx_;
    }

    bsp_i2s_interface_t iface_;
    uint32_t sampleRate_;
    mutable i2s_chan_handle_t cachedTx_ = nullptr;
    std::atomic<uint32_t> writeErrors_{0};
    std::atomic<int>      lastError_{0};
};

} // namespace crosspad
