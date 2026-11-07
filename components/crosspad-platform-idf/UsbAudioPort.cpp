// SPDX-License-Identifier: GPL-3.0-or-later
#include "UsbAudioPort.hpp"
#include "SampleToolsPort.hpp"
#include "crosspad/audio/AudioRingBuffer.hpp"
#include <cstdint>
#include <cstring>
#include "esp_cpu.h"
#include <vector>

namespace {

/* Ring depth in milliseconds. The engine renders 64-frame blocks (1.45 ms at
 * 44.1 kHz) and the host moves 1 ms packets that the UAC task drains every
 * 4 ms; 64 ms absorbs scheduling jitter on both sides with room for the
 * feedback loop to settle. Playback starts once a quarter of that is in, so
 * a stream that opens mid-block does not underrun on its first render. */
constexpr uint32_t kRingMs    = 64;
constexpr uint32_t kPrefillMs = 16;
/* After an underrun, what the host sent during the silence is dropped as the
 * speaker task brings it in -- within a few blocks of the restart. Past this
 * many frames (32 blocks of 64, 46 ms) whatever is left of it is forgotten,
 * so a level that merely swings later is never cut. */
constexpr uint32_t kDebtFrames = 2048;
/* Capture: what the mic task leaves in the ring after taking its interval.
 * The render adds a block at a time, so the level before a read swings by a
 * block, and a render pass late by up to a block (rt_gap_max 1.6-2.2 ms on
 * the bench at 64 frames) must not leave a read short: three blocks. */
constexpr size_t kCapCushionBlocks = 3;
/* Playback drift. The feedback endpoint reports this board's rate, but what a
 * host sends against it is its own business: Linux, fed 44104.5 Hz, sent
 * 44101 and the ring lost ~7 frames/s -- an underrun and 16 ms of silence
 * every couple of minutes, every 4-7 s while the feedback itself was noisy.
 * The render therefore takes the host's frames at 1 +- the limit of their
 * rate, steered by the ring's level (smoothed over kAsrcTauS): the level
 * holds instead of walking, whatever the host does, and the feedback stays a
 * pure clock measure (see usb_device_uac.c on why it must). macOS is further
 * off: fed 44100 +- 50 Hz it sent 44020-44095 (-100 to -1800 ppm, wandering
 * over tens of seconds) while the capture stream it could clock by ran at
 * 44098-44103 with nothing lost (UAC_FLOW, 2026-10-23); at +-1000 ppm the
 * ratio sat on its limit and the ring ran dry, 9 underruns in two minutes --
 * the glitches a USB take in the Recorder records. Most of that was packets
 * the board itself lost (the USB interrupt on core 0, usb_device_uac.c), but
 * the macOS host profile keeps +-5000 ppm (8.6 cents at the very worst) and a
 * gain that reaches 1800 ppm 300 frames under the target; Linux keeps the
 * +-1000 ppm and 2 ppm a frame it was measured with. Set per host by
 * usb_audio_port_set_asrc(). The loop's own time constant is
 * 1 / (ppm_per_frame * 1e-6 * rate) -- ~11 s at 2, ~3.8 s at 6, at 44.1 kHz,
 * slower than the smoothing either way, so it settles without ringing; a host
 * off by N ppm leaves the ring N / ppm_per_frame frames under its target. */
std::atomic<float> s_asrcMaxPpm{1000.0f};
std::atomic<float> s_asrcPpmPerFrame{2.0f};
constexpr float kAsrcTauS = 1.0f;
/* Input frames one render may take: the largest block plus the ratio's slack. */
constexpr size_t kAsrcScratchFrames = 520;

struct Port {
    crosspad::AudioRingBuffer<int16_t> playback;   // host → board, interleaved stereo
    crosspad::AudioRingBuffer<int16_t> capture;    // board → host
    std::atomic<bool>     active{false};
    std::atomic<bool>     priming{true};           // playback waits for prefill
    std::atomic<int>      source{0};
    std::atomic<uint32_t> sampleRate{44100};
    std::atomic<uint32_t> engineFrames{64};
    std::atomic<uint32_t> playUnderrun{0};
    std::atomic<uint32_t> playRefused{0};
    std::atomic<uint32_t> capDropped{0};
    std::atomic<uint32_t> playMin{UINT32_MAX};     // fewest frames a render found, since last asked
    std::atomic<uint32_t> capUnder{0};       // mic reads the ring could not fill
    std::atomic<bool>     capOpen{false};    // the host has the mic stream open: the render writes
    std::atomic<bool>     capReopen{false};  // opened since the mic task last read
    bool     capPrimed = false;              // mic task only
    uint32_t capDropSeen = 0;                // mic task only: capDropped at its last read
    bool   played = false;     // render only: this stream has been primed once
    size_t playDebt = 0;       // render only: silence played since, samples
    uint32_t debtFrames = 0;   // render only: frames left to drop it in
    size_t prefillSamples = 0;
    size_t playbackCapacity = 0;   // usable samples (ring holds capacity-1)
    /* Render only: the resampler. hist holds the last four input frames, the
     * output sits between hist[1] and hist[2] at fracQ (32.32 of a frame). */
    std::vector<int16_t> asrcIn;   // kAsrcScratchFrames stereo frames
    float    hist[4][2] = {};
    uint64_t fracQ = 0;
    uint64_t incQ = 1ull << 32;
    float    levelEma = 0.0f;
    std::atomic<int32_t> asrcPpm{0};   // the ratio last applied, for UAC_STATS
    /* Frames the host delivered and frames the render took from the ring,
     * since boot: two reads of them a known time apart give both rates. */
    std::atomic<uint32_t> hostFrames{0};
    std::atomic<uint32_t> takenFrames{0};
    /* Runs of 32+ digital-silence frames inside the stream, as the host sent
     * them (speaker task) and as the render played them: silence the host
     * itself sends is not a fault of this board. */
    std::atomic<uint32_t> hostZeroRuns{0};
    std::atomic<uint32_t> playZeroRuns{0};
    uint32_t hostZeroLen = 0;   // speaker task only
    uint32_t playZeroLen = 0;   // render only
    std::atomic<uint32_t> asrcCycMax{0};
    uint32_t asrcReadSum = 0, asrcLoopSum = 0, asrcN = 0, asrcReadAvg = 0, asrcLoopAvg = 0;
};

Port& port() {
    static Port p;
    return p;
}

/* Drop up to n samples off the read side, through scratch (chunk samples at
 * a time). Out of line: the ring's read is ~150 B wherever it is inlined. */
__attribute__((noinline)) void ringSkip(crosspad::AudioRingBuffer<int16_t>& r, int16_t* scratch,
                                        size_t n, size_t chunk)
{
    while (n) {
        const size_t got = r.read(scratch, n < chunk ? n : chunk);
        if (!got) break;
        n -= got;
    }
}

/* Render only: back to a plain copy, for a stream starting over. */
void asrcReset(Port& p)
{
    std::memset(p.hist, 0, sizeof p.hist);
    p.fracQ = 0;
    p.incQ = 1ull << 32;
    p.levelEma = -1.0f;
    p.asrcPpm.store(0, std::memory_order_relaxed);
}

inline int16_t sat16(float v)
{
    const int32_t i = static_cast<int32_t>(v);
    return static_cast<int16_t>(i > 32767 ? 32767 : (i < -32768 ? -32768 : i));
}

} // namespace

// ─────────────────────────────────────────────────────────────── C side (UAC tasks)

extern "C" void usb_audio_port_start(uint32_t sample_rate, uint32_t engine_frames)
{
    Port& p = port();
    p.active.store(false, std::memory_order_release);
    const size_t samplesPerMs = (sample_rate / 1000) * 2;
    p.playback.resize(samplesPerMs * kRingMs + 1);
    p.asrcIn.resize(kAsrcScratchFrames * 2);
    p.capture.resize(samplesPerMs * kRingMs + 1);
    p.prefillSamples = samplesPerMs * kPrefillMs;
    p.playbackCapacity = samplesPerMs * kRingMs;
    p.sampleRate.store(sample_rate, std::memory_order_relaxed);
    p.engineFrames.store(engine_frames, std::memory_order_relaxed);
    p.priming.store(true, std::memory_order_relaxed);
    p.played = false; p.playDebt = 0; p.debtFrames = 0;
    p.capOpen.store(false, std::memory_order_relaxed);
    p.capReopen.store(false, std::memory_order_relaxed);
    p.capPrimed = false; p.capDropSeen = 0;
    p.playUnderrun.store(0); p.playRefused.store(0); p.capDropped.store(0); p.capUnder.store(0);
    p.active.store(true, std::memory_order_release);
}

/* The block the render makes, for the capture cushion; it changes live. */
extern "C" void usb_audio_port_set_engine_frames(uint32_t engine_frames)
{
    port().engineFrames.store(engine_frames, std::memory_order_relaxed);
}

/* The render keeps running through a profile switch, so a pass may be inside
 * a ring right now: the rings are left as they are -- nothing touches them
 * while the port is inactive, and start() empties them (resize) a re-install
 * later, long after any pass that saw the port active has finished. */
extern "C" void usb_audio_port_stop(void)
{
    Port& p = port();
    p.active.store(false, std::memory_order_release);
    p.capOpen.store(false, std::memory_order_release);
    p.priming.store(true, std::memory_order_relaxed);
}

/* The host set the mic's alternate setting (TinyUSB task). Closed, the render
 * stops writing: nobody reads, and nothing it would write is a drop. Opened,
 * it writes again and the mic task starts the ring over on its next read. */
extern "C" void usb_audio_port_capture_open(bool open)
{
    Port& p = port();
    if (open) p.capReopen.store(true, std::memory_order_release);
    p.capOpen.store(open, std::memory_order_release);
}

extern "C" bool usb_audio_port_active(void)
{
    return port().active.load(std::memory_order_acquire);
}

/* A host is playing into the board: the stream is up and past its prefill. */
extern "C" bool usb_audio_port_playback_streaming(void)
{
    const Port& p = port();
    return p.active.load(std::memory_order_acquire) && !p.priming.load(std::memory_order_relaxed);
}

/// Adds to @p runs every run of zero frames reaching kZeroRun in @p stereo.
static void countZeroRuns(const int16_t* stereo, size_t frames, uint32_t& len, std::atomic<uint32_t>& runs)
{
    constexpr uint32_t kZeroRun = 32;
    for (size_t i = 0; i < frames; ++i) {
        if (stereo[2 * i] == 0 && stereo[2 * i + 1] == 0) {
            if (++len == kZeroRun) runs.fetch_add(1, std::memory_order_relaxed);
        } else {
            len = 0;
        }
    }
}

extern "C" size_t usb_audio_port_playback_write(const int16_t *stereo, size_t frames)
{
    Port& p = port();
    if (!p.active.load(std::memory_order_acquire)) return frames;   // sink it: no stream
    const size_t got = p.playback.write(stereo, frames * 2) / 2;
    p.hostFrames.fetch_add((uint32_t)frames, std::memory_order_relaxed);
    countZeroRuns(stereo, frames, p.hostZeroLen, p.hostZeroRuns);
    if (got < frames) p.playRefused.fetch_add((uint32_t)(frames - got), std::memory_order_relaxed);
    return got;
}

extern "C" size_t usb_audio_port_playback_space(void)
{
    return port().playback.space() / 2;
}

extern "C" size_t usb_audio_port_playback_capacity(void)
{
    return port().playbackCapacity / 2;
}

/* Mic task, once an interval, and the only side that moves the ring's read
 * index (the render never resets it under the reader). A stream starts --
 * and starts over after a short read or an overflow -- with the ring holding
 * this interval and kCapCushionFrames: older audio is dropped here rather
 * than handed to the host as a fixed latency (it was whatever the ring held
 * when the host opened the stream, 1.5-58 ms, a different one each session),
 * and until that much is in, the caller zero-fills. */
extern "C" size_t usb_audio_port_capture_read(int16_t *stereo, size_t frames)
{
    Port& p = port();
    if (!p.active.load(std::memory_order_acquire)) return 0;
    const size_t want = frames * 2;
    if (p.capReopen.exchange(false, std::memory_order_acq_rel)) {
        ringSkip(p.capture, stereo, SIZE_MAX, want);    // left from before the close
        p.capPrimed = false;
    }
    /* Closed under a read already under way: the host takes nothing more,
     * and the ring has stopped filling -- not an underrun. */
    if (!p.capOpen.load(std::memory_order_acquire)) return 0;
    const uint32_t drops = p.capDropped.load(std::memory_order_relaxed);
    if (drops != p.capDropSeen) {                       // overflowed: a full ring is 64 ms late
        p.capDropSeen = drops;
        p.capPrimed = false;
    }
    if (!p.capPrimed) {
        const size_t keep = want + kCapCushionBlocks * p.engineFrames.load(std::memory_order_relaxed) * 2;
        const size_t avail = p.capture.available();
        if (avail < keep) return 0;
        ringSkip(p.capture, stereo, avail - keep, want);
        p.capPrimed = true;
    }
    const size_t got = p.capture.read(stereo, want) / 2;
    if (got < frames) {
        p.capUnder.fetch_add(1, std::memory_order_relaxed);
        p.capPrimed = false;
    }
    return got;
}

extern "C" uint32_t usb_audio_port_capture_underruns(void)
{
    return port().capUnder.load(std::memory_order_relaxed);
}

extern "C" void usb_audio_port_set_capture_source(int source)
{
    port().source.store(source, std::memory_order_relaxed);
}

extern "C" int usb_audio_port_get_capture_source(void)
{
    return port().source.load(std::memory_order_relaxed);
}

extern "C" void usb_audio_port_stats(uint32_t *play_underrun, uint32_t *play_refused,
                                     uint32_t *cap_dropped, size_t *play_level, size_t *cap_level)
{
    Port& p = port();
    if (play_underrun) *play_underrun = p.playUnderrun.load(std::memory_order_relaxed);
    if (play_refused)  *play_refused  = p.playRefused.load(std::memory_order_relaxed);
    if (cap_dropped)   *cap_dropped   = p.capDropped.load(std::memory_order_relaxed);
    /* Outside the USB audio profile the rings were never sized, and an empty
     * ring's available() divides by its zero capacity. */
    const bool active = p.active.load(std::memory_order_acquire);
    if (play_level)    *play_level    = active ? p.playback.available() / 2 : 0;
    if (cap_level)     *cap_level     = active ? p.capture.available() / 2 : 0;
}

extern "C" void usb_audio_port_flow(uint32_t *host, uint32_t *taken)
{
    Port& p = port();
    if (host)  *host  = p.hostFrames.load(std::memory_order_relaxed);
    if (taken) *taken = p.takenFrames.load(std::memory_order_relaxed);
}

extern "C" void usb_audio_port_zero_runs(uint32_t *host, uint32_t *played)
{
    Port& p = port();
    if (host)   *host   = p.hostZeroRuns.load(std::memory_order_relaxed);
    if (played) *played = p.playZeroRuns.load(std::memory_order_relaxed);
}

extern "C" void usb_audio_port_set_asrc(float max_ppm, float ppm_per_frame)
{
    s_asrcMaxPpm.store(max_ppm, std::memory_order_relaxed);
    s_asrcPpmPerFrame.store(ppm_per_frame, std::memory_order_relaxed);
}

extern "C" int32_t usb_audio_port_asrc_ppm(void)
{
    return port().asrcPpm.load(std::memory_order_relaxed);
}

extern "C" uint32_t usb_audio_port_asrc_cyc_max(void)
{
    Port& p = port();
    const uint32_t n = p.asrcN ? p.asrcN : 1;
    p.asrcReadAvg = p.asrcReadSum / n; p.asrcLoopAvg = p.asrcLoopSum / n;
    p.asrcReadSum = p.asrcLoopSum = p.asrcN = 0;
    return p.asrcCycMax.exchange(0, std::memory_order_relaxed);
}

extern "C" uint32_t usb_audio_port_asrc_avg(int which)
{
    return which ? port().asrcLoopAvg : port().asrcReadAvg;
}

extern "C" uint32_t usb_audio_port_play_min(void)
{
    return port().playMin.exchange(UINT32_MAX, std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────── render side

namespace crosspad {

uint32_t UsbPlaybackInput::read(int16_t* interleaved, uint32_t frameCount)
{
    Port& p = port();
    if (!p.active.load(std::memory_order_acquire)) return 0;
    const size_t want = (size_t)frameCount * 2;
    size_t avail = p.playback.available();
    if (p.priming.load(std::memory_order_relaxed)) {
        if (avail < p.prefillSamples) {                  // silence until the host is ahead
            /* The host does not stop for an underrun -- the feedback reports
             * the clock, not this level -- so all it sends while this plays
             * silence would stay buffered for good, as latency: 16 ms more
             * per underrun. Counted here, dropped below. Past half the ring
             * it is a stream that stopped, not one that came late. */
            if (p.played && (p.playDebt += want) > p.playbackCapacity / 2) {
                p.played = false;
                p.playDebt = 0;
            }
            return 0;
        }
        p.priming.store(false, std::memory_order_relaxed);
        asrcReset(p);
        p.played = true;
        p.debtFrames = kDebtFrames;
    }
    if (p.playDebt) {
        /* Never below the prefill, never more than the silence: the level
         * goes back to where it stood before the gap, not lower. The gap is
         * already there; the delay need not outlive it. */
        if (avail > p.prefillSamples) {
            size_t drop = avail - p.prefillSamples;
            if (drop > p.playDebt) drop = p.playDebt;
            drop &= ~(size_t)1;
            p.playDebt -= drop;
            ringSkip(p.playback, interleaved, drop, want);
            avail -= drop;
        }
        if (p.debtFrames > frameCount) p.debtFrames -= frameCount;
        else { p.debtFrames = 0; p.playDebt = 0; }
    }
    const uint32_t level = (uint32_t)(avail / 2);
    if (level < p.playMin.load(std::memory_order_relaxed)) p.playMin.store(level, std::memory_order_relaxed);
    /* Input frames this block takes: exactly the hist shifts the loop below
     * makes, so nothing is read that is not used. */
    const size_t need = (size_t)((p.fracQ + (uint64_t)frameCount * p.incQ) >> 32);
    uint32_t got;
    if (avail < need * 2 || need > kAsrcScratchFrames || p.asrcIn.empty()) {
        /* Ran dry: play what is there, then wait for the prefill again rather
         * than chase the host one block at a time (that is the 250 Hz chop). */
        p.playUnderrun.fetch_add(1, std::memory_order_relaxed);
        p.priming.store(true, std::memory_order_relaxed);
        got = (uint32_t)(p.playback.read(interleaved, want) / 2);
        p.takenFrames.fetch_add(got, std::memory_order_relaxed);
        asrcReset(p);
    } else {
        const uint32_t c0 = esp_cpu_get_cycle_count();
        int16_t* in = p.asrcIn.data();
        p.playback.read(in, need * 2);
        p.takenFrames.fetch_add((uint32_t)need, std::memory_order_relaxed);
        const uint32_t c1 = esp_cpu_get_cycle_count();
        const float target = (float)(p.prefillSamples / 2 + frameCount);
        const float lvl = (float)((avail - need * 2) / 2);
        if (p.levelEma < 0.0f) p.levelEma = target;
        const float a = (float)frameCount / ((float)p.sampleRate.load(std::memory_order_relaxed) * kAsrcTauS);
        p.levelEma += (lvl - p.levelEma) * a;
        /* The history in registers for the block: through the struct, every
         * output store reloaded it, and its shift was a memmove call a frame
         * -- 590 cycles a frame, 158 us of the 1451 us block. */
        float h0l = p.hist[0][0], h0r = p.hist[0][1], h1l = p.hist[1][0], h1r = p.hist[1][1];
        float h2l = p.hist[2][0], h2r = p.hist[2][1], h3l = p.hist[3][0], h3r = p.hist[3][1];
        uint64_t frac = p.fracQ;
        const uint64_t inc = p.incQ;
        const int16_t* src = in;
        int16_t* dst = interleaved;
        for (uint32_t j = 0; j < frameCount; ++j, dst += 2) {
            /* Catmull-Rom as four weights, shared by both channels. */
            const float t = (float)(uint32_t)frac * (1.0f / 4294967296.0f);
            const float t2 = t * t, t3 = t2 * t;
            const float w0 = 0.5f * (-t3 + 2.0f * t2 - t);
            const float w1 = 0.5f * (3.0f * t3 - 5.0f * t2) + 1.0f;
            const float w2 = 0.5f * (-3.0f * t3 + 4.0f * t2 + t);
            const float w3 = 0.5f * (t3 - t2);
            dst[0] = sat16(w0 * h0l + w1 * h1l + w2 * h2l + w3 * h3l);
            dst[1] = sat16(w0 * h0r + w1 * h1r + w2 * h2r + w3 * h3r);
            frac += inc;
            for (uint32_t adv = (uint32_t)(frac >> 32); adv; --adv, src += 2) {
                h0l = h1l; h1l = h2l; h2l = h3l; h3l = (float)src[0];
                h0r = h1r; h1r = h2r; h2r = h3r; h3r = (float)src[1];
            }
            frac &= 0xFFFFFFFFull;
        }
        p.fracQ = frac;
        p.hist[0][0] = h0l; p.hist[0][1] = h0r; p.hist[1][0] = h1l; p.hist[1][1] = h1r;
        p.hist[2][0] = h2l; p.hist[2][1] = h2r; p.hist[3][0] = h3l; p.hist[3][1] = h3r;
        const float maxPpm = s_asrcMaxPpm.load(std::memory_order_relaxed);
        float ppm = (p.levelEma - target) * s_asrcPpmPerFrame.load(std::memory_order_relaxed);
        ppm = ppm > maxPpm ? maxPpm : (ppm < -maxPpm ? -maxPpm : ppm);
        p.incQ = (uint64_t)((1.0 + (double)ppm * 1e-6) * 4294967296.0);
        p.asrcPpm.store((int32_t)ppm, std::memory_order_relaxed);
        const uint32_t c2 = esp_cpu_get_cycle_count();
        const uint32_t cyc = c2 - c0;
        if (cyc > p.asrcCycMax.load(std::memory_order_relaxed)) p.asrcCycMax.store(cyc, std::memory_order_relaxed);
        p.asrcReadSum += c1 - c0; p.asrcLoopSum += c2 - c1; p.asrcN++;
        got = frameCount;
    }
    /* The Recorder's USB input: the whole block, what the host was late with
     * as silence, so a take keeps time with the host rather than skipping. */
    if (got < frameCount) std::memset(interleaved + got * 2, 0, (size_t)(frameCount - got) * 2 * sizeof(int16_t));
    countZeroRuns(interleaved, frameCount, p.playZeroLen, p.playZeroRuns);
    sample_tools_rt_usb(interleaved, frameCount);
    int16_t pl = 0, pr = 0;
    for (uint32_t i = 0; i < got; ++i) {
        int16_t l = interleaved[i * 2], r = interleaved[i * 2 + 1];
        l = l < 0 ? -l : l; r = r < 0 ? -r : r;
        if (l > pl) pl = l;
        if (r > pr) pr = r;
    }
    if (pl > peakL_.load(std::memory_order_relaxed)) peakL_.store(pl, std::memory_order_relaxed);
    if (pr > peakR_.load(std::memory_order_relaxed)) peakR_.store(pr, std::memory_order_relaxed);
    return got;
}

uint32_t UsbPlaybackInput::getSampleRate() const { return port().sampleRate.load(std::memory_order_relaxed); }
uint32_t UsbPlaybackInput::getBufferSize() const { return port().engineFrames.load(std::memory_order_relaxed); }

UsbPlaybackInput& usbPlaybackInput()
{
    static UsbPlaybackInput input;
    return input;
}

bool usbAudioPortWantsCodec(int codec)
{
    Port& p = port();
    if (!p.active.load(std::memory_order_acquire)) return false;
    const int src = p.source.load(std::memory_order_relaxed);
    return src == USB_AUDIO_PORT_CAPTURE_MIX || src == codec;
}

void usbAudioPortCaptureTap(const int16_t* rx0, const int16_t* rx1, uint32_t frames)
{
    Port& p = port();
    if (!p.active.load(std::memory_order_acquire)) return;
    /* Nobody reading while the host has the mic stream closed: nothing is
     * written, and nothing counts as dropped. The mic task drops what is
     * stale when the stream opens (usb_audio_port_capture_read); a full ring
     * while it is open is an overflow, counted below. */
    if (!p.capOpen.load(std::memory_order_acquire)) return;
    const int src = p.source.load(std::memory_order_relaxed);
    const int16_t* single = (src == 1) ? rx1 : rx0;
    size_t wrote = 0;
    if (src == USB_AUDIO_PORT_CAPTURE_MIX) {
        /* Summed with int16 saturation, block by block; both codecs run off
         * the same BCLK so their blocks line up sample for sample. */
        int16_t sum[2 * 64];
        const uint32_t samples = frames * 2;
        for (uint32_t off = 0; off < samples; off += sizeof(sum) / sizeof(sum[0])) {
            uint32_t n = samples - off;
            if (n > sizeof(sum) / sizeof(sum[0])) n = sizeof(sum) / sizeof(sum[0]);
            for (uint32_t i = 0; i < n; ++i) {
                int32_t v = (rx0 ? rx0[off + i] : 0) + (rx1 ? rx1[off + i] : 0);
                sum[i] = (int16_t)(v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v));
            }
            wrote += p.capture.write(sum, n);
        }
    } else if (single) {
        wrote = p.capture.write(single, frames * 2);
    } else {
        return;
    }
    if (wrote < (size_t)frames * 2) {
        p.capDropped.fetch_add((uint32_t)(frames - wrote / 2), std::memory_order_relaxed);
    }
}

void usbAudioPortCaptureBus(const int16_t* stereo, uint32_t frames)
{
    Port& p = port();
    if (!p.active.load(std::memory_order_acquire)) return;
    if (p.source.load(std::memory_order_relaxed) != USB_AUDIO_PORT_CAPTURE_SAMPLER) return;
    /* Written only while the host has the mic open, as the ADC tap is. */
    if (!stereo || !p.capOpen.load(std::memory_order_acquire)) return;
    const size_t wrote = p.capture.write(stereo, (size_t)frames * 2);
    if (wrote < (size_t)frames * 2) {
        p.capDropped.fetch_add((uint32_t)(frames - wrote / 2), std::memory_order_relaxed);
    }
}

} // namespace crosspad
