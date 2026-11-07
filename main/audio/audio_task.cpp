/**
 * @file audio_task.cpp
 * @brief FreeRTOS RT (audio) + BG (SD streaming) tasks.
 */

#include "CrosspadPlatform.hpp"
#include "EspEventBus.hpp"
#include "SongEnginePort.hpp"
#include "audio_task.h"
#include "usb_audio_port.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "audio_module.h"
#include "SamplePlayerAccess.hpp"
#include "SamplerEnginePort.hpp"
#include "IdfI2sStream.hpp"
#include "crosspad/instrument/PitchedInstrument.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/settings/CrosspadSettings.hpp"
#include "lat_trace.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include <atomic>

/* components/crosspad-platform-idf/IdfAudioModule.cpp. Whether the pass that
 * just ran handed its block to an output that will make the next one wait. */
extern "C" bool audio_rt_pass_paced(void);
extern "C" uint32_t native_sampler_stream_lead(void);
extern "C" void audio_rt_last_write(uint32_t* write_done_us, uint32_t* dac_us);
extern "C" uint32_t audio_rt_pacer_wait_us(void);
extern "C" uint32_t audio_rt_block_frames(void);
extern "C" void audio_rt_fade_next(int dir);
extern "C" void audio_rt_pacer_restart(void);


namespace
{
    constexpr uint32_t RT_TASK_STACK_SIZE = 8192; // 4096;
    constexpr uint32_t BG_TASK_STACK_SIZE = 4096;  // measured high-water: ~2.1 kB used

    constexpr UBaseType_t RT_TASK_PRIORITY = 19; // configMAX_PRIORITIES - 2;
    constexpr UBaseType_t BG_TASK_PRIORITY = 18; // configMAX_PRIORITIES - 3;

    constexpr BaseType_t RT_TASK_CORE = 1;
    constexpr BaseType_t BG_TASK_CORE = 0;

    constexpr TickType_t WAIT_FOR_TASK_TERMINATION_MS = 10;
}


static std::atomic<bool> g_run{true};
static std::atomic<bool> g_suspended{false};
static std::atomic<bool> g_ready{false};
/* audio_rt_park(): asked, and answered by the render once it has stopped --
 * with the number of the request it stopped for, so an answer left over from
 * the previous park can never pass for this one. */
static std::atomic<bool>     g_parkReq{false};
static std::atomic<uint32_t> g_parkSeq{0};
static std::atomic<uint32_t> g_parkedSeq{0};

static const char* TAG = "AudioTasks";
static TaskHandle_t g_rtTaskHandle = nullptr;
static TaskHandle_t g_bgTaskHandle = nullptr;

// Static stacks and TCBs: with NimBLE up, the heap at this point can have
// 11 kB free and no 8 kB block, and a task that does not exist plays no audio.
// .bss is laid out at link time, so these never depend on what allocated first.
static StackType_t  s_rtStack[RT_TASK_STACK_SIZE];
static StaticTask_t s_rtTcb;
/* audio_bg's in PSRAM: card reads and the arena, never a flash write, and its
 * 4 kB of internal RAM pay for the render's buffers there. Still static. */
EXT_RAM_BSS_ATTR static StackType_t s_bgStack[BG_TASK_STACK_SIZE];
static StaticTask_t s_bgTcb;


std::atomic<uint32_t> g_rt_gap_max_us{0};   /* read-and-clear by the swap log */
/* A pass's own work: its length less the time it waited on the I2S DMA
 * (g_rt_io_wait_us: room in TX, and the codec reads of the USB audio
 * profile). Over a block, the next one is late and the DMA's 4 x 64 frames
 * are what is left. One copy of the longest per reader (audio_task.h). */
static std::atomic<uint32_t> s_rtBusyMax[AUDIO_RT_BUSY_READERS];
std::atomic<uint32_t> g_rt_overruns{0};     /* passes busy for longer than a block, since boot */
/* Every pass's busy time summed, and how many, for audio_rt_busy_avg(): the
 * transport bar's CPU readout is the average, the peak beside it. */
static std::atomic<uint64_t> s_rtBusySum{0};
static std::atomic<uint32_t> s_rtBusyPasses{0};
static std::atomic<uint32_t> s_blockUs{0};

/* The sample engines' polyphony against the measured budget. Their voice
 * caps (PitchedSampleNode::kVoiceCap, the native sampler's maxVoices) are a
 * guess at what a pass has room for -- sixteen native voices of a 48 kHz kit,
 * every one interpolating, are ~1.1 ms of a 1.45 ms block on their own; this
 * is the pass itself, with whatever else it carries -- the USB audio streams,
 * the song, the metronome. A pass averaging over 85 % of a block holds the
 * polyphony where it is (PitchedInstrument / SamplerEngine::setOverloaded: a
 * new note takes a voice's place) until the average is back under 75 %. Over
 * a whole block the render never waits for the DMA again, core 1's lower
 * priorities -- the event task that plays and releases the notes -- stop
 * running, so sounding voices are let go one at a time as well
 * (kShedVoiceEvery) until the pass fits the block again. A running
 * average that weighs each pass a quarter: ~4 passes to cross, so one long
 * pass alone does not cut a tail. */
/* Offline (audio_rt_offline()): asked for, and taken -- what the asker waits on. */
static std::atomic<bool> g_offlineReq{false};
static std::atomic<bool> g_offlineOn{false};
/* An offline pass waits while a streaming voice has fewer frames than this
 * delivered ahead of it: two of the streamer's 4096-frame blocks, what one of
 * its passes refills a voice by. */
static constexpr uint32_t kOfflineLead = 8192;
/* ...and gives core 1 a tick every so many passes: IDLE1 feeds the watchdog. */
static constexpr uint32_t kOfflineYieldEvery = 8;
/* ...and waits for the event task no longer than this many ticks a pass: a
 * bus that never empties must not hold the freeze. */
static constexpr uint32_t kOfflineEventWaitMax = 50;
/* ...and waits for a voice's lead no longer than this many ticks without it
 * growing. A voice the streamer has no ring for (a kit slot's pool is four
 * rings, fixed) only lets go of it once it plays within kRingRetryFrames
 * (4096) of its head's end -- which, short of kOfflineLead, the render never
 * reached: Song 15's freeze sat at frame 803829 until "timed out", every try
 * (30.10.2026). Past this the render goes on a pass a tick, about real time,
 * so the voice ends as it would there and the others' rings keep up. Longer
 * than the card's stalls under a freeze (48 ms the worst piece measured). */
static constexpr uint32_t kOfflineStreamWaitMax = 300;
/* Off the clock only while nobody listens: a player's press within this long
 * keeps a take in real time, and one during it brings it back there
 * (audio_rt_note_live()). The outputs are silent off the clock, and a press
 * lands wherever the render happens to be. */
static constexpr int64_t kLiveQuietUs = 2000000;
static std::atomic<int64_t> g_liveAtUs{INT64_MIN / 2};
static std::atomic<bool> g_offlineDrop{false};

static constexpr uint32_t kShedOnPct = 85;
static constexpr uint32_t kShedOffPct = 75;
/* Over the whole block and staying there, holding the polyphony is not
 * enough: loops never end, and sixteen pitched ones held core 1 until the
 * task watchdog fired on IDLE1. Then one voice is let go (faded, as a steal)
 * every this many passes until the average is back inside the block --
 * ~12 ms apart, time for a fade and the average to see it. */
static constexpr uint32_t kShedVoiceEvery = 8;

void AudioTask_RT(void *pvParameters)
{
    (void)pvParameters;

    if (!AudioModule_RT_Setup())
    {
        vTaskDelete(nullptr);
        return;
    }

    g_ready.store(true, std::memory_order_release);
    const auto& engine = crosspad::CrosspadSettings::getInstance()->audioEngine;
    /* The budget follows the block the render really makes: it changes live
     * (Settings -> Audio -> Buffer). */
    uint32_t blockFrames = 0, blockUs = 0, shedOnUs = 0, shedOffUs = 0;
    crosspad::PitchedInstrument* pitched = crosspad::getPlatformServices().pitchedInstrument;
    uint32_t busyAvg = 0;
    bool shed = false;
    uint32_t overPasses = 0;   /* passes over the whole block in a row, towards the next shed voice */
    bool fadedOut = false, parked = false;
    bool offFaded = false, offline = false;
    uint32_t offPasses = 0, offEventWaits = 0;
    uint32_t offStreamWaits = 0, offLead = 0;   /* ticks the lowest lead has not grown, and what it was */
    bool offPaced = false;                      /* gave up waiting on it: a pass a tick until it is back */
    int64_t lastPassUs = 0;
    while (true) /* todo add shutdown mechanism */
    {
        /* Offline: one pass faded to silence, then the clock and the pads tap
         * only, written nowhere -- nothing paces it but what it waits on. */
        if (g_offlineReq.load(std::memory_order_acquire) && !g_suspended.load(std::memory_order_acquire)) {
            if (offline && (g_offlineDrop.exchange(false, std::memory_order_acq_rel) ||
                            usb_audio_port_playback_streaming())) {
                /* Someone is listening -- a press, or a host stream started:
                 * the rest of the take in real time, the outputs back. */
                g_offlineReq.store(false, std::memory_order_release);
                crosspad::sequencer_poll_from_render(false);
                ESP_LOGI(TAG, "off the clock -> real time: someone is playing");
                continue;
            }
            if (offline) {
                /* The notes the last pass's sequencer posted are through the
                 * event task first -- the Sampler's take plays from there --
                 * as they are by the next block in real time. */
                const bool notes = crosspad::esp_event_bus_pending() && offEventWaits < kOfflineEventWaitMax;
                const uint32_t lead = native_sampler_stream_lead();
                const bool shortLead = lead < kOfflineLead;
                if (!shortLead) {
                    offPaced = false;
                    offStreamWaits = 0;
                } else if (!offPaced) {
                    if (lead > offLead) offStreamWaits = 0;
                    if (++offStreamWaits > kOfflineStreamWaitMax) {
                        offPaced = true;
                        ESP_LOGW(TAG, "offline: a voice's lead stuck at %u frames for %u ms, rendering on in real time",
                                 (unsigned)lead, (unsigned)kOfflineStreamWaitMax);
                    }
                }
                offLead = lead;
                if (notes || crosspad::song_engine_capture_backlogged() || (shortLead && !offPaced)) {
                    if (notes) ++offEventWaits;
                    vTaskDelay(1);
                    continue;
                }
                offEventWaits = 0;
                AudioModule_RT_Offline();
                crosspad::sequencer_poll_now();     // this pass's notes, for the next one
                if (offPaced || ++offPasses % kOfflineYieldEvery == 0) vTaskDelay(1);
                continue;
            }
            if (offFaded) {
                offline = true;
                g_offlineOn.store(true, std::memory_order_release);
                continue;
            }
            audio_rt_fade_next(-1);
            offFaded = true;
        } else if (offline || offFaded) {
            if (offline) audio_rt_pacer_restart();
            audio_rt_fade_next(+1);
            offline = offFaded = offPaced = false;
            offStreamWaits = 0;
            lastPassUs = 0;
            g_offlineOn.store(false, std::memory_order_release);
        }
        if (g_suspended.load(std::memory_order_acquire))
        {
            g_parkedSeq.store(g_parkSeq.load(std::memory_order_acquire), std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        /* Parked for a reconfiguration: one more pass, faded to silence, then
         * nothing until released; the first pass after fades back in. */
        if (g_parkReq.load(std::memory_order_acquire)) {
            /* Answered already -- also from the suspended branch above, which
             * then must not be followed by a pass under the reconfiguration. */
            const uint32_t seq = g_parkSeq.load(std::memory_order_acquire);
            if (fadedOut || g_parkedSeq.load(std::memory_order_acquire) == seq) {
                g_parkedSeq.store(seq, std::memory_order_release);
                parked = true;
                vTaskDelay(1);
                lastPassUs = 0;
                continue;
            }
            audio_rt_fade_next(-1);
            fadedOut = true;
        } else if (parked || fadedOut) {
            /* Back -- or a request withdrawn after its faded pass. */
            if (parked) audio_rt_pacer_restart();
            if (fadedOut) audio_rt_fade_next(+1);
            parked = fadedOut = false;
        }
        const uint32_t frames = audio_rt_block_frames();
        if (frames != blockFrames) {
            blockFrames = frames;
            blockUs = (uint32_t)((uint64_t)(frames ? frames : engine.frameCount) * 1000000u / engine.sampleRate);
            shedOnUs = blockUs * kShedOnPct / 100;
            shedOffUs = blockUs * kShedOffPct / 100;
            s_blockUs.store(blockUs, std::memory_order_relaxed);
        }
        /* Longest wait between two render passes, for the kit-load swap log:
         * silence with no gated block and no underrun is this task not being
         * called — a lock it waited on, or the flash cache going away. */
        const int64_t now = esp_timer_get_time();
        if (lastPassUs != 0) {
            /* Less the last pass's wait for its DMA buffer: a render paced
             * ahead of the DAC by fewer buffers waits there on purpose, up to
             * several blocks while it settles to its depth. */
            const uint32_t gap = (uint32_t)(now - lastPassUs) - audio_rt_pacer_wait_us();
            if (gap > g_rt_gap_max_us.load(std::memory_order_relaxed)) {
                g_rt_gap_max_us.store(gap, std::memory_order_relaxed);
            }
        }
        lastPassUs = now;
        g_rt_io_wait_us = 0;
        AudioModule_RT_Process();
        uint32_t writeDoneUs, dacUs;
        audio_rt_last_write(&writeDoneUs, &dacUs);
        lat_trace_pass_end(writeDoneUs, dacUs);
        const uint32_t busy = (uint32_t)(esp_timer_get_time() - now) - g_rt_io_wait_us;
        if (busy > blockUs) g_rt_overruns.fetch_add(1, std::memory_order_relaxed);
        for (auto &m : s_rtBusyMax) {
            if (busy > m.load(std::memory_order_relaxed)) m.store(busy, std::memory_order_relaxed);
        }
        s_rtBusySum.fetch_add(busy, std::memory_order_relaxed);
        s_rtBusyPasses.fetch_add(1, std::memory_order_relaxed);
        busyAvg = busyAvg - busyAvg / 4 + busy / 4;
        const bool over = shed ? busyAvg >= shedOffUs : busyAvg > shedOnUs;
        if (over != shed) {
            shed = over;
            if (pitched) pitched->setOverloaded(over);
            native_sampler_set_overloaded(over);
        }
        if (busyAvg > blockUs) {
            if (++overPasses >= kShedVoiceEvery) {
                overPasses = 0;
                native_sampler_shed_voice();
            }
        } else {
            overPasses = 0;
        }
        /* The pacer is the blocking I2S write inside the pass, not this yield:
         * at priority 19 on core 1 there is nothing above to yield *to*, so
         * taskYIELD() puts this task straight back on the CPU. That is right
         * while a codec is taking the blocks -- the write waits for DMA room,
         * 8.7 ms of it -- and catastrophic when none is: the loop then renders
         * as fast as the core allows, the sequencer's beat (counted in frames
         * rendered) runs away, the USB capture ring drops blocks faster than
         * they are made, and IDLE1 never runs, which is a task watchdog with
         * audio_rt in the backtrace. Measured on the board in the USB-audio
         * profile, where the channels are re-enabled under the render.
         *
         * So: yield while something is consuming, sleep a tick when nothing
         * is. A tick is 1 ms against 8.7 ms of DMA, so it cannot cause an
         * underrun on a board that *is* playing. */
        if (audio_rt_pass_paced()) taskYIELD();
        else                       vTaskDelay(1);
    }

    vTaskDelete(nullptr);
}

void AudioTask_BG(void *pvParameters)
{
    (void)pvParameters;

    if (!AudioModule_BG_Setup())
    {
        vTaskDelete(nullptr);
        return;
    }

    while (true) /* todo add shutdown mechanism */
    {
        AudioModule_BG_Process();
        vTaskDelay(1);
    }

    vTaskDelete(nullptr);
}

/* In the USB audio profile the console lines (the engine's error line, the
 * UAC speaker's 'SPK #') are all there is. */
extern "C" uint32_t audio_rt_busy_max(int reader, uint32_t *idle1Pct)
{
    static uint32_t s_idle, s_us;
    if (idle1Pct) {
        const uint32_t idle = ulTaskGetIdleRunTimeCounterForCore(1);
        const uint32_t us = (uint32_t)esp_timer_get_time();
        const uint32_t span = (us - s_us) / 100;
        *idle1Pct = (s_us && span) ? (idle - s_idle) / span : 0;
        s_idle = idle;
        s_us = us;
    }
    return s_rtBusyMax[reader].exchange(0, std::memory_order_relaxed);
}

extern "C" uint32_t audio_rt_busy_avg(void)
{
    const uint32_t n = s_rtBusyPasses.exchange(0, std::memory_order_relaxed);
    const uint64_t sum = s_rtBusySum.exchange(0, std::memory_order_relaxed);
    return n ? (uint32_t)(sum / n) : 0;
}

extern "C" void audio_rt_note_live(void)
{
    g_liveAtUs.store(esp_timer_get_time(), std::memory_order_relaxed);
    if (g_offlineReq.load(std::memory_order_acquire)) g_offlineDrop.store(true, std::memory_order_release);
}

extern "C" bool audio_rt_pressed_recently(void)
{
    return esp_timer_get_time() - g_liveAtUs.load(std::memory_order_relaxed) < kLiveQuietUs;
}

extern "C" bool audio_rt_live_recently(void)
{
    return audio_rt_pressed_recently() || usb_audio_port_playback_streaming();
}

extern "C" bool audio_rt_offline(bool on, uint32_t timeout_ms)
{
    if (!g_rtTaskHandle) return false;
    g_offlineReq.store(on, std::memory_order_release);
    const int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (g_offlineOn.load(std::memory_order_acquire) != on) {
        if (esp_timer_get_time() > until) {
            if (on) g_offlineReq.store(false, std::memory_order_release);
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

extern "C" uint32_t audio_rt_block_us(void)
{
    return s_blockUs.load(std::memory_order_relaxed);
}

extern "C" uint32_t audio_rt_overruns(void)
{
    return g_rt_overruns.load(std::memory_order_relaxed);
}

bool AudioTasks_IsReady(void)
{
    return g_ready.load(std::memory_order_acquire);
}

void AudioTasks_Start(void)
{
    g_run.store(true);

    // Before either task exists, so the first render and the first refill
    // already have something to take. These two tasks and the kit loader all
    // reach into the same prebuilt player; see SamplePlayerAccess.hpp.
    crosspad::samplePlayerAccessInit();

    // Both creates used to go unchecked. With NimBLE up the RT task never
    // existed, no codec was ever initialised and USB audio failed with
    // "I2S0 not initialized" — with nothing in the log to say why.
    g_rtTaskHandle = xTaskCreateStaticPinnedToCore(AudioTask_RT, "audio_rt", RT_TASK_STACK_SIZE,
                                                   nullptr, RT_TASK_PRIORITY, s_rtStack, &s_rtTcb,
                                                   RT_TASK_CORE);
    if (!g_rtTaskHandle) {
        ESP_LOGE(TAG, "audio_rt task not created");
        return;
    }
    g_bgTaskHandle = xTaskCreateStaticPinnedToCore(AudioTask_BG, "audio_bg", BG_TASK_STACK_SIZE,
                                                   nullptr, BG_TASK_PRIORITY, s_bgStack, &s_bgTcb,
                                                   BG_TASK_CORE);
    if (!g_bgTaskHandle) {
        ESP_LOGE(TAG, "audio_bg task not created");
    }
}

/* For SMPLENG's bg_stack_free=: bytes of audio_bg's 4 kB stack never used
 * since boot. The native engine's refills and the audition's header parse
 * run on it (serviceIo()); a number near zero means it needs more. */
extern "C" uint32_t audio_bg_stack_free(void)
{
    TaskHandle_t h = g_bgTaskHandle;
    return h ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(h)) : 0u;
}

/* For the native engine's note-on stall record: how much of a slow note-on
 * the streamer was running. */
extern "C" TaskHandle_t audio_bg_task(void)
{
    return g_bgTaskHandle;
}

void AudioTasks_SetSuspended(bool suspended)
{
    g_suspended.store(suspended, std::memory_order_release);
}

/* Stop the render between passes and wait until it has (true), or let it go
 * (false). For what must change under it: its buffers, the mixer's, every
 * node's -- the block size (AudioBufferPort.cpp). False when it did not stop
 * within timeout_ms; the request is withdrawn then. */
extern "C" bool audio_rt_park(bool park, uint32_t timeout_ms)
{
    if (!park) {
        g_parkReq.store(false, std::memory_order_release);
        return true;
    }
    if (!g_rtTaskHandle) return false;
    const uint32_t seq = g_parkSeq.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_parkReq.store(true, std::memory_order_release);
    const int64_t until = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (g_parkedSeq.load(std::memory_order_acquire) != seq) {
        if (esp_timer_get_time() > until) {
            g_parkReq.store(false, std::memory_order_release);
            return false;
        }
        vTaskDelay(1);
    }
    return true;
}

void AudioTasks_Stop(void)
{
    g_run.store(false);

    /* Optional: wait for tasks to terminate */
    vTaskDelay(pdMS_TO_TICKS(WAIT_FOR_TASK_TERMINATION_MS));

    g_rtTaskHandle = nullptr;
    g_bgTaskHandle = nullptr;
}
