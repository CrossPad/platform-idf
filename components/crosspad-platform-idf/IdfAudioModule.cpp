// SPDX-License-Identifier: GPL-3.0-or-later
//
// Out-of-line: needs the concrete AudioMixerEngine type (header only fwd-decls).

#include "crosspad_s3.h"   // first: after the headers below, esp_lcd's I2C declarations conflict
#include "IdfAudioModule.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad/dsp/AudioKernels.hpp>
#include "UsbAudioPort.hpp"
#include "lat_trace.h"
#include "IdfI2sInput.hpp"
#include "InputGainPort.hpp"
#include "InputMonitorPort.hpp"
#include "SampleToolsPort.hpp"
#if CROSSPAD_HAS_SONG_ENGINE
#include "SongEnginePort.hpp"
#endif

extern "C" {
#include "i2s_driver.h"
}
#include "driver/i2s_std.h"
#include "freertos/semphr.h"

#include "esp_cpu.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>

#include "esp_log.h"
#include "esp_timer.h"

uint32_t g_rt_io_wait_us;

namespace {

/* ── How far ahead of the DAC the render runs ─────────────────────────────
 *
 * The render used to be paced by the blocking TX write alone: it waits until
 * the DMA frees a buffer, fills it, and starts the next block at once. The
 * DMA comes back to a buffer only after the other DESC_NUM - 1, so a block
 * started that way reaches the DAC DESC_NUM buffers after its pass began --
 * 8.7 ms with 6 x 64 frames, measured 8.46 ms from pass start (LAT_TRACE
 * pass_to_dac) and 86 % of everything a note spends on the ESP.
 *
 * The ring stays that size; what changes is when the render starts. Paced, a
 * pass waits for the end-of-buffer interrupt (bsp_i2s_tx_set_waiter) and
 * leaves k freed buffers unwritten, so the buffer it fills was freed k
 * buffers ago and plays that much sooner: q = DESC_NUM - n - k buffers from
 * pass start to DAC, n buffers to a block. Every buffer is still written
 * once and in ring order, so moving q -- up, down, at runtime -- never drops
 * or repeats a frame; it costs only the passes that catch up.
 *
 * k is set from a known point: a write that had to wait took the buffer the
 * DMA had just freed, so right after it nothing is free (k = 0). From there
 * the pacer lets k + n buffers free before the next pass. A late pass shows
 * up the same way -- its write waits -- and is simply such a point again.
 *
 * q >= 3: the driver drops a half-written buffer for a newer one once only
 * one slot of its queue is left (i2s_channel_write), so k + n must stay below
 * DESC_NUM - 2. q = DESC_NUM is the blocking write as before. */
constexpr uint32_t kRingBuffers = BSP_I2S_DMA_DESC_NUM;
constexpr uint32_t kBufFrames   = BSP_I2S_DMA_FRAME_NUM;
constexpr uint32_t kMinQueue    = 3;
constexpr uint32_t kWaitedUs    = 100;

std::atomic<uint8_t> s_queueTarget{kRingBuffers};
std::atomic<uint32_t> s_resyncs{0};      // paced passes whose write waited after all
/* Buffers codec 0 played unrefilled while the render was keeping up -- its
 * underruns. The DMA's own count also takes in a render that was parked (the
 * USB-audio profile, a codec re-init) and the buffers it plays while the
 * render catches up after one, so it counts only from a write that found the
 * queue full (`fed`) until a gap over kParkedUs. */
std::atomic<uint32_t> s_txUnderruns{0};
constexpr uint32_t kParkedUs = 50000;
uint32_t s_pacerWaitUs = 0;   // audio_rt only: this pass's wait for its buffer

StaticSemaphore_t s_eofSemBuf;
SemaphoreHandle_t s_eofSem = nullptr;

/* audio_rt only. */
struct Pacer {
    uint8_t  q = kRingBuffers;
    bool     synced = false;
    int32_t  credit = 0;          // frames the DMA has freed beyond those written
    uint32_t wakeUs = 0;          // the EOF this pass started on, when it waited for one
    uint32_t tx0BeginUs = 0, tx0EndUs = 0;
    uint32_t dacUs = 0;           // when this pass's first frame plays, 0 unknown
    uint32_t unfilled = 0;        // codec 0's count at the last pass
    bool     fed = false;         // the render has kept the queue up since it last waited
} s_pacer;

/// Buffers a block of `frames` takes, 0 when it is not a whole number of them.
inline uint32_t buffers_per_block(uint32_t frames)
{
    return (frames && frames % kBufFrames == 0) ? frames / kBufFrames : 0;
}

/// Whether `q` can be paced for this block; otherwise the blocking write paces.
inline bool pacing(uint32_t q, uint32_t frames)
{
    const uint32_t n = buffers_per_block(frames);
    return n && q < kRingBuffers && q >= kMinQueue && q + n <= kRingBuffers;
}

inline uint32_t buffers_to_us(uint32_t buffers)
{
    const uint32_t rate = bsp_i2s_get_sample_rate();
    return rate ? (uint32_t)((uint64_t)buffers * kBufFrames * 1000000u / rate) : 0;
}

/// Before the render: wait for the buffer this pass is due to fill.
void pace_before_render(uint32_t frames)
{
    if (!s_eofSem) {
        s_eofSem = xSemaphoreCreateCountingStatic(255, 0, &s_eofSemBuf);
        bsp_i2s_tx_set_waiter(BSP_I2S_INTERFACE_0, s_eofSem);
    }
    Pacer& p = s_pacer;
    const uint8_t q = s_queueTarget.load(std::memory_order_relaxed);
    if (q != p.q) { p.q = q; p.synced = false; }
    p.wakeUs = 0;
    s_pacerWaitUs = 0;
    if (!p.synced || !pacing(q, frames)) return;
    const int64_t t0 = esp_timer_get_time();
    bool waited = false;
    while (p.credit < (int32_t)frames) {
        if (xSemaphoreTake(s_eofSem, 0) != pdTRUE) {
            waited = true;
            /* No end-of-buffer for this long: the channel stopped (a profile
             * switch re-inits it). The blocking write takes over again. */
            if (xSemaphoreTake(s_eofSem, pdMS_TO_TICKS(10)) != pdTRUE) {
                p.synced = false;
                break;
            }
        }
        p.credit += kBufFrames;
    }
    if (p.synced) {
        p.credit -= (int32_t)frames;
        if (waited) {
            bsp_i2s_tx_clock_t c;
            bsp_i2s_tx_clock(BSP_I2S_INTERFACE_0, &c);
            p.wakeUs = c.last_us;
        }
    }
    s_pacerWaitUs = (uint32_t)(esp_timer_get_time() - t0);
    g_rt_io_wait_us += s_pacerWaitUs;
}

/// After codec 0 took the block [t0, t1]: re-anchor on a write that waited.
void pace_after_write(uint32_t frames, uint32_t t0, uint32_t t1)
{
    Pacer& p = s_pacer;
    bsp_i2s_tx_clock_t c;
    bsp_i2s_tx_clock(BSP_I2S_INTERFACE_0, &c);
    /* Woken by the interrupt that freed the buffer it filled. The interrupt
     * landing inside a write that did not wait is not that: a write that
     * finds a free buffer takes ~10 us, one that waited took longer. An
     * anchor taken by mistake would let the queue run k buffers deep twice. */
    const bool waited = (t1 - t0) > kWaitedUs &&
                        (int32_t)(c.last_us - t0) >= 0 && (int32_t)(t1 - c.last_us) >= 0;
    if (t0 - p.tx0BeginUs >= kParkedUs) p.fed = false;
    if (c.unfilled != p.unfilled) {
        if (p.fed) s_txUnderruns.fetch_add(c.unfilled - p.unfilled, std::memory_order_relaxed);
        p.unfilled = c.unfilled;
    }
    if (waited) p.fed = true;
    p.tx0BeginUs = t0;
    p.tx0EndUs = t1;
    const bool paced = pacing(p.q, frames);
    if (paced && p.synced && p.wakeUs) {
        p.dacUs = p.wakeUs + buffers_to_us(p.q);
    } else if (!paced && waited && buffers_per_block(frames)) {
        /* The wait ended on the block's last buffer; its first was freed
         * n - 1 buffers earlier and plays DESC_NUM - 1 after that. */
        p.dacUs = c.last_us + buffers_to_us(kRingBuffers - buffers_per_block(frames));
    } else {
        p.dacUs = 0;
    }
    if (!paced || !waited) return;
    if (p.synced) s_resyncs.fetch_add(1, std::memory_order_relaxed);
    while (xSemaphoreTake(s_eofSem, 0) == pdTRUE) { }
    const uint32_t k = kRingBuffers - buffers_per_block(frames) - p.q;
    p.credit = -(int32_t)(k * kBufFrames);
    p.synced = true;
    p.dacUs = 0;
}

} // namespace

namespace crosspad {

uint32_t IdfI2sStream::pushSamples(const void* samples, AudioFormat fmt, uint32_t frames) {
    if (fmt != kFormat) return 0;
    i2s_chan_handle_t tx = txHandle();
    if (!tx) return 0;
    constexpr size_t frameBytes = kChannels * sizeof(int16_t);
    size_t bytes_written = 0;
    const int64_t t0 = esp_timer_get_time();
    const esp_err_t err = i2s_channel_write(tx, samples, frames * frameBytes, &bytes_written,
                                            pdMS_TO_TICKS(100));
    const int64_t t1 = esp_timer_get_time();
    g_rt_io_wait_us += (uint32_t)(t1 - t0);
    if (iface_ == BSP_I2S_INTERFACE_0 && err == ESP_OK) pace_after_write(frames, (uint32_t)t0, (uint32_t)t1);
    /* A channel whose handle exists but is not enabled (a profile switch, a
     * codec re-init) answers at once; discarded, that let the RT loop
     * free-run. Counted, and the pass reports itself unpaced. */
    if (err != ESP_OK) {
        writeErrors_.fetch_add(1, std::memory_order_relaxed);
        lastError_.store(err, std::memory_order_relaxed);
        return 0;
    }
    return static_cast<uint32_t>(bytes_written / frameBytes);
}

namespace {

/* A block faded to silence before the render stops for a reconfiguration
 * and from it when the render comes back: the DMA plays zeros in between,
 * and a cut at full level either side of them is a click. */
std::atomic<int8_t> s_fade{0};

/* The block the render last made, for code outside it that sizes by the
 * block (the USB rings, the RT loop's budget). */
std::atomic<uint32_t> s_blockFrames{0};

void fade_block(float* bus, uint32_t frames, int8_t dir)
{
    const float step = 1.0f / (float)frames;
    for (uint32_t i = 0; i < frames; ++i) {
        const float g = dir < 0 ? 1.0f - (float)(i + 1) * step : (float)i * step;
        bus[i * 2] *= g;
        bus[i * 2 + 1] *= g;
    }
}

/* The render thread must never wait. Its budget is one I2S period -- 1.45 ms
 * at 44.1 kHz with a 64-frame block -- and MetronomeNode counts the
 * sequencer's beat in frames *rendered*, so a render that overruns does not
 * merely thin the sound, it takes the pulse with it.
 *
 * These reads used to pass pdMS_TO_TICKS(4): a blocking read with a timeout
 * nearly three times the whole budget, on the one thread that cannot afford
 * to block, for up to two codecs. In steady state RX is clocked by the same
 * BCLK the TX write just paced and returns at once -- which is why it was
 * written that way -- but "in steady state" is not a guarantee, and the
 * profile where these run, USB audio, is the profile where the pulse came
 * apart while CDC was clean.
 *
 * Non-blocking: a block RX has not got ready is skipped. That costs the
 * *capture* one block -- 1.45 ms of gap in what reaches the host or the
 * Recorder -- and costs the render nothing. Skips are counted rather than
 * swallowed (SMPL_PEAK's rxskip=), because a path that quietly captures
 * nothing is worse than one that waited. */
std::atomic<uint32_t> s_rxSkips{0};

/* Whether the last render pass handed its block to anything that will make
 * the next one wait. The RT loop has no clock of its own -- `taskYIELD()` at
 * priority 19 on core 1 yields to nothing above it, so the blocking I2S write
 * *is* the pacer. When every output refuses the block the loop has no brake
 * at all, and free-ran at 100 % of core 1 until the task watchdog said so. */
std::atomic<bool>     s_paced{true};
std::atomic<uint32_t> s_unpaced{0};

/* ── Where the render pass actually goes ──────────────────────────────────
 *
 * `TASKS` said audio_rt costs 42 % of core 1 at the launcher with no kit
 * loaded -- about 605 us of work for a 64-frame block whose whole budget is
 * 1450 us. That is the number behind "everything is worse in the USB-audio
 * profile": core 1 has 46 % left for LVGL, TinyUSB and the event bus before
 * the UAC tasks are even started.
 *
 * `TASKS` cannot say *which part*, so this does: four spans of one pass,
 * summed, read and cleared by RT_PROF. The clock is the cycle counter -- one
 * RSR instruction, per-core, and this all runs on one core -- so leaving it
 * on costs the render four register reads a pass. Off by default anyway. */
std::atomic<bool>     s_prof{false};
std::atomic<uint32_t> s_profPasses{0};
/* Sums of each span in units of 16 cycles (66.7 ns), 32-bit: a 64-bit atomic
 * is not lock-free on this core, and these are added to by the render every
 * pass. A whole 1.45 ms pass is ~21 800 units, so even a span that took every
 * pass wraps only after ~3 minutes unread; RT_PROF reads and clears. */
constexpr uint32_t kProfShift = 4;
std::atomic<uint32_t> s_profBegin{0}, s_profMix{0}, s_profTap{0};
std::atomic<uint32_t> s_profPush{0}, s_profCap{0}, s_profMeter{0};
std::atomic<uint32_t> s_profWorst{0};

/// Cycles to profile units, rounded to nearest so the means are not biased low.
inline uint32_t prof_units(uint32_t cycles) { return (cycles + (1u << (kProfShift - 1))) >> kProfShift; }

inline uint32_t cyc() { return esp_cpu_get_cycle_count(); }

bool read_codec_block(int c, int16_t* buf, uint32_t frames)
{
    i2s_chan_handle_t h = bsp_i2s_get_rx_handle(c == 0 ? BSP_I2S_INTERFACE_0
                                                       : BSP_I2S_INTERFACE_1);
    if (!h) return false;
    size_t got = 0;
    const size_t want = static_cast<size_t>(frames) * 2 * sizeof(int16_t);
    if (i2s_channel_read(h, buf, want, &got, 0) != ESP_OK || got != want) {
        s_rxSkips.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    input_gain_apply(c, buf, frames);
    return true;
}

} // namespace

/* When the last pass's block reaches the DAC (pace_after_write()), 0 when a
 * pass cannot know: a blocking write knows the EOF that woke it, a paced pass
 * the one it started on, a pass that caught up neither. */
extern "C" void audio_rt_last_write(uint32_t* write_done_us, uint32_t* dac_us)
{
    *write_done_us = s_pacer.tx0EndUs;
    *dac_us = s_pacer.dacUs;
}

/// Frames from a pass's start to its first frame at the DAC, rounded down to
/// whole DMA buffers; the ring's size or more is the blocking write.
extern "C" void audio_rt_set_queue_frames(uint32_t frames)
{
    const uint32_t q = frames / kBufFrames;
    s_queueTarget.store((uint8_t)(q < kRingBuffers ? q : kRingBuffers), std::memory_order_relaxed);
}

extern "C" uint32_t audio_rt_queue_frames(void)
{
    return (uint32_t)s_queueTarget.load(std::memory_order_relaxed) * kBufFrames;
}

/// Whether a block of `block` frames can run `queue` frames ahead of the DAC
/// here: the ring's whole size always, less only as whole DMA buffers the
/// pacer can hold (pacing()).
extern "C" bool audio_rt_queue_supported(uint32_t block, uint32_t queue)
{
    if (queue == kRingBuffers * kBufFrames) return block <= kRingBuffers * kBufFrames;
    return queue % kBufFrames == 0 && pacing(queue / kBufFrames, block);
}

/// Paced passes whose write had to wait after all -- each a late pass or a
/// channel that restarted, and the pacer re-anchored there.
extern "C" uint32_t audio_rt_pacer_resyncs(void) { return s_resyncs.load(std::memory_order_relaxed); }

/// The last pass's wait for its buffer: time the render chose not to run.
extern "C" uint32_t audio_rt_pacer_wait_us(void) { return s_pacerWaitUs; }

/// Buffers codec 0 played unrefilled while the render was running.
extern "C" uint32_t audio_rt_tx_underruns(void) { return s_txUnderruns.load(std::memory_order_relaxed); }

/// The render is back from a park (audio_rt_park()). RT thread: the DMA
/// played silence meanwhile and the block may be another size, so the pacer
/// starts over from the next write that waits, and what the DMA played
/// unrefilled before it is not an underrun.
extern "C" void audio_rt_pacer_restart(void)
{
    s_pacer.synced = false;
    s_pacer.fed = false;
    s_pacer.credit = 0;
}

/// Frames of the block the render makes now; 0 before its first pass.
extern "C" uint32_t audio_rt_block_frames(void) { return s_blockFrames.load(std::memory_order_relaxed); }

/// The next pass fades its output out (dir < 0) or in (dir > 0).
extern "C" void audio_rt_fade_next(int dir) { s_fade.store((int8_t)(dir < 0 ? -1 : 1), std::memory_order_release); }

/// Capture blocks the render skipped because RX was not ready (SMPL_PEAK).
extern "C" uint32_t audio_rx_skips(void) { return s_rxSkips.load(std::memory_order_relaxed); }

/// False when the last pass gave its block to no output -- the RT loop must
/// then sleep instead of yielding, or nothing will slow it down.
extern "C" bool audio_rt_pass_paced(void) { return s_paced.load(std::memory_order_relaxed); }
/// How many passes went out to nothing. Zero on a healthy board.
extern "C" uint32_t audio_rt_unpaced_passes(void) { return s_unpaced.load(std::memory_order_relaxed); }

/* The mixer's own profile, which only the platform can give it a clock for.
 * Set on the first enable rather than at construction: the module does not
 * own the mixer and is handed it later. */
extern "C" void audio_mixer_prof_enable(bool on, void* mixer)
{
    auto* m = static_cast<AudioMixerEngine*>(mixer);
    if (!m) return;
    m->setProfClock(&cyc);
    m->profEnable(on);
}

extern "C" void audio_mixer_prof_read(void* mixer, char* out, uint32_t cap)
{
    auto* m = static_cast<AudioMixerEngine*>(mixer);
    if (!m) { if (out && cap) std::snprintf(out, cap, "MIXPROF: ERR no_mixer\r\n"); return; }
    m->profRead(out, cap, 240.0f);
}

extern "C" void audio_rt_prof_enable(bool on)
{
    s_profPasses.store(0, std::memory_order_relaxed);
    s_profBegin.store(0, std::memory_order_relaxed);
    s_profMix.store(0, std::memory_order_relaxed);
    s_profTap.store(0, std::memory_order_relaxed);
    s_profPush.store(0, std::memory_order_relaxed);
    s_profCap.store(0, std::memory_order_relaxed);
    s_profMeter.store(0, std::memory_order_relaxed);
    s_profWorst.store(0, std::memory_order_relaxed);
    s_prof.store(on, std::memory_order_relaxed);
}

/// Mean microseconds a pass spent in each span, and the worst whole pass.
/// Reads and clears. Zero passes means the profile was never enabled.
extern "C" void audio_rt_prof_read(char* out, uint32_t cap)
{
    if (!out || cap == 0) return;
    const uint32_t n = s_profPasses.exchange(0, std::memory_order_relaxed);
    const uint64_t begin = s_profBegin.exchange(0, std::memory_order_relaxed);
    const uint64_t mix   = s_profMix.exchange(0, std::memory_order_relaxed);
    const uint64_t tap   = s_profTap.exchange(0, std::memory_order_relaxed);
    const uint64_t push  = s_profPush.exchange(0, std::memory_order_relaxed);
    const uint64_t capt  = s_profCap.exchange(0, std::memory_order_relaxed);
    const uint64_t meter = s_profMeter.exchange(0, std::memory_order_relaxed);
    const uint32_t worst = s_profWorst.exchange(0, std::memory_order_relaxed);
    /* The cycle counter runs at the CPU clock; 240 MHz on this part. The span
     * sums are in units of 1 << kProfShift cycles; the worst pass in cycles.
     * Tenths of a microsecond in integers: this runs on the 4 kB main task
     * under hil_control_handle_cdc's frame, and a double through snprintf
     * (dtoa) with an interrupt on top overflowed it (stack canary, 16.10). */
    const uint64_t d = (n ? n : 1) * 240ull;
    auto tenths = [d](uint64_t units) { return (unsigned)(((units << kProfShift) * 10 + d / 2) / d); };
    const unsigned t[7] = {tenths(begin), tenths(mix), tenths(tap), tenths(push), tenths(capt), tenths(meter),
                           tenths(begin + mix + tap + push + capt + meter)};
    const unsigned w = (unsigned)(((uint64_t)worst * 10 + 120) / 240);
    std::snprintf(out, cap,
                  "RTPROF: on=%d passes=%u begin_us=%u.%u mix_us=%u.%u tap_us=%u.%u "
                  "push_us=%u.%u cap_us=%u.%u meter_us=%u.%u total_us=%u.%u worst_us=%u.%u\r\n",
                  s_prof.load(std::memory_order_relaxed) ? 1 : 0, (unsigned)n,
                  t[0] / 10, t[0] % 10, t[1] / 10, t[1] % 10, t[2] / 10, t[2] % 10,
                  t[3] / 10, t[3] % 10, t[4] / 10, t[4] % 10, t[5] / 10, t[5] % 10,
                  t[6] / 10, t[6] % 10, w / 10, w % 10);
}

void IdfAudioModule::readCodecsForTaps(uint32_t frames) {
    for (int c = 0; c < 2; ++c) {
        if (!sample_tools_wants_codec(c) && !input_monitor_wants_codec(c)) continue;
        if (!read_codec_block(c, captureScratch_[c].data(), frames)) continue;
        sample_tools_rt_input(c, captureScratch_[c].data(), frames);
        IdfI2sInput::offer(c, captureScratch_[c].data(), frames);
    }
}

bool IdfAudioModule::processOffline() {
    if (!mixer_) return false;
    const uint32_t frames = config_.frameCount;
    uint8_t chs[4];
    uint8_t n = 0;
    if (clockChannel_ != 0xFF) chs[n++] = clockChannel_;
    for (uint8_t c : padChannels_) if (c != 0xFF) chs[n++] = c;
    mixer_->renderOnly(chs, n, frames);
    mixer_->renderTap(padChannels_, 3, padsTap_.data(), frames);
#if CROSSPAD_HAS_SONG_ENGINE
    crosspad::song_engine_rt_capture(padsTap_.data(), frames);
#endif
    return true;
}

void IdfAudioModule::setupBoardFix(uint32_t rate) {
    codec1Fix_.setRate(rate);
    if (!BSP_CODEC1_OUT_POLE_HZ) return;
    /* The RBJ shelf closest to the inverse of a 30.3 kHz pole at 44.1 kHz:
     * within 0.01 dB of it from 500 Hz to 20 kHz (+0.45 dB at 10 kHz, +0.95
     * at 15, +1.3 at 18). A pole elsewhere wants its own fit. */
    static_assert(BSP_CODEC1_OUT_POLE_HZ == 0 || BSP_CODEC1_OUT_POLE_HZ == 30300, "refit the shelf for this pole");
    EqSettings fix;
    fix.on = true;
    fix.band[0] = EqBand{EqKind::HighShelf, 13926.0f, 1.65f, 0.46f};
    codec1Fix_.set(fix);
    codec1FixDb_ = fix.band[0].gainDb;
}

float IdfAudioModule::boardFixHeadroom(uint8_t out) const {
    return (out == 1 && codec1FixDb_ > 0.0f) ? powf(10.0f, -codec1FixDb_ / 20.0f) : 1.0f;
}

void IdfAudioModule::processMixer() {
    const uint32_t frames  = config_.frameCount;
    const uint32_t samples = frames * 2;

    s_blockFrames.store(frames, std::memory_order_relaxed);
    pace_before_render(frames);
    lat_trace_pass_begin((uint32_t)esp_timer_get_time());

    const bool prof = s_prof.load(std::memory_order_relaxed);
    const uint32_t cPass = prof ? cyc() : 0;

    sample_tools_rt_block_begin();
    const uint32_t cBegin = prof ? cyc() : 0;
    float* outBuses[NUM_OUTPUTS];
    for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) outBuses[s] = mixerBus_[s].data();
    mixer_->render(outBuses, NUM_OUTPUTS, frames);
    bool appMixed = false;
    for (auto& slot : appNodes_) {
        IAudioNode* n = slot.load(std::memory_order_acquire);
        if (n && n->enabled() && n->isGenerator()) {
            n->mix(mixerBus_[0].data(), frames);
            appMixed = true;
        }
    }
    const uint32_t cMixOnly = prof ? cyc() : 0;
    /* The pads alone, as they are mixed: a bounce and a PADS take record this,
     * not the USB rec bus, which the Mixer can send the mics and the song to.
     * Only while one does: made every block it was ~58 us of a 1451 us block
     * with nothing recording (the seq tracks' meters come from the engines). */
#if CROSSPAD_HAS_SONG_ENGINE
    const bool songTap = crosspad::song_engine_capturing();
#else
    const bool songTap = false;
#endif
    if (songTap || sample_tools_wants_bus()) {
        mixer_->renderTap(padChannels_, 3, padsTap_.data(), frames);
#if CROSSPAD_HAS_SONG_ENGINE
        crosspad::song_engine_rt_capture(padsTap_.data(), frames);
#endif
        sample_tools_rt_bus(padsTap_.data(), frames);
    }
    const uint32_t cMix = prof ? cyc() : 0;

    /* isOpen() says the handle exists, not that the channel is enabled, and a
     * disabled channel refuses the write without waiting -- so what matters
     * here is how many frames were actually taken. None means this pass went
     * nowhere and the loop has nothing pacing it. */
    uint32_t taken = 0;
    const int8_t fade = s_fade.exchange(0, std::memory_order_acq_rel);
    for (uint8_t s = 0; s < NUM_OUTPUTS; ++s) {
        IAudioStream* stream = getOutputStream(s);
        if (!stream || !stream->isOpen()) continue;
        if (s == 1 && codec1Fix_.on()) codec1Fix_.process(mixerBus_[1].data(), frames);
        if (fade) fade_block(mixerBus_[s].data(), frames, fade);
        dsp::floatToInt16(mixerBus_[s].data(),
                          mixerScratchInt16_[s].data(),
                          samples);
        taken += stream->pushSamples(mixerScratchInt16_[s].data(),
                                     AudioFormat::Int16, frames);
    }
    s_paced.store(taken != 0, std::memory_order_relaxed);
    if (taken == 0) s_unpaced.fetch_add(1, std::memory_order_relaxed);
    const uint32_t cPush = prof ? cyc() : 0;

    // USB capture: this thread is the only ADC reader while the USB audio
    // profile is up (the line-in nodes are suppressed then), so the codecs the
    // capture source needs are read here, one block per render, and handed to
    // the board→host ring. RX fills at the same BCLK the TX write above just
    // paced, so in steady state these reads return at once.
    if (usb_audio_port_active() &&
        usb_audio_port_get_capture_source() == USB_AUDIO_PORT_CAPTURE_SAMPLER) {
        /* Pads-only bus, already rendered above and going to no codec. Digital,
         * so a DAW monitoring its own metronome cannot leak back in. */
        dsp::floatToInt16(mixerBus_[CAPTURE_BUS].data(),
                          mixerScratchInt16_[CAPTURE_BUS].data(), samples);
        usbAudioPortCaptureBus(mixerScratchInt16_[CAPTURE_BUS].data(), frames);
        readCodecsForTaps(frames);
    } else if (usb_audio_port_active()) {
        const int16_t* rx[2] = {nullptr, nullptr};
        for (int c = 0; c < 2; ++c) {
            if (!usbAudioPortWantsCodec(c) && !sample_tools_wants_codec(c) && !input_monitor_wants_codec(c)) continue;
            if (read_codec_block(c, captureScratch_[c].data(), frames)) rx[c] = captureScratch_[c].data();
        }
        /* One read a codec a block, shared: the recorder takes the same
         * block the host gets, and a codec only the recorder wants is not
         * handed to the host. */
        for (int c = 0; c < 2; ++c) {
            if (rx[c] && sample_tools_wants_codec(c)) sample_tools_rt_input(c, rx[c], frames);
            if (rx[c]) IdfI2sInput::offer(c, rx[c], frames);    // the IN channel, next render
        }
        usbAudioPortCaptureTap(usbAudioPortWantsCodec(0) ? rx[0] : nullptr,
                               usbAudioPortWantsCodec(1) ? rx[1] : nullptr, frames);
    }

    const uint32_t cCap = prof ? cyc() : 0;

    // Peak meter on bus0 — the VU surface in the GUI watches output 0. The
    // mixer metered that bus as the last thing it did to it: the same
    // numbers, without a pass over it.
    float pl = 0.0f, pr = 0.0f;
    mixer_->getOutputLevel(AudioMixerEngine::OutputId{0}, pl, pr);
    /* The mixer meters its own render; an app's node joined the bus after
     * it. Without this the meter -- and the speaker amp gate it drives --
     * never saw the app's sound, and the speakers stayed off. */
    if (appMixed) {
        const float* bus = mixerBus_[0].data();
        for (uint32_t i = 0; i < frames; ++i) {
            pl = std::max(pl, std::fabs(bus[i * 2]));
            pr = std::max(pr, std::fabs(bus[i * 2 + 1]));
        }
    }
    peakL_.store(pl, std::memory_order_relaxed);
    peakR_.store(pr, std::memory_order_relaxed);

    if (prof) {
        const uint32_t cEnd = cyc();
        s_profBegin.fetch_add(prof_units(cBegin - cPass), std::memory_order_relaxed);
        s_profMix.fetch_add(prof_units(cMixOnly - cBegin), std::memory_order_relaxed);
        s_profTap.fetch_add(prof_units(cMix - cMixOnly), std::memory_order_relaxed);
        s_profPush.fetch_add(prof_units(cPush - cMix), std::memory_order_relaxed);
        s_profCap.fetch_add(prof_units(cCap - cPush), std::memory_order_relaxed);
        s_profMeter.fetch_add(prof_units(cEnd - cCap), std::memory_order_relaxed);
        const uint32_t whole = cEnd - cPass;
        if (whole > s_profWorst.load(std::memory_order_relaxed))
            s_profWorst.store(whole, std::memory_order_relaxed);
        s_profPasses.fetch_add(1, std::memory_order_relaxed);
    }
}

void IdfAudioModule::addNode(IAudioNode* node) {
    if (!mixer_) {
        AbstractAudioModule::addNode(node);
        return;
    }
    if (!node) return;
    if (config_.sampleRate > 0) node->onPrepare(config_.frameCount, config_.sampleRate);
    for (auto& slot : appNodes_) {
        IAudioNode* empty = nullptr;
        if (slot.compare_exchange_strong(empty, node, std::memory_order_acq_rel)) return;
    }
    ESP_LOGW("IdfAudioModule", "no room for node %s", node->name());
}

void IdfAudioModule::removeNode(IAudioNode* node) {
    for (auto& slot : appNodes_) {
        IAudioNode* expected = node;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
    AbstractAudioModule::removeNode(node);
}

} // namespace crosspad
