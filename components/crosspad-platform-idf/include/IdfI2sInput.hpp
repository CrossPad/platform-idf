// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file IdfI2sInput.hpp
 * @brief IAudioInput for one of the board's inputs, read from the ES8388 RX
 *        I2S channel of whichever codec the input pair puts it on.
 *
 * ES8388 is initialized stereo 16-bit by the BSP and the RX channel is
 * enabled at i2s_driver init time. We poll-read from the RX FIFO each
 * audio frame; underruns return 0 frames and AudioInputNode zero-fills.
 *
 * Peak metering tracks the loudest absolute sample since the last
 * getInputLevel() read (matches PC PcAudioInput semantics so the mixer VU
 * displays input activity identically on both platforms).
 *
 * The channel is silent while its ADC is on the DAC loop a capture preset may
 * use (setLive): an IN channel sent to an output then feeds nothing back. An
 * input the pair leaves out reads nothing, so each codec has one reader.
 */

#include "InputGainPort.hpp"
#include "InputPairPort.hpp"
#include "SampleToolsPort.hpp"
#include "InputMonitorPort.hpp"
#include "crosspad/synth/IAudioInput.hpp"

extern "C" {
#include "i2s_driver.h"
}

#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace crosspad {

class IdfI2sInput : public IAudioInput {
public:
    IdfI2sInput(InputSource source, uint32_t sampleRate, uint32_t bufferFrames)
        : source_(source), sampleRate_(sampleRate), bufferFrames_(bufferFrames) {
        if (source < kInputSources) instances_[source] = this;
        offered_.assign(bufferFrames * 2, 0);
    }

    /// A new block size, while the render is parked (it reads offered_).
    void setFrameCount(uint32_t frames) {
        bufferFrames_ = frames;
        offered_.assign(frames * 2, 0);
        offeredFrames_ = 0;
    }

    /// Whether codec @p codec's ADC is on the input its IN channel carries
    /// rather than the DAC loop or another jack. Only then does the channel
    /// carry anything. Set by audio_route_control with the ADC; false at
    /// boot, when the BSP leaves both on LINE1.
    static void setLive(uint8_t codec, bool live) {
        if (codec < 2) live_[codec].store(live, std::memory_order_release);
    }

    /// USB audio profile: the render reads the ADCs itself after the mixer,
    /// one read a codec a block, and hands the block (gain applied) here for
    /// the next render's IN channel -- one block later than in the CDC
    /// profile, where the channel reads the I2S itself. RT thread only.
    static void offer(int codec, const int16_t* block, uint32_t frames) {
        const int src = input_pairs_source_on(codec);
        IdfI2sInput* in = src >= 0 && src < kInputSources ? instances_[src] : nullptr;
        if (!in || !block) return;
        in->offeredFrames_ = std::min<uint32_t>(frames, (uint32_t)(in->offered_.size() / 2));
        std::memcpy(in->offered_.data(), block, in->offeredFrames_ * 2 * sizeof(int16_t));
    }

    /// In USB audio mode the UAC bridge owns the RX channels; a second reader
    /// on the same channel starves it (the driver's per-channel semaphore
    /// admits one read at a time — the loser errors out and each side gets
    /// half the samples). usb_config_manager suppresses line-in reads for the
    /// duration of audio mode; the render hands the channel its own read
    /// instead (offer).
    static void setSuppressed(bool suppressed) {
        suppressed_.store(suppressed, std::memory_order_release);
    }

    uint32_t read(int16_t* interleaved, uint32_t frameCount) override {
        constexpr size_t frameBytes = 2 /*stereo*/ * sizeof(int16_t);
        const int codec = input_pairs_codec_of(source_);
        if (codec < 0) return 0;
        uint32_t got = 0;
        if (suppressed_.load(std::memory_order_acquire)) {
            /* The block the render read after the last mixer pass, if it
             * read this codec at all. */
            got = std::min(frameCount, offeredFrames_);
            if (got) std::memcpy(interleaved, offered_.data(), got * frameBytes);
            offeredFrames_ = 0;
        } else {
            /* Does anything want this codec's block at all?
             *
             * Asked *before* the read, which is the whole point. `live_` was
             * only consulted afterwards, so a codec sitting on the DAC loop --
             * which is both of them at boot, and what `route=000` on the IN
             * channel means -- was read, gain-corrected, handed to the sample
             * tools and then thrown away. Measured on the board: the "Mic"
             * channel cost **704 us of a 1450 us block** at the launcher with
             * no kit loaded, against 16 us for "Line" next to it, because the
             * read waits for RX DMA that the render has already run ahead of.
             * That was over half the render budget spent on a block nobody
             * was going to hear. */
            const bool wanted = live_[codec].load(std::memory_order_acquire) ||
                                crosspad::sample_tools_wants_codec(codec) ||
                                crosspad::input_monitor_wants_codec(codec);
            if (!wanted) return 0;

            i2s_chan_handle_t rx = bsp_i2s_get_rx_handle(static_cast<bsp_i2s_interface_t>(codec));
            if (!rx) return 0;
            size_t bytes_read = 0;
            /* One tick, not one frame period rounded up.
             *
             * The old bound worked out to 3 ms for a 64-frame block at
             * 44.1 kHz -- twice the whole budget of the pass it sits in -- on
             * the assumption that "in steady state this returns immediately".
             * It does not: the loop is paced by the TX write at the *end* of a
             * pass, so by the time the next pass asks for RX the block is not
             * there yet and this waits for it. A codec that stops clocking
             * would stall the render for 3 ms a block.
             *
             * A tick is the floor FreeRTOS can express and still be a wait;
             * a block that misses it is skipped, which costs the input one
             * block and the render nothing -- the same trade
             * `read_codec_block()` in IdfAudioModule.cpp already makes, for
             * the same reason, and counts rather than swallows. */
            esp_err_t err = i2s_channel_read(rx, interleaved, frameCount * frameBytes,
                                             &bytes_read, 1);
            if (err == ESP_ERR_TIMEOUT) skips_.fetch_add(1, std::memory_order_relaxed);
            if (err != ESP_OK && err != ESP_ERR_TIMEOUT) return 0;
            got = static_cast<uint32_t>(bytes_read / frameBytes);
            /* The one read of this codec this block: the recorder takes it here
             * rather than reading the ADC a second time (see setSuppressed). */
            if (got) crosspad::input_gain_apply(codec, interleaved, got);
            if (got) crosspad::sample_tools_rt_input(codec, interleaved, got);
        }
        /* On the DAC loop: nothing, so no send of this channel is a feedback. */
        if (!got || !live_[codec].load(std::memory_order_acquire)) return 0;

        int16_t pl = 0, pr = 0;
        for (uint32_t i = 0; i < got; ++i) {
            int16_t l = interleaved[i * 2];
            int16_t r = interleaved[i * 2 + 1];
            int16_t al = l < 0 ? -l : l;
            int16_t ar = r < 0 ? -r : r;
            if (al > pl) pl = al;
            if (ar > pr) pr = ar;
        }
        if (pl > peakL_.load(std::memory_order_relaxed)) peakL_.store(pl, std::memory_order_relaxed);
        if (pr > peakR_.load(std::memory_order_relaxed)) peakR_.store(pr, std::memory_order_relaxed);
        return got;
    }

    uint32_t getSampleRate() const override { return sampleRate_; }
    uint32_t getBufferSize() const override { return bufferFrames_; }

    void getInputLevel(int16_t& left, int16_t& right) const override {
        left  = peakL_.exchange(0, std::memory_order_relaxed);
        right = peakR_.exchange(0, std::memory_order_relaxed);
    }

    void setSampleRate(uint32_t sr) { sampleRate_ = sr; }

    /// Blocks an input read asked for and did not get in a tick. Shared by
    /// both codecs; a diagnostic, not a per-channel figure.
    static uint32_t skips() { return skips_.load(std::memory_order_relaxed); }

private:
    inline static std::atomic<bool> suppressed_{false};
    inline static std::atomic<bool> live_[2] = {};
    inline static IdfI2sInput* instances_[kInputSources] = {};
    inline static std::atomic<uint32_t> skips_{0};

    std::vector<int16_t> offered_;         // USB audio profile: the block handed on
    uint32_t offeredFrames_ = 0;

    InputSource source_;
    uint32_t sampleRate_;
    uint32_t bufferFrames_;

    mutable std::atomic<int16_t> peakL_{0};
    mutable std::atomic<int16_t> peakR_{0};
};

} // namespace crosspad
