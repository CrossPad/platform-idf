/**
 * @file audio_module.cpp
 * @brief Audio task glue. RT path drives getAudioModule().process() → I2S +
 *        VU meter; BG path drives the SD streaming passes.
 */


#include "IdfAudioModule.hpp"
#include "audio_module.h"
#include "audio_control.h"
#include "crosspad/audio/IAudioModule.hpp"
#include "crosspad/audio/AudioFormatConvert.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"
#include "CrosspadPlatform.hpp"
#include "crosspad-gui/components/vu_meter.h"
#include "SamplePlayerAccess.hpp"
#include "SamplerEnginePort.hpp"
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "MarcelSampleNode.hpp"
#include <ml_smpl_stream_ply.h>
#endif
#include "esp_timer.h"
#include <atomic>

std::atomic<uint32_t> g_bg_pass_max_us{0};   /* read-and-clear by the swap log */
std::atomic<uint32_t> g_bg_pass_last_us{0};  /* the loader paces itself on it */
/* Where the streamer is, for an engine error's line (sample_player_port.cpp):
 * in a pass, when it began (us, odd); between passes, when the last one
 * ended (even). An underrun reported mid-pass with the pass hundreds of ms
 * old is a refill stuck on the card; one reported long after the last pass
 * ended is a streamer that did not run. */
std::atomic<uint32_t> g_bg_pass_mark_us{0};
void sample_player_port_stream_let_go(void);   /* sample_player_port.cpp */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
void sample_player_port_pass_begin(void);
void sample_player_port_pass_end(uint32_t passUs);
#endif


bool AudioModule_RT_Setup(void)
{
    return AudioControl_Setup();
}

void AudioModule_RT_Process(void)
{
    auto& audio = crosspad::getAudioModule();
    audio.process();
    AudioControl_Process();

    float l = 0.0f, r = 0.0f;
    audio.getOutputLevel(0, l, r);
    crosspad_gui::vu_set_levels(
        static_cast<int16_t>(crosspad::clampFloat(l) * crosspad::kFloatScaleToInt16),
        static_cast<int16_t>(crosspad::clampFloat(r) * crosspad::kFloatScaleToInt16));

    // Same peak drives the speaker amp gate (Stm32Manager turns the amp off
    // after a quiet hold). Lock-free — no I2C on this path.
    crosspad::getStm32Manager().ampGate().submitLevel(l > r ? l : r);
}

bool AudioModule_RT_Offline(void)
{
    return static_cast<crosspad::IdfAudioModule&>(crosspad::getAudioModule()).processOffline();
}

bool AudioModule_BG_Setup(void)
{
    return true;
}

/* Stamps a streaming pass for the readers that give way to it
 * (sample_player_port_yield_to_streamer) and for the swap log. */
static void pass_begin(int64_t t0)
{
    g_bg_pass_mark_us.store((uint32_t)t0 | 1u, std::memory_order_relaxed);
}

static void pass_end(int64_t t0, int64_t t1)
{
    const uint32_t dt = (uint32_t)(t1 - t0);
    g_bg_pass_mark_us.store((uint32_t)t1 & ~1u, std::memory_order_relaxed);
    g_bg_pass_last_us.store(dt, std::memory_order_relaxed);
    if (dt > g_bg_pass_max_us.load(std::memory_order_relaxed)) {
        g_bg_pass_max_us.store(dt, std::memory_order_relaxed);
    }
}

void AudioModule_BG_Process(void)
{
    /* Between passes: the one moment this task's file slot is not being read. */
    sample_player_port_stream_let_go();

    /* The native engine's streaming pass. Outside the prebuilt library's
     * stream mutex on purpose: it shares nothing with it, only one of the two
     * ever holds a kit, and it must not wait on a teardown of the other.
     * Stamped like the prebuilt one's: until it was, a loader under play saw
     * no pass at all and never gave the card back. */
    if (native_sampler_active()) {
        const int64_t t0 = esp_timer_get_time();
        pass_begin(t0);
        native_sampler_service_io();
        pass_end(t0, esp_timer_get_time());
    } else {
        native_sampler_service_io();
    }

#if CONFIG_CROSSPAD_SAMPLER_MARCEL

    /* The refill reads through its own file slot, so a kit load does not hold
     * it up; only the engine teardown does, and for that it stands aside
     * rather than races: at prio 18 against a prio-5 loader, competing means
     * the loader never runs. See SamplePlayerAccess.hpp. */
    if (!crosspad::samplePlayerStreamBegin()) return;
    /* Read inside the stream mutex, not before it. marcel_engine_stop() lowers
     * the flag and *then* takes this mutex, so a refill that sees the flag up
     * is holding the one lock the teardown still has to acquire — the library
     * cannot be torn down under this call. Seeing it down means a pitched kit
     * owns the arena and there is nothing to stream. */
    if (crosspad::marcelEngineReady().load(std::memory_order_acquire)) {
        /* One pass is one SampleStreamPlayer_BkgProc(); how long the
         * longest pass took is how long a voice waited behind it. */
        const int64_t t0 = esp_timer_get_time();
        sample_player_port_pass_begin();
        pass_begin(t0);
        SampleStreamPlayer_BkgProc();
        const int64_t t1 = esp_timer_get_time();
        sample_player_port_pass_end((uint32_t)(t1 - t0));
        pass_end(t0, t1);
    }
    crosspad::samplePlayerStreamEnd();
#endif
}
