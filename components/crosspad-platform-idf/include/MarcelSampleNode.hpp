// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file MarcelSampleNode.hpp
 * @brief IAudioNode wrapper around the prebuilt ml_sample_stream_player (planar
 *        int16 → float). Generator node (mix(), isGenerator()==true); SD
 *        streaming runs separately on the BG task.
 */

#include "crosspad/audio/IAudioNode.hpp"
#include "crosspad/audio/AudioFormatConvert.hpp"
#include "crosspad/audio/ScopeTap.hpp"
#include "SamplePlayerAccess.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

#include <ml_smpl_stream_ply.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include <new>

namespace crosspad {

// The render buffers handed to SampleStreamPly_Process(): 16-byte aligned, or
// the library's SIMD (PIE) mixing falls back to plain C for every voice. Same
// heap as a plain vector's (MALLOC_CAP_DEFAULT), only aligned.
template <typename T>
struct Align16Allocator {
    using value_type = T;
    Align16Allocator() = default;
    template <typename U> Align16Allocator(const Align16Allocator<U>&) {}
    T* allocate(size_t n) {
        void* p = heap_caps_aligned_alloc(16, n * sizeof(T), MALLOC_CAP_DEFAULT);
        if (!p) throw std::bad_alloc();
        return static_cast<T*>(p);
    }
    void deallocate(T* p, size_t) { heap_caps_free(p); }
    template <typename U> bool operator==(const Align16Allocator<U>&) const { return true; }
    template <typename U> bool operator!=(const Align16Allocator<U>&) const { return false; }
};

// Peak of what the sample player actually renders, before any mixer routing.
// Lets HIL tell "the engine produced no samples" apart from "the bus never
// carried them to an output".
inline std::atomic<int32_t>& marcelSamplePeak() {
    static std::atomic<int32_t> peak{0};
    return peak;
}

// Longest single SampleStreamPly_Process() call since last read, in µs. The
// render period is fixed by the I2S buffer; a call that exceeds it is the RT
// task hogging its core, which the library's own load counters do not report.
// Read and cleared by SMPL_PEAK.
inline std::atomic<uint32_t>& marcelSampleRtMaxUs() {
    static std::atomic<uint32_t> us{0};
    return us;
}

// The same, for the engine's error line: a reader of its own, so neither
// clears the other's peak.
inline std::atomic<uint32_t>& marcelSampleRtMaxLogUs() {
    static std::atomic<uint32_t> us{0};
    return us;
}

// Render blocks skipped because a mutator held the gate — each one is 64
// frames of silence on the bus. Read by the kit-load swap log: what a kit
// change cost in audible holes, counted where the hole is made.
inline std::atomic<uint32_t>& marcelGatedBlocks() {
    static std::atomic<uint32_t> blocks{0};
    return blocks;
}

// True only between SampleStreamPlayer_Init() and SampleStreamPlayer_DeInit().
// The engine is stopped for the whole time a pitched kit is current — its
// arena memory is that kit's sample bank — and every entry point into the prebuilt
// library has to check this first, including this node's mix(): the library's
// state has been torn down and its buffers handed back to the arena. Owned by
// main/audio/engine_handoff.cpp; it lives here so the node and the mixer can
// see it without depending on `main`.
inline std::atomic<bool>& marcelEngineReady() {
    static std::atomic<bool> ready{false};
    return ready;
}

class MarcelSampleNode : public IAudioNode {
public:
    MarcelSampleNode() { scopeTap_.setEnabled(false); }

    const char* name() const override { return "MarcelSampler"; }

    bool isGenerator() const override { return true; }

    void onPrepare(uint32_t maxFrames, uint32_t sampleRate) override {
        planarL_.resize(maxFrames);
        planarR_.resize(maxFrames);
        preparedMaxFrames_ = maxFrames;
        // Sized here, unconditionally, and never again: setScopeEnabled() runs
        // on the LVGL task and mix() reads scopeMono_ on the RT audio task
        // (the other core) with no lock. A resize() toggled from the UI while
        // mix() was mid-block raced its reallocation against a concurrent
        // read/write on the other core — silent heap corruption, not a click
        // that visibly does nothing. The tap's 32 kB ring is taken here too:
        // left to the first setScopeEnabled(true), it was allocated on the
        // waveform click, with a kit loaded and PSRAM too fragmented for it —
        // bad_alloc, abort() on the LVGL task.
        scopeMono_.resize(maxFrames);
        scopeTap_.reserve();
        scopeTap_.setSampleRate(sampleRate);
    }

    void onSampleRateChange(uint32_t newSr) override {
        // The prebuilt player has a fixed compile-time SR; no runtime reconfig.
        scopeTap_.setSampleRate(newSr);
    }

    ScopeTap& scopeTap() { return scopeTap_; }
    const ScopeTap& scopeTap() const { return scopeTap_; }

    void setScopeEnabled(bool enabled) {
        if (enabled && !scopeTap_.enabled()) {
            scopeTap_.clear();
            scopeTap_.setEnabled(true);
        } else if (!enabled) {
            scopeTap_.setEnabled(false);
        }
    }

    void mix(float* out, uint32_t frames) override {
        const uint32_t scopeFrames = scopeTap_.enabled()
            ? (frames < scopeMono_.size() ? frames : static_cast<uint32_t>(scopeMono_.size()))
            : 0;
        if (scopeFrames > 0) {
            std::fill_n(scopeMono_.data(), scopeFrames, 0.0f);
        }

        // Stopped engine: no library call at all, and nothing added to the bus.
        // The render gate below only excludes a *mutator*; a torn-down engine
        // has no voices to gate. Checked before the gate so a pitched kit costs
        // this node nothing per block.
        if (!marcelEngineReady().load(std::memory_order_acquire)) {
            dcXL_ = dcYL_ = dcXR_ = dcYR_ = 0.0f;
            if (scopeFrames > 0) scopeTap_.write(scopeMono_.data(), scopeFrames);
            return;
        }

        // Pre-size on first call; steady-state hits the resize() no-op.
        if (planarL_.size() < frames) planarL_.resize(frames);
        if (planarR_.size() < frames) planarR_.resize(frames);

        // The prebuilt player uses *= for some voices, so buffers must be cleared
        // each call to avoid stale residue.
        std::memset(planarL_.data(), 0, frames * sizeof(int16_t));
        std::memset(planarR_.data(), 0, frames * sizeof(int16_t));

        // Stand down while a kit load or a pad edit is restructuring voices.
        // Rendering through that window read buffers the mutator had already
        // freed — a null deref inside SampleStreamPly_Process, on this task.
        // Contributing nothing to the bus is the right answer: the kit change
        // has stopped every voice by now, so there is nothing to hear. Never
        // blocks; the gate is a flag, not a lock.
        if (!samplePlayerRenderBegin()) {
            marcelGatedBlocks().fetch_add(1, std::memory_order_relaxed);
            // Contributing nothing, so the DC blocker below must not carry its
            // x[n-1] across the gap and kick on the next call.
            dcXL_ = dcYL_ = dcXR_ = dcYR_ = 0.0f;
            if (scopeFrames > 0) scopeTap_.write(scopeMono_.data(), scopeFrames);
            return;
        }
        const int64_t t0 = esp_timer_get_time();
        SampleStreamPly_Process(planarL_.data(), planarR_.data(), frames);
        samplePlayerRenderEnd();
        const uint32_t dt = (uint32_t)(esp_timer_get_time() - t0);
        if (dt > marcelSampleRtMaxUs().load(std::memory_order_relaxed)) {
            marcelSampleRtMaxUs().store(dt, std::memory_order_relaxed);
        }
        if (dt > marcelSampleRtMaxLogUs().load(std::memory_order_relaxed)) {
            marcelSampleRtMaxLogUs().store(dt, std::memory_order_relaxed);
        }
        // Mono compatibility: the player writes only the L planar buffer for mono
        // WAV samples (R stays at the zero we memset). Detect that and mirror
        // L → R so mono kits play through both channels.
        bool stereoR = false;
        for (uint32_t i = 0; i < frames; ++i) {
            if (planarR_[i] != 0) { stereoR = true; break; }
        }
        const int16_t* srcR = stereoR ? planarR_.data() : planarL_.data();

        // DC blocker.
        //
        // When a one-shot voice reaches the end of its WAV the player stops
        // advancing but keeps emitting that file's last sample value, forever.
        // Measured on the DRUMS kit: one hit on pad 0 (playbackMode 0) and the
        // rendered peak parks on exactly 1118 and the output bus on exactly
        // 0.03412 — bit-identical for 40 s, and it never comes back down. That
        // is a constant, so it costs two things. The VU meter never falls to
        // zero — it sits wherever that offset lands on the scale — and the
        // speaker-amp gate reads a bus that never goes quiet, so the amp stays
        // powered on an idle board.
        //
        // The player is a prebuilt library, so the voice teardown cannot be
        // fixed where it happens; a one-pole high-pass on this node's own
        // contribution removes any constant regardless of which voice left it.
        // y[n] = x[n] - x[n-1] + R*y[n-1], R chosen for a ~5 Hz corner at
        // 48 kHz — well below anything musical, and -3 dB at 5 Hz.
        constexpr float kDcR = 0.99935f;

        int32_t peak = 0;
        for (uint32_t i = 0; i < frames; ++i) {
            const float xl = planarL_[i] * kInt16ScaleToFloat;
            const float xr = srcR[i]     * kInt16ScaleToFloat;

            const float yl = xl - dcXL_ + kDcR * dcYL_;
            const float yr = xr - dcXR_ + kDcR * dcYR_;
            dcXL_ = xl; dcYL_ = yl;
            dcXR_ = xr; dcYR_ = yr;

            // Peak after the filter: what this node actually contributes, so
            // SMPL_PEAK stops reporting a voice that has finished.
            const float al = yl < 0 ? -yl : yl;
            const float ar = yr < 0 ? -yr : yr;
            const float am = al > ar ? al : ar;
            const int32_t q = static_cast<int32_t>(am * kFloatScaleToInt16);
            if (q > peak) peak = q;

            out[i * 2 + 0] += yl;
            out[i * 2 + 1] += yr;
            if (i < scopeFrames) scopeMono_[i] = (yl + yr) * 0.5f;
        }
        if (scopeFrames > 0) scopeTap_.write(scopeMono_.data(), scopeFrames);
        if (peak > marcelSamplePeak().load(std::memory_order_relaxed)) {
            marcelSamplePeak().store(peak, std::memory_order_relaxed);
        }
    }

private:
    std::vector<int16_t, Align16Allocator<int16_t>> planarL_;
    std::vector<int16_t, Align16Allocator<int16_t>> planarR_;
    std::vector<float> scopeMono_;
    ScopeTap scopeTap_;
    uint32_t preparedMaxFrames_ = 0;

    // DC blocker state, one pair per channel.
    float dcXL_ = 0.0f, dcYL_ = 0.0f;
    float dcXR_ = 0.0f, dcYR_ = 0.0f;
};

} // namespace crosspad
