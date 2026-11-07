// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file PitchedSampleNode.hpp
 * @brief The board's timing wrapper around crosspad-core's PitchedInstrument.
 *
 * The engine is an IAudioNode already and gates itself (see
 * PitchedInstrument::gateEnter / installBank), so all this adds is the two
 * things core cannot measure portably: **rt max**, the longest single mix()
 * since the last read (PITCHED_STATUS), and the **peak** this node rendered
 * (SMPL_PEAK). Same shape as marcelSampleRtMaxUs() / marcelSamplePeak() next
 * door.
 */

#include "crosspad/audio/AudioFormatConvert.hpp"
#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/dsp/AudioKernels.hpp"
#include "crosspad/instrument/PitchedInstrument.hpp"

#include <atomic>
#include <cstdint>

#include "esp_cpu.h"
#include "esp_rom_sys.h"

namespace crosspad {

/// Longest single PitchedInstrument::mix() since the last read, in µs.
inline std::atomic<uint32_t>& pitchedSampleRtMaxUs() {
    static std::atomic<uint32_t> us{0};
    return us;
}

/// Peak this node rendered since the last read, on the int16 scale — the
/// pitched half of what marcelSamplePeak() reports for the sampler, so
/// SMPL_PEAK answers for whichever engine is current.
inline std::atomic<int32_t>& pitchedSamplePeak() {
    static std::atomic<int32_t> peak{0};
    return peak;
}

/// The same peak for the song's meter of a Pitch track (ISongEnginePlatform
/// seqLevel(2)), taken on its own clock so the meter and SMPL_PEAK each see
/// every block.
inline std::atomic<int32_t>& pitchedMeterPeak() {
    static std::atomic<int32_t> peak{0};
    return peak;
}

class PitchedSampleNode : public IAudioNode {
public:
    /**
     * Voices one render pass has room for on core 1, whatever a kit or the
     * kit editor asks (PitchedInstrument::setVoiceCap; every setParams meets
     * it). Measured in the 16-note chord (2026-10-05): the node alone took
     * 1030 us at 15 voices and 1089 us at 16, ~65-69 us a voice; the rest of
     * the pass is ~550 us (CDC) to ~680 us (USB audio streaming) on average.
     * Ten voices are ~670 us: 84 % of the 1451 us block over the CDC base,
     * 93 % over the USB audio one, where the render's own overload guard
     * (audio_task.cpp: over 85 % a note takes a voice's place) holds it
     * lower. Twelve were ~820 us, over the block with the phone streaming.
     * Set once by CrosspadPlatform_Init; PITCHED_CAP sets it on hardware.
     *
     * Four since 2026-10-21, by design rather than by CPU: Pitch Play is one
     * of the kit types (Performance, Drums, Slice, Pitch) and is played as
     * chords of at most four notes beside the other engines.
     */
    static constexpr uint8_t kVoiceCap = 4;

    const char* name() const override { return "Pitched"; }
    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override {
        instrument_.onPrepare(maxFrames, sampleRate);
    }

    void mix(float* out, uint32_t frames) override {
        /* The cycle counter, not esp_timer: one RSR against ~235 cycles of a
         * SYSTIMER read and its wait, twice a block. */
        const uint32_t c0 = esp_cpu_get_cycle_count();
        instrument_.mix(out, frames);
        const uint32_t dt = (esp_cpu_get_cycle_count() - c0) / static_cast<uint32_t>(esp_rom_get_cpu_ticks_per_us());
        if (dt > pitchedSampleRtMaxUs().load(std::memory_order_relaxed)) {
            pitchedSampleRtMaxUs().store(dt, std::memory_order_relaxed);
        }

        // Nothing rendered: the block is as handed in, no pass over it.
        if (instrument_.lastMixSilent()) return;
        // The mixer clears a channel's scratch before calling it, so scanning
        // the buffer here measures this node alone. Without the mixer
        // component the chain shares one buffer and the figure is the bus.
        float l = 0.0f, r = 0.0f;
        dsp::peakStereo(out, frames, l, r);
        const int32_t q = static_cast<int32_t>(dsp::maxOf(l, r) * kFloatScaleToInt16);
        if (q > pitchedSamplePeak().load(std::memory_order_relaxed)) {
            pitchedSamplePeak().store(q, std::memory_order_relaxed);
        }
        if (q > pitchedMeterPeak().load(std::memory_order_relaxed)) {
            pitchedMeterPeak().store(q, std::memory_order_relaxed);
        }
    }

    bool lastMixSilent() const override { return instrument_.lastMixSilent(); }

    PitchedInstrument& instrument() { return instrument_; }

private:
    PitchedInstrument instrument_;
};

/// The one instance the mixer renders and the kit port drives.
inline PitchedSampleNode& getPitchedSampleNode() {
    static PitchedSampleNode node;
    return node;
}

} // namespace crosspad
