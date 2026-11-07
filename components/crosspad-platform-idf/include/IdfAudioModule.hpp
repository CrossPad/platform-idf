// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file IdfAudioModule.hpp
 * @brief ESP-IDF IAudioModule over dual ES8388 I2S streams.
 *
 * process() runs the node chain, or — when an AudioMixerEngine is attached via
 * setMixerEngine() — bypasses it and renders N float buses (one per ES8388),
 * matching the PC mixer topology. Call process() from a single audio thread
 * only; register nodes/channels at init, not against the render thread.
 */

#include "crosspad/audio/AbstractAudioModule.hpp"
#include "crosspad/audio/AudioFormatConvert.hpp"
#include "crosspad/audio/OutputEq.hpp"
#include "crosspad/dsp/AlignedVector.hpp"
#include "IdfI2sStream.hpp"

#include <atomic>
#include <cstring>
#include <vector>

class AudioMixerEngine;  // fwd; concrete type lives in crosspad-mixer

namespace crosspad {

class IdfAudioModule : public AbstractAudioModule {
public:
    /* Outputs 0 and 1 are the two codecs. Output CAPTURE_BUS has no stream --
     * getOutputStream() returns nullptr for it and the push loop skips it -- so
     * it costs one summing pass and is never heard. It exists so the USB mic can
     * record the pads alone: the sampler and the pitched engine are routed to it,
     * the host playback channel is not, which the speaker DAC loop could never
     * separate because the mixer had already summed them. */
    static constexpr uint8_t NUM_STREAM_OUTPUTS = 2;
    static constexpr uint8_t CAPTURE_BUS        = 2;
    static constexpr uint8_t NUM_OUTPUTS        = 3;

    IdfAudioModule()
        : stream0_(BSP_I2S_INTERFACE_0, 48000),
          stream1_(BSP_I2S_INTERFACE_1, 48000) {}

    /* Once, at boot: built for size (GCC `cold`) in the -O2 files that see
     * it -- the image is PSRAM too (CLAUDE.md). */
    __attribute__((cold)) bool setup(const AudioModuleConfig& config) override {
        if (!AbstractAudioModule::setup(config)) return false;
        stream0_.setSampleRate(config.sampleRate);
        stream1_.setSampleRate(config.sampleRate);
        const size_t stereo = static_cast<size_t>(config.frameCount) * 2;
        for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) {
            mixerBus_[s].assign(stereo, 0.0f);
            mixerScratchInt16_[s].assign(stereo, 0);
            captureScratch_[s].assign(stereo, 0);
        }
        padsTap_.assign(stereo, 0.0f);
        setupBoardFix(config.sampleRate);
        return true;
    }

    /**
     * @brief A new block size, live. Only while the render is parked: the
     *        render reads every buffer resized here. The mixer and its nodes
     *        are the caller's to prepare for the same size.
     */
    void reconfigure(uint32_t frameCount) {
        config_.frameCount = frameCount;
        const size_t stereo = static_cast<size_t>(frameCount) * 2;
        for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) {
            mixerBus_[s].assign(stereo, 0.0f);
            mixerScratchInt16_[s].assign(stereo, 0);
            captureScratch_[s].assign(stereo, 0);
        }
        padsTap_.assign(stereo, 0.0f);
        for (auto& slot : appNodes_)
            if (IAudioNode* n = slot.load(std::memory_order_acquire)) n->onPrepare(frameCount, config_.sampleRate);
    }

    /// With the mixer rendering, a node an app adds (DOOM's sound) is no mixer
    /// channel: it goes into a fixed table the render reads without a lock,
    /// and plays on the speakers' bus. Without the mixer, the node chain. An
    /// app may add one while the render runs; one it removes must outlive the
    /// render pass that may still be in it.
    void addNode(IAudioNode* node) override;
    void removeNode(IAudioNode* node) override;

    /// The mixer channels a bounce and the Recorder's PADS take record: the
    /// pads alone, as they are mixed, not the USB rec bus -- the Mixer can
    /// send anything to that one, the inputs included.
    /* Three, not two. The tap was Sampler + Pitched, from before a kit could
     * be played by the native engine; once it was, everything that records
     * this tap -- a song BOUNCE and the Recorder's PADS take -- wrote
     * perfectly formed WAVs of pure digital silence, and the Arranger played
     * them faithfully. An unused slot stays 0xFF, which renderTap skips. */
    void setPadChannels(uint8_t a, uint8_t b, uint8_t c = 0xFF)
    {
        padChannels_[0] = a; padChannels_[1] = b; padChannels_[2] = c;
    }

    IAudioStream* getOutputStream(uint8_t index) override {
        switch (index) {
            case 0: return &stream0_;
            case 1: return &stream1_;
            default: return nullptr;
        }
    }

    /// Plug an AudioMixerEngine in front of the audio output. Pass nullptr to
    /// fall back to the default node chain.
    void setMixerEngine(AudioMixerEngine* mixer) { mixer_ = mixer; }
    AudioMixerEngine* getMixerEngine() const { return mixer_; }

    void process() override {
        if (mixer_) processMixer();
        else        AbstractAudioModule::process();
    }

    /// The channel whose node runs the clock the sequencer plays from (the
    /// metronome, "Click"): an offline pass renders it with the pads tap.
    void setClockChannel(uint8_t ch) { clockChannel_ = ch; }
    /// RT, in place of process() while the outputs are off (audio_task's
    /// offline mode, a freeze): the clock and the pads tap's channels only,
    /// and the tap handed to a bounce -- nothing written to I2S, so nothing
    /// paces it. False without a mixer.
    bool processOffline();

    /// What output @p out's limiter ceiling is multiplied by so a peak at the
    /// ceiling stays under full scale after the board's fix on that output
    /// (codec 1's shelf boosts the top by up to its gain); 1 elsewhere.
    float boardFixHeadroom(uint8_t out) const;

private:
    void processMixer();   // out-of-line — needs the AudioMixerEngine type
    /// The shelf that undoes the low-pass on codec 1's jack
    /// (BSP_CODEC1_OUT_POLE_HZ); none where the board has none.
    __attribute__((cold)) void setupBoardFix(uint32_t rate);
    /// In the USB audio profile, the ADC reads the taps need -- the recorder,
    /// an IN channel the mix sends somewhere -- that the USB capture did not
    /// already make (see IdfI2sInput::setSuppressed).
    void readCodecsForTaps(uint32_t frames);

    IdfI2sStream stream0_;
    IdfI2sStream stream1_;
    AudioMixerEngine* mixer_ = nullptr;
    static constexpr size_t kAppNodes = 4;
    std::atomic<IAudioNode*> appNodes_[kAppNodes] = {};

    // Per-stream float buses for mixer-driven mode, on 16 bytes so the
    // render's kernels move them four floats a load (dsp/AudioKernels.hpp).
    dsp::AlignedVector<float> mixerBus_[NUM_OUTPUTS];
    dsp::AlignedVector<float> padsTap_;             // the pad channels, summed after render
    uint8_t              padChannels_[3] = {0xFF, 0xFF, 0xFF};
    uint8_t              clockChannel_ = 0xFF;
    std::vector<int16_t> mixerScratchInt16_[NUM_OUTPUTS];
    // One ADC block per codec for the USB capture tap (audio profile only).
    std::vector<int16_t> captureScratch_[NUM_OUTPUTS];
    EqChain              codec1Fix_;   // stream 1, after the mixer's own EQ, before the DAC
    float                codec1FixDb_ = 0.0f;   // its most boost (the shelf's gain, reached at Nyquist)
};

} // namespace crosspad
