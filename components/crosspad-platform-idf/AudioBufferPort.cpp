// SPDX-License-Identifier: GPL-3.0-or-later
//
// Settings -> Audio -> Buffer on this board: how many frames a render pass
// makes and how far ahead of the DAC it runs (IdfAudioModule.cpp, "How far
// ahead of the DAC the render runs"). The queue moves live without a frame
// lost; a new block size parks the render for one reconfiguration -- its
// buffers, the mixer's and every node's -- with the output faded out before
// and in after.

#include "AudioBufferPort.hpp"

#include "IdfAudioModule.hpp"
#include "IdfI2sInput.hpp"
#include "NvsSettingsStore.hpp"
#include "usb_audio_port.h"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/audio/IAudioBuffer.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"

extern "C" {
#include "i2s_driver.h"
}
#include "esp_log.h"

#include <atomic>

extern "C" void     audio_rt_set_queue_frames(uint32_t frames);
extern "C" uint32_t audio_rt_queue_frames(void);
extern "C" bool     audio_rt_queue_supported(uint32_t block, uint32_t queue);
/* main/audio/audio_task.cpp */
extern "C" bool     audio_rt_park(bool park, uint32_t timeout_ms);

namespace crosspad {
namespace {

constexpr const char* TAG = "AudioBuffer";

/* What the 6 x 64-frame DMA ring can run (audio_rt_queue_supported()). The
 * queue is from a pass starting to its first frame at the DAC; 384 is the
 * whole ring, the blocking write of old. A block of 128 or 256 does more
 * per pass for less overhead and waits longer for its turn, so it trades
 * latency for room under a heavy kit; 32 is not offered: at a 32-frame DMA
 * buffer the interrupts alone cost the render as many overruns again, for
 * the same latency at the same queue (measured 2026-10-16). */
constexpr IAudioBuffer::Choice kChoices[] = {
    {64, 192}, {64, 256}, {64, 320}, {64, 384},
    {128, 256}, {128, 384},
    {256, 384},
};
constexpr uint8_t kCount = sizeof(kChoices) / sizeof(kChoices[0]);

/* 64 frames, 4.4 ms -- the shallowest queue the pacer runs (three DMA
 * buffers). It was 7.3 ms while the render took twice as long: under
 * sustained overload (live_set "many") the codec then played 8-9 buffers
 * unrefilled at 7.3 ms and 32 at 5.8 and 4.4. With the render on core's dsp
 * kernels and the native sampler shedding voices over budget (16.10), none
 * at any of the three in live_set "many", nor under a 25 hits/s storm with
 * kit swaps, nor with USB audio both ways; a 48 kHz kit churned under the
 * storm, the worst case left, played 1-2 per 45 s at 4.4 ms against 0-1 at
 * 7.3. */
constexpr uint8_t kDefault = 0;

/* A render parked for longer than this has not come back from its pass. */
constexpr uint32_t kParkTimeoutMs = 200;

class IdfAudioBuffer final : public IAudioBuffer {
public:
    void bind(IdfAudioModule& module, AudioMixerEngine& mixer, IdfI2sInput* const* inputs, int n)
    {
        module_ = &module;
        mixer_ = &mixer;
        inputCount_ = n < kMaxInputs ? n : kMaxInputs;
        for (int i = 0; i < inputCount_; ++i) inputs_[i] = inputs[i];
    }

    uint8_t choices() const override { return kCount; }
    Choice  choice(uint8_t i) const override { return i < kCount ? kChoices[i] : Choice{}; }
    uint32_t sampleRate() const override { return bsp_i2s_get_sample_rate(); }

    uint8_t current() const override
    {
        return find(module_->getConfig().frameCount, audio_rt_queue_frames());
    }

    /// The choice for a block and queue, or the nearest: the same block's
    /// deepest, then the default.
    static uint8_t find(uint32_t block, uint32_t queue)
    {
        uint8_t deepest = kCount;
        for (uint8_t i = 0; i < kCount; ++i) {
            if (kChoices[i].blockFrames != block) continue;
            if (kChoices[i].queueFrames == queue) return i;
            if (deepest == kCount || kChoices[i].queueFrames > kChoices[deepest].queueFrames) deepest = i;
        }
        return deepest < kCount ? deepest : kDefault;
    }

    bool select(uint8_t i) override
    {
        if (i >= kCount || !module_) return false;
        const Choice c = kChoices[i];
        if (!audio_rt_queue_supported(c.blockFrames, c.queueFrames)) return false;
        /* The Settings row and AUDIO_BUF may ask at once; the second is
         * refused rather than queued behind a park. */
        if (busy_.exchange(true, std::memory_order_acq_rel)) return false;
        const bool ok = apply(c);
        busy_.store(false, std::memory_order_release);
        if (!ok) return false;
        if (auto* cfg = CrosspadSettings::getInstance()) {
            cfg->audioEngine.frameCount = c.blockFrames;
            cfg->audioEngine.queueFrames = c.queueFrames;
            cfg->saveTo(NvsSettingsStore::getInstance());
        }
        ESP_LOGI(TAG, "block %u, queue %u frames", (unsigned)c.blockFrames, (unsigned)c.queueFrames);
        return true;
    }

private:
    bool apply(const Choice& c)
    {
        if (c.blockFrames == module_->getConfig().frameCount) {
            audio_rt_set_queue_frames(c.queueFrames);
            return true;
        }
        /* Everything the render reads is sized by the block: its own buses,
         * the mixer's scratch and limiter, each node's onPrepare(), the IN
         * channels' hand-over blocks. None of it can change under a pass. */
        if (!audio_rt_park(true, kParkTimeoutMs)) {
            ESP_LOGW(TAG, "render did not park within %u ms", (unsigned)kParkTimeoutMs);
            return false;
        }
        module_->reconfigure(c.blockFrames);
        mixer_->setup(c.blockFrames, module_->getConfig().sampleRate, IdfAudioModule::NUM_OUTPUTS);
        for (int k = 0; k < inputCount_; ++k) inputs_[k]->setFrameCount(c.blockFrames);
        usb_audio_port_set_engine_frames(c.blockFrames);
        audio_rt_set_queue_frames(c.queueFrames);
        audio_rt_park(false, 0);
        return true;
    }

    static constexpr int kMaxInputs = 4;
    IdfAudioModule*   module_ = nullptr;
    AudioMixerEngine* mixer_ = nullptr;
    IdfI2sInput*      inputs_[kMaxInputs] = {};
    int               inputCount_ = 0;
    std::atomic<bool> busy_{false};
};

IdfAudioBuffer s_buffer;

} // namespace

void audio_buffer_init(IdfAudioModule& module, AudioMixerEngine& mixer,
                       IdfI2sInput* const* inputs, int inputCount)
{
    s_buffer.bind(module, mixer, inputs, inputCount);
    getPlatformServices().audioBuffer = &s_buffer;
    /* The block came from the settings with the module's setup; the queue is
     * the saved one if this block can run it, else the nearest choice's. */
    const auto* cfg = CrosspadSettings::getInstance();
    const uint32_t block = module.getConfig().frameCount;
    uint32_t queue = cfg ? cfg->audioEngine.queueFrames : 0;
    if (queue == 0) queue = kChoices[kDefault].queueFrames;
    if (!audio_rt_queue_supported(block, queue)) {
        queue = kChoices[IdfAudioBuffer::find(block, queue)].queueFrames;
        if (!audio_rt_queue_supported(block, queue)) queue = BSP_I2S_DMA_DESC_NUM * BSP_I2S_DMA_FRAME_NUM;
    }
    audio_rt_set_queue_frames(queue);
}

} // namespace crosspad
