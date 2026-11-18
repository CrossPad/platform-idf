/* CDC verbs: pads, kits, the sample engines and the pitched instrument. Dispatch order is in hil_control.cpp. */
#include "i2s_driver.h"
#include "include/hil_control.h"
#include "include/hil_text.h"
#include "stm32_i2c.h"
#include "stm32_uart.h"
#include "stm32_reg_map.h"

#include "tinyusb.h"
#include "include/lv_async_safe.h"

#include "gui.h"
#include "audio/audio_task.h"
#if __has_include(<crosspad-mixer/AudioMixerEngine.hpp>)
#include <crosspad-mixer/AudioMixerEngine.hpp>
#include <crosspad-mixer/MixerPresets.hpp>
#define HIL_HAS_MIXER 1
#endif
#include "crosspad/audio/IInputMonitor.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sampletools/SliceMath.hpp"
#if __has_include("crosspad-dawcontrol/DawControlState.hpp")
#include "crosspad-dawcontrol/DawControlState.hpp"
#endif
#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad-sampler/waveform/waveform_cache.hpp"
#include "crosspad-sampler/waveform/waveform_loader.hpp"

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
#include "ml_smpl_stream_ply.h"
#include "MarcelSampleNode.hpp"
#endif
#include "SamplerEnginePort.hpp"
#include "PitchedKitPort.hpp"
#include "engine_arena.hpp"
#include "crosspad/instrument/IInstrumentHost.hpp"
#include "instrument_host.h"
#include "PitchedSampleNode.hpp"
#include "crosspad/dsp/FftFactory.hpp"
#include "crosspad/dsp/PortableFft.hpp"
#include "sample_player_port.hpp"
#include "SamplerPlatformPort.hpp"

#include "hil_verbs.h"

static const char *TAG = "hil_control";

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/* Free sample slots of whichever engine holds the kit (-1: none does). */
static int hil_free_slots(void)
{
    const int n = native_sampler_free_slots();
    return n >= 0 ? n : marcel_engine_free_slots();
}
#else
static int hil_free_slots(void) { return native_sampler_free_slots(); }
#endif

/* KIT_DELETE: the kit browser's delete, on the thread the browser runs it on. */
static int s_delete_kit = -1;

static void async_kit_delete(void *)
{
    auto *mgr = crosspad::getKitManager();
    const bool ok = mgr && mgr->removeKit(s_delete_kit);
    ESP_LOGI(TAG, "KIT_DELETE %d %s", s_delete_kit, ok ? "done" : "refused");
}

/* PAD_MIX / KIT_SAVE: a host mixer (crosspad-hil padmix) riding the current
 * kit's pad volumes the way the Pad Mixer app does, then writing kit.json.
 * The kit model is the LVGL thread's, so a set is parked here per pad --
 * newest wins, a held arrow key on the host coalesces -- and applied there,
 * only to the kit it was asked for. */
static std::atomic<int> s_mix_pending[KIT_PADS];   /* volume + 1; 0 = nothing pending */
static std::atomic<int> s_mix_kit{-1};
static std::atomic<bool> s_mix_posted{false};

static void mix_apply_pending(void)
{
    auto *mgr = crosspad::getKitManager();
    crosspad::KitInfo *kit = (mgr && mgr->getCurrentKitId() == s_mix_kit.load()) ? mgr->getCurrentKit() : nullptr;
    auto *mix = crosspad::getPlatformServices().padMix;
    for (int i = 0; i < KIT_PADS; ++i) {
        const int v = s_mix_pending[i].exchange(0);
        if (!v || !kit) continue;
        crosspad::Pad &pad = kit->pads[i];
        pad.volume = v - 1;
        pad.dirtyModified = true;   /* as the Pad Mixer marks it: the Sampler's save prompt offers it */
        if (mix) mix->setPadMix((uint8_t)i, pad.volume, pad.pan);
    }
}

static void async_pad_mix(void *)
{
    s_mix_posted.store(false);   /* first: a set arriving meanwhile posts again */
    mix_apply_pending();
}

static std::atomic<bool> s_kit_save_busy{false};
static std::atomic<int> s_kit_save_ok{-1};
static std::atomic<int> s_kit_save_kit{-1};

/* On the LVGL thread, where the Sampler's own save runs. */
static void async_kit_save(void *)
{
    mix_apply_pending();
    auto *mgr = crosspad::getKitManager();
    crosspad::KitInfo *kit = mgr ? mgr->getCurrentKit() : nullptr;
    bool ok = false;
    /* An unparsed kit carries sixteen empty pads: writing it would take every
     * sample out of the kit. */
    if (kit && kit->parsed && mgr->getCurrentKitId() == s_kit_save_kit.load() &&
        !crosspad_sampler::kit_load_task_is_loading()) {
        ok = mgr->saveKit(*kit);
        if (ok) {
            for (int i = 0; i < KIT_PADS; ++i) kit->pads[i].dirtyModified = false;
        }
    }
    s_kit_save_ok.store(ok ? 1 : 0);
    s_kit_save_busy.store(false);
    ESP_LOGI(TAG, "KIT_SAVE %d %s", s_kit_save_kit.load(), ok ? "done" : "failed");
}

/* For the CDC verbs: the hil_pad task runs at 6 on this core, so a send
 * hands it the CPU before the reply goes out -- and a hit can wait on the card
 * for a second while a kit loads. Room is checked first (only an error is
 * worth answering late), the reply goes out, then the hit is queued. */
static void pad_inject_reply_first(HilPadInject kind, uint8_t pad, uint8_t val, void (*reply)(const char*))
{
    if (!s_pad_inject_queue || uxQueueSpacesAvailable(s_pad_inject_queue) == 0) {
        ++g_hil_pad_inject_drop;
        reply("ERR busy\r\n");
        return;
    }
    reply("OK\r\n");
    hil_pad_inject(kind, pad, val);
}

/* The last kit loads, one row each, oldest first: when they were asked for,
 * how long the parse and the pad loop took, which pads waited for their voice
 * to end, and what the engine skipped meanwhile. A swap that sounded wrong
 * becomes numbers here. */
static void handle_kit_swap_log(void)
{
    /* Static, both of them: this runs on the main task's stack, which the
     * CDC drain loop keeps small. One record at a time — a ring's worth of
     * them was 1.2 kB of internal RAM for a verb that prints. */
    static crosspad_sampler::KitSwapRecord r;
    EXT_RAM_BSS_ATTR static char out[512];
    static const char* const kWhy[] = {"-", "parse", "none_readable"};   /* KitLoadWhy */
    const int n = crosspad_sampler::kit_load_task_swap_log_count();
    for (int i = 0; i < n; ++i) {
        if (!crosspad_sampler::kit_load_task_swap_log(i, &r)) break;
        size_t len = snprintf(out, sizeof(out),
                              "KITSWAP: seq=%u kit=%d ok=%d req_ms=%u parse_ms=%u done_ms=%u "
                              "gated_blocks=%u gate_max_us=%u gate_total_us=%u gate_count=%u "
                              "underruns=%u noblock=%u deferred=0x%04x forced=0x%04x "
                              "dropped_hits=%u late_hits=%u rt_gap_max_us=%u "
                              "stream_fopens=%u bg_pass_max_us=%u fopen_max_us=%u "
                              "stream_kb=%u loader_kb=%u empty=0x%04x failed=0x%04x why=%s pads=",
                              (unsigned)r.seq, r.kitId, r.ok ? 1 : 0, (unsigned)r.reqMs,
                              (unsigned)r.parseMs, (unsigned)r.doneMs,
                              (unsigned)r.engine.gatedBlocks, (unsigned)r.engine.gateMaxUs,
                              (unsigned)r.engine.gateTotalUs, (unsigned)r.engine.gateCount,
                              (unsigned)r.engine.underruns, (unsigned)r.engine.noFreeBlock,
                              (unsigned)r.deferred, (unsigned)r.forced,
                              (unsigned)r.engine.droppedHits, (unsigned)r.engine.lateHits,
                              (unsigned)r.engine.rtGapMaxUs, (unsigned)r.engine.streamFopens,
                              (unsigned)r.engine.bgPassMaxUs, (unsigned)r.engine.fopenMaxUs,
                              (unsigned)r.engine.streamKb, (unsigned)r.engine.loaderKb,
                              (unsigned)r.empty, (unsigned)r.failed, kWhy[(unsigned)r.why < sizeof kWhy / sizeof kWhy[0] ? (unsigned)r.why : 0]);
        for (int p = 0; p < 16 && len < sizeof(out) - 12; ++p) {
            len += snprintf(out + len, sizeof(out) - len, "%s%d", p ? "," : "",
                            (int)r.padDoneMs[p]);
        }
        snprintf(out + len, sizeof(out) - len, "\r\n");
        cdc_reply(out);
    }
    /* Since boot: preloads read around stdio as one CMD18 per 4 kB, and the
     * ones that fell back to it (no DMA-capable RAM, or no fd position). */
    snprintf(out, sizeof(out), "KITSWAP: io staged=%u fallback=%u\r\n",
             (unsigned)g_port_staged, (unsigned)g_port_stage_fallback);
    cdc_reply(out);
    snprintf(out, sizeof(out), "KITSWAP: end count=%d\r\n", n);
    cdc_reply(out);
}

static void handle_kit_status(void)
{
    char reply[160];
    hil_text_kit_status(reply, sizeof(reply),
                        crosspad::control::readKitStatus(crosspad_sampler::kit_load_task_is_loading(),
                                                         crosspad_sampler::kit_load_task_pending_kit()));
    cdc_reply(reply);
}

/* Waveform cache coverage per sample layer: which pad:layer the panel would
 * draw empty, plus the loader's counters. */
/* Last eight note-ons the engine executed, with the STM's last decoded press
 * beside them — pad 8 sounding like pad 4 is one or the other. */
static void handle_smpl_last(void)
{
    EXT_RAM_BSS_ATTR static char out[640];
    EXT_RAM_BSS_ATTR static uint32_t s_lastBytes, s_lastMs;
    const uint32_t total = g_engine_last_count.load();
    const uint32_t n = total < 8 ? total : 8;
    const uint32_t bytes = g_port_stream_bytes.load(std::memory_order_relaxed);
    const uint32_t nowMs = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t dMs = s_lastMs ? nowMs - s_lastMs : 0;
    size_t len = snprintf(out, sizeof(out), "SMPLLAST: stm_presses=%u stm_note=%u stm_pad=%d engine_total=%u events=",
                          (unsigned)g_hil_stm_press_count.load(), (unsigned)g_hil_stm_last_note.load(),
                          (int)(int8_t)g_hil_stm_last_pad.load() == -1 ? -1 : (int)g_hil_stm_last_pad.load(),
                          (unsigned)total);
    for (uint32_t i = 0; i < n; ++i) {
        const EngineNoteEvent &e = g_engine_last[(total - n + i) % 8];
        len += snprintf(out + len, sizeof(out) - len, "%s%u:%u@%u", i ? "," : "", e.note, e.wavIdx, (unsigned)e.ms);
    }
    snprintf(out + len, sizeof(out) - len,
             " opens=%u/%u(%s) seeks=%u/%u(%s) reads=%u/%u(%s) closes=%u/%u(%s) defer=%u/%u evict=%u"
             " lat_us last=%u avg=%u max=%u n=%u"
             " stream_staged=%u stream_fb=%u stream_kbps=%u stream_read_max_us=%u late_max_ms=%u late_lost=%u pf=%u/%u win_max_us=%u"
             " pool_fixed=0\r\n",
             (unsigned)g_port_opens.audio_bg, (unsigned)g_port_opens.other, g_port_opens.lastOther,
             (unsigned)g_port_seeks.audio_bg, (unsigned)g_port_seeks.other, g_port_seeks.lastOther,
             (unsigned)g_port_reads.audio_bg, (unsigned)g_port_reads.other, g_port_reads.lastOther,
             (unsigned)g_port_closes.audio_bg, (unsigned)g_port_closes.other, g_port_closes.lastOther,
             (unsigned)g_port_defer, (unsigned)g_port_defer_lost, (unsigned)g_port_evict,
             (unsigned)g_note_lat_last_us.load(),
             (unsigned)(g_note_lat_count.load() ? g_note_lat_sum_us.load() / g_note_lat_count.load() : 0),
             (unsigned)g_note_lat_max_us.load(), (unsigned)g_note_lat_count.load(),
             (unsigned)g_port_stream_staged, (unsigned)g_port_stream_fb,
             (unsigned)(dMs ? (bytes - s_lastBytes) / dMs : 0),   /* B/ms = kB/s */
             (unsigned)g_port_stream_read_max_us, (unsigned)g_late_max_ms, (unsigned)g_late_lost,
             (unsigned)g_port_pf_hits, (unsigned)g_port_pf_misses, (unsigned)g_reload_win_max_us);
    /* Rate, worst read and held hits since the previous SMPLLAST. */
    s_lastBytes = bytes;
    s_lastMs = nowMs;
    g_port_stream_read_max_us = 0;
    g_late_max_ms = 0;
    g_late_lost = 0;
    g_reload_win_max_us = 0;
    cdc_reply(out);
}

static void handle_wave_status(void)
{
    crosspad_sampler::WaveformLoaderStats loader;
    crosspad_sampler::waveform_loader_get_stats(&loader);

    /* In PSRAM: this board boots with a couple of hundred bytes of
     * internal RAM to spare, and a reply buffer is read and written
     * only from the CDC loop, never with the cache off. */
    EXT_RAM_BSS_ATTR static crosspad::control::WaveStatus st;
    EXT_RAM_BSS_ATTR static char out[1024];
    /* A cache lookup per layer: resolving a path stats the card, and this is
     * the prio-1 main loop. */
    crosspad::control::readWaveCoverage(st, crosspad_sampler::waveform_cache_contains);
    crosspad::control::setWaveLoader(st, loader, crosspad_sampler::waveform_loader_is_running());
    hil_text_wave_status(out, sizeof(out), st);
    cdc_reply(out);
}

static void handle_kit_list(void)
{
    auto *mgr = crosspad::getKitManager();
    if (!mgr) {
        cdc_reply("ERR no kit manager\r\n");
        return;
    }
    if (!g_kit_scan_done.load()) {
        /* A bare "ERR …", not a "KITS: ERR …" row: the host's multi-line reader
         * accepts an ERR line *in place of* the first row and finishes there,
         * so a scenario gets an error at once instead of idling out the read
         * waiting for a "KITS: end" that is not coming. */
        cdc_reply("ERR scanning\r\n");
        return;
    }
    /* Multi-line, like APP_VERSIONS: one line per batch of kits, then
     * "KITS: end count=N current=M". A single line has been outgrown twice —
     * 256 bytes silently listed the first 17 of 43, then 1024 listed 77 of 83 —
     * and each time the truncation was invisible, so a kit that was on the card
     * and in memory looked like a kit that had failed to copy. */
    /* In PSRAM: this board boots with a couple of hundred bytes of
     * internal RAM to spare, and a reply buffer is read and written
     * only from the CDC loop, never with the cache off. */
    EXT_RAM_BSS_ATTR static char out[512];
    HilKitListText text{out, sizeof(out), 0, 0, cdc_reply};
    hil_text_kit_list_begin(text);
    int listed = 0;
    crosspad::control::readKitList(*mgr, [&text, &listed](const crosspad::control::KitListRow &row) {
        if (!row.removed) ++listed;
        hil_text_kit_list_add(text, row);
    });
    /* count= is what was listed, so a host can tell a dropped line from a
     * deleted kit (its id stays, blank). */
    hil_text_kit_list_end(text, listed, mgr->getCurrentKitId());
}

static void handle_pad_stats(void)
{
    /* Every stage a pad hit crosses here, so a missing burst can be placed:
     * STM link → parsed presses → sampler → USB MIDI. */
    char reply[200];
    snprintf(reply, sizeof(reply),
             "PADSTATS: press=%u release=%u inj_drop=%u played=%u last=%u:%u freeslots=%d stm=%u at=%u rxmsg=%u "
             "uart_ovf=%u sbuf_drop=%u usb_tx=%u usb_drop=%u\r\n",
             (unsigned)g_hil_pad_press_count.load(),
             (unsigned)g_hil_pad_release_count.load(),
             (unsigned)g_hil_pad_inject_drop.load(),
             (unsigned)g_hil_note_on_count.load(),
             (unsigned)(g_hil_last_note_on.load() >> 8), (unsigned)(g_hil_last_note_on.load() & 0x7F),
             hil_free_slots(),
             (unsigned)g_hil_stm_press_count.load(),
             (unsigned)g_hil_stm_at_count.load(),
             (unsigned)stm32_get_rx_msg_count(false),
             (unsigned)g_stm_uart_ovf.load(),
             (unsigned)g_stm_sbuf_drop.load(),
             (unsigned)g_usb_midi_tx_ok.load(),
             (unsigned)g_usb_midi_tx_fail.load());
    cdc_reply(reply);
}

/* ── Memory ────────────────────────────────────────────────────────── */

/* The note each pad answers to. A kit redefines them, so a DAW or BLE host
 * has to ask rather than assume the 36+pad default. */
static void handle_pad_notes(void)
{
    auto &pads = crosspad::getPadManager();
    char reply[160];
    int n = snprintf(reply, sizeof(reply), "PADNOTES:");
    for (uint8_t i = 0; i < 16; ++i) {
        n += snprintf(reply + n, sizeof(reply) - n, " %u:%u",
                      (unsigned)i, (unsigned)pads.getMidiNoteForPad(i));
    }
    snprintf(reply + n, sizeof(reply) - n, "\r\n");
    cdc_reply(reply);
}

/* ── PITCHED_BENCH ─────────────────────────────────────────────────── */
/* The measurement behind the design's rejection of a per-voice phase vocoder:
 * what an FFT and an IFFT actually cost on this silicon against the 1.45 ms
 * block, beside what a Hermite voice costs. It also checks the FFT core
 * settled on (esp-dsp when its twiddle table could be built) against
 * crosspad-core's PortableFft on the same data.
 *
 * It runs on its own task, not the CDC drain loop: that stack has about a kB
 * of headroom and the one-shot table init's first ESP_LOGI trips the stack
 * canary on it. The reply is therefore asynchronous — the verb returns at once
 * and the rows arrive as they are measured. All temporaries come from PSRAM:
 * the FFT buffers because the aes3 kernel wants 16-byte alignment and nobody's
 * stack is 96 kB, the voice zone because it is a second of audio. */

/* One render block, in frames — the RT budget everything here is compared to. */
static constexpr uint32_t kBenchFrames = 64;
/* Voices are re-armed every this many blocks: at ~1.5x a 1 s zone lasts about
 * 460 blocks, so the voices must be restarted before they run out. */
static constexpr int kBenchRearmBlocks = 128;
/* Iteration bounds. The upper one is what keeps a run inside a sane host
 * timeout, given the tick this task yields per iteration. */
static constexpr int kBenchMinIters = 1;
static constexpr int kBenchMaxIters = 500;
static constexpr int kBenchDefaultIters = 20;
/* Synthetic zone: one second of C4 at root 60, played from root+7 so the read
 * increment is not 1.0. */
static constexpr double kBenchZoneHz = 261.63;
static constexpr float  kBenchZoneRoot = 60.0f;
static constexpr double kBenchZoneAmp = 12000.0;
static constexpr uint8_t kBenchFirstNote = 67;

static void *bench_alloc(size_t bytes)
{
    return heap_caps_aligned_alloc(16, (bytes + 15u) & ~(size_t)15, MALLOC_CAP_SPIRAM);
}

static void bench_free(void *p) { heap_caps_free(p); }

/* FftMemory's allocator takes the alignment esp-dsp's aes3 kernel wants; the
 * bench already allocates at 16, which is that alignment. */
static void *bench_fft_alloc(size_t bytes, size_t align)
{
    if (align < 16) align = 16;
    return heap_caps_aligned_alloc(align, (bytes + align - 1u) & ~(align - 1u), MALLOC_CAP_SPIRAM);
}

static void bench_row(const char *name, double us)
{
    char line[64];
    snprintf(line, sizeof(line), "BENCH: %s=%.2f\r\n", name, us);
    cdc_reply(line);
}

static void handle_pitched_bench(int iters)
{
    constexpr uint32_t kMaxN = 4096;                 /* largest size benched */
    const size_t floats = (size_t)kMaxN * 2;         /* interleaved [re, im] */

    float *ref  = (float *)bench_alloc(floats * sizeof(float));
    float *work = (float *)bench_alloc(floats * sizeof(float));
    float *cmp  = (float *)bench_alloc(floats * sizeof(float));
    if (!ref || !work || !cmp) {
        bench_free(ref); bench_free(work); bench_free(cmp);
        cdc_reply("BENCH: ERR no PSRAM\r\n");
        return;
    }

    /* Random input scaled by 1/sqrt(N): the spectrum then sits at unit scale,
     * so fft_maxdiff is an absolute number that means what it looks like. */
    const float amp = 1.0f / 64.0f;
    unsigned seed = 12345u;
    for (size_t i = 0; i < floats; ++i) {
        seed = seed * 1103515245u + 12345u;
        ref[i] = amp * ((float)((seed >> 16) & 0x7FFF) / 16383.5f - 1.0f);
    }

    /* The one FFT this process uses -- created by the kit port on the first
     * pitched load, so on a board that has not loaded one yet this call is what
     * builds it. Its table memory is the verb's own aligned PSRAM; core keeps
     * whichever backend it settles on for the life of the process. */
    crosspad::IFft *cur = crosspad::currentFft();
    if (!cur) {
        crosspad::FftMemory mem;
        mem.alloc = bench_fft_alloc;
        mem.free  = bench_free;
        mem.log   = nullptr;
        cur = &crosspad::createFft(mem);
    }
    crosspad::IFft      &esp = *cur;
    crosspad::PortableFft port;

    auto bench = [&](crosspad::IFft &f, uint32_t n, bool inverse) -> double {
        int64_t total = 0;
        for (int i = 0; i < iters; ++i) {
            memcpy(work, ref, (size_t)n * 2 * sizeof(float));
            const int64_t t0 = esp_timer_get_time();
            if (inverse) f.inverse(work, n); else f.forward(work, n);
            total += esp_timer_get_time() - t0;
            /* Outside the timed window, so it costs the measurement nothing.
             * This task sits one priority above idle on core 0: an unbroken run
             * starves the idle task the WDT feeds, and PITCHED_BENCH 500 is far
             * past 5 s of back-to-back transforms. The tick per iteration is
             * why a large iters needs a matching host timeout. */
            vTaskDelay(1);
        }
        return (double)total / iters;
    };

    bench_row("fft1024_fwd_us", bench(esp, 1024, false));
    bench_row("fft1024_inv_us", bench(esp, 1024, true));
    bench_row("fft2048_fwd_us", bench(esp, 2048, false));
    bench_row("fft4096_fwd_us", bench(esp, 4096, false));
    bench_row("fft4096_inv_us", bench(esp, 4096, true));
    bench_row("portable_fft4096_fwd_us", bench(port, 4096, false));

    memcpy(work, ref, floats * sizeof(float));
    memcpy(cmp,  ref, floats * sizeof(float));
    double maxdiff = 0.0;
    if (esp.forward(work, kMaxN) && port.forward(cmp, kMaxN)) {
        for (size_t i = 0; i < floats; ++i) {
            const double d = fabs((double)work[i] - (double)cmp[i]);
            if (d > maxdiff) maxdiff = d;
        }
    } else {
        maxdiff = -1.0;
    }
    char line[64];
    snprintf(line, sizeof(line), "BENCH: fft_maxdiff=%.3e\r\n", maxdiff);
    cdc_reply(line);

    bench_free(ref); bench_free(work); bench_free(cmp);

    /* One Hermite voice, one block — the real per-voice cost to compare the
     * FFT numbers against. */
    const uint32_t rate = bsp_i2s_get_sample_rate() ? bsp_i2s_get_sample_rate() : 44100;
    auto *bank = new (std::nothrow) crosspad::SampleBank(bench_alloc, bench_free);
    auto *inst = new (std::nothrow) crosspad::PitchedInstrument();
    float *out = (float *)bench_alloc(kBenchFrames * 2 * sizeof(float));
    if (!bank || !inst || !out) {
        delete bank; delete inst; bench_free(out);
        cdc_reply("BENCH: ERR no PSRAM for voice\r\n");
        return;
    }
    int16_t *pcm = bank->allocPcm(rate);
    if (!pcm) {
        delete bank; delete inst; bench_free(out);
        cdc_reply("BENCH: ERR no PSRAM for zone\r\n");
        return;
    }
    const double w = 2.0 * M_PI * kBenchZoneHz / (double)rate;
    for (uint32_t i = 0; i < rate; ++i) {
        pcm[i] = (int16_t)(kBenchZoneAmp * sin(w * (double)i));
    }
    crosspad::Zone z;
    z.pcm = pcm; z.length = rate; z.rootMidi = kBenchZoneRoot;
    bank->addZone(z);

    crosspad::InstrumentParams params;
    params.maxVoices = crosspad::PitchedInstrument::kMaxVoices;
    params.attackMs  = 2.0f;
    params.releaseMs = 1.0f;
    inst->setParams(params);
    inst->onPrepare(kBenchFrames, rate);
    inst->setBank(bank);
    for (uint8_t p = 0; p < crosspad::PitchedInstrument::kPads; ++p) {
        inst->setPadMix(p, 100, 0);
    }

    auto benchVoices = [&](int voices) -> double {
        int64_t total = 0;
        for (int i = 0; i < iters; ++i) {
            if ((i % kBenchRearmBlocks) == 0) {
                inst->allNotesOff();
                for (int v = 0; v < voices; ++v) {
                    inst->noteOn((uint8_t)(kBenchFirstNote + v), 100,
                                 (uint8_t)(v % crosspad::PitchedInstrument::kPads));
                }
            }
            memset(out, 0, kBenchFrames * 2 * sizeof(float));
            const int64_t t0 = esp_timer_get_time();
            inst->mix(out, kBenchFrames);
            total += esp_timer_get_time() - t0;
            vTaskDelay(1);   /* idle-task watchdog, as above */
        }
        inst->allNotesOff();
        return (double)total / iters;
    };

    bench_row("voice64_us", benchVoices(1));
    bench_row("voice64x16_us", benchVoices(crosspad::PitchedInstrument::kPads));

    inst->setBank(nullptr);
    delete inst;
    delete bank;
    bench_free(out);
    cdc_reply("OK\r\n");
}

static volatile bool s_bench_busy = false;
static int s_bench_iters = kBenchDefaultIters;

static void pitched_bench_task(void *)
{
    handle_pitched_bench(s_bench_iters);
    s_bench_busy = false;
    /* Created WithCaps, so it must be deleted WithCaps or the stack leaks. */
    vTaskDeleteWithCaps(nullptr);
}

bool verb_pad_notes(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "PAD_NOTES", 9) == 0) {
        handle_pad_notes();
        return true;
    }
    return false;
}

bool verb_pad_stats_reset(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "PAD_STATS_RESET", 15) == 0) {
        g_hil_pad_press_count = 0;
        g_hil_pad_release_count = 0;
        g_hil_pad_inject_drop = 0;
        g_hil_note_on_count = 0;
        g_hil_last_note_on = 0;
        g_hil_stm_press_count = 0;
        g_hil_stm_at_count = 0;
        g_stm_uart_ovf = 0;
        g_stm_sbuf_drop = 0;
        g_usb_midi_tx_ok = 0;
        g_usb_midi_tx_fail = 0;
        stm32_get_rx_msg_count(true);
        g_note_lat_last_us = 0; g_note_lat_max_us = 0; g_note_lat_count = 0; g_note_lat_sum_us = 0;
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_stm_vpad(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STM_VPAD", 8) == 0) {
        // STM-side virtual pads: STM_VPAD <rate_hz> <count> [hold_ms] [mode]
        // | STOP | (no args = status). Hits leave the STM on the physical
        // pad's own path — note-on, pressure stream, note-off — so the whole
        // link is measured.
        if (strncmp(cmd + 8, " STOP", 5) == 0) {
            cdc_reply(stm32_i2c_write_u8(REG_CMD_VPAD, 0) == ESP_OK ? "OK\r\n" : "ERR i2c\r\n");
            return true;
        }
        unsigned rate = 0, count = 0, hold = 100, mode = 0;
        int n = sscanf(cmd + 8, "%u %u %u %u", &rate, &count, &hold, &mode);
        if (n >= 2) {
            if (rate < 1 || rate > 65535 || count < 1 || count > 65535 || hold > 255 || mode > 255) {
                cdc_reply("ERR STM_VPAD <rate_hz> <count> [hold_ms] [mode]\r\n");
                return true;
            }
            uint8_t cfg[6] = { (uint8_t)(rate >> 8), (uint8_t)rate, (uint8_t)(count >> 8), (uint8_t)count,
                               (uint8_t)hold, (uint8_t)mode };
            if (stm32_i2c_write(REG_CTL_VPAD_RATE_HI, cfg, sizeof(cfg)) != ESP_OK ||
                stm32_i2c_write_u8(REG_CMD_VPAD, 1) != ESP_OK) {
                cdc_reply("ERR i2c\r\n");
                return true;
            }
        }
        uint8_t d[36];
        if (stm32_i2c_read(REG_DEBUG_VPAD, d, sizeof(d)) != ESP_OK) {
            cdc_reply("ERR i2c\r\n");
            return true;
        }
        uint32_t w[9];
        for (int i = 0; i < 9; i++)
            w[i] = d[4*i] | (d[4*i+1] << 8) | (d[4*i+2] << 16) | ((uint32_t)d[4*i+3] << 24);
        char out[160];
        snprintf(out, sizeof(out),
                 "VPAD: sent=%lu at_sent=%lu remaining=%lu short_esp=%lu short_usb=%lu esp_busy=%lu usb_busy=%lu esp_hwm=%lu usb_hwm=%lu\r\n",
                 (unsigned long)w[0], (unsigned long)w[8], (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3],
                 (unsigned long)w[4], (unsigned long)w[5], (unsigned long)w[6], (unsigned long)w[7]);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_stool(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "STOOL", 5) == 0 && (cmd[5] == 0 || cmd[5] == ' ')) {
        auto* st = crosspad::getPlatformServices().sampleTools;
        if (!st) { cdc_reply("ERR STOOL no sample tools\r\n"); return true; }
        static std::vector<uint32_t> s_slices;
        static uint32_t s_sliceEnd = 0;
        static std::string s_sliceFile;
        static uint16_t s_sliceAtk = 0;
        static int s_sliceSemis = 0;
        EXT_RAM_BSS_ATTR static char path[128];
        const char* arg = cmd + 5;
        while (*arg == ' ') ++arg;
        unsigned a = 0, b = 0, c = 0;
        bool ok = true;
        if      (strcmp(arg, "IN MIC") == 0)   ok = st->selectInput(crosspad::RecInput::Mic);
        else if (strcmp(arg, "IN LINE") == 0)  ok = st->selectInput(crosspad::RecInput::Line);
        else if (strcmp(arg, "IN PADS") == 0)  ok = st->selectInput(crosspad::RecInput::Pads);
        else if (strcmp(arg, "IN USB") == 0)   ok = st->selectInput(crosspad::RecInput::Usb);
        else if (strcmp(arg, "REC") == 0)      ok = st->startRecording();
        else if (sscanf(arg, "SYNC %u %u", &a, &b) == 2) ok = st->startRecordingWithSong(a, (uint8_t)b);
        else if (sscanf(arg, "SYNC %u", &a) == 1) ok = st->startRecordingWithSong(a);
        else if (strncmp(arg, "DIR", 3) == 0 && (arg[3] == 0 || arg[3] == ' ')) {
            const char* d = arg + 3;
            while (*d == ' ') ++d;
            ok = st->requestFolder(d);
        }
        else if (strcmp(arg, "STOP") == 0)     st->stopRecording();
        else if (strcmp(arg, "RELEASE") == 0)  st->releaseInput();
        else if (strcmp(arg, "LIST") == 0)     ok = st->requestRecordings();
        else if (strcmp(arg, "PSTOP") == 0)    st->stopPreview();
        else if (sscanf(arg, "OV %127s %u %u", path, &a, &b) >= 1) ok = st->requestOverview(path, a, b, 300);
        else if (sscanf(arg, "PLAY %127s %u %u %u", path, &a, &b, &c) == 4) ok = st->preview(path, a, b, c != 0);
        else if (sscanf(arg, "SLICE %127s %u %u", path, &a, &b) == 3) {
            crosspad::PreviewSound snd;
            snd.slice = true;
            snd.attackMs = s_sliceAtk;
            snd.semitones = (int8_t)s_sliceSemis;
            ok = st->previewAs(path, a, b, false, snd);
        }
        else if (sscanf(arg, "ATK %u", &a) == 1) s_sliceAtk = (uint16_t)a;
        else if (sscanf(arg, "PITCH %d", &s_sliceSemis) == 1) {}
        else if (strncmp(arg, "FX ", 3) == 0) {
            /* A sampler pad's copy: path out semitones hp lp reverse(0|1) [keep(0|1)]. */
            EXT_RAM_BSS_ATTR static char out[128];
            int semis = 0;
            unsigned hp = 0, lp = 0, rev = 0, keep = 0;
            crosspad::SampleFx fx;
            ok = sscanf(arg + 3, "%127s %127s %d %u %u %u %u", path, out, &semis, &hp, &lp, &rev, &keep) >= 6;
            fx.semitones = semis;
            fx.hpHz = (uint16_t)hp;
            fx.lpHz = (uint16_t)lp;
            fx.reverse = rev != 0;
            fx.keepLength = keep != 0;
            ok = ok && st->requestRendered(path, out, fx);
        }
        else if (sscanf(arg, "SAVE %127s %u %u", path, &a, &b) == 3) ok = st->requestSaveRegion(path, a, b);
        else if (sscanf(arg, "HANDOFF %127s %u %u", path, &a, &b) == 3) st->setHandoff(path, a, b);
        else if (sscanf(arg, "ONSETS %u", &a) == 1) {
            const crosspad::WaveOverview ov = st->overview();
            s_slices = crosspad::SliceMath::onsets(ov.envelope, ov.hop, ov.start, ov.start, ov.end,
                                                   (int)a, ov.sampleRate / 20);
            s_sliceEnd = ov.end;
            s_sliceFile = ov.file;
        }
        else if (sscanf(arg, "EQ %u", &a) == 1) {
            const crosspad::WaveOverview ov = st->overview();
            s_slices = crosspad::SliceMath::equal(ov.start, ov.end, (uint8_t)a);
            s_sliceEnd = ov.end;
            s_sliceFile = ov.file;
        }
        else if (strncmp(arg, "EXPORT ", 7) == 0) {
            crosspad::SliceKit kit;
            kit.source = s_sliceFile;
            kit.name = arg + 7;
            kit.starts = s_slices;
            kit.end = s_sliceEnd;
            kit.attackMs = s_sliceAtk;
            kit.semitones = (int8_t)s_sliceSemis;
            ok = st->requestExportKit(kit);
        }
        else if (*arg) { cdc_reply("ERR STOOL IN MIC|LINE|PADS|USB|REC|SYNC bar [beats]|DIR [path]|STOP|RELEASE|LIST|OV path [a b]|ONSETS sens|EQ n|PLAY path a b loop|SLICE path a b|ATK ms|PITCH st|PSTOP|SAVE path a b|EXPORT name|HANDOFF path a b|FX path out st hp lp rev [keep]\r\n"); return true; }

        float pl = 0, pr = 0;
        st->peaks(pl, pr);
        const crosspad::RecStatus rs = st->recStatus();
        const crosspad::WaveOverview ov = st->overview();
        const std::vector<std::string> recs = st->recordings();
        EXT_RAM_BSS_ATTR static char reply[480];
        constexpr int kTail = 3;
        int n = snprintf(reply, sizeof(reply) - kTail,
                         "STOOL: ok=%d in=%d pk=%d/%d rec=%d song=%d wait=%d from=%u frames=%u sr=%u job=%d prog=%u prev=%d at=%u "
                         "ov=%u..%u/%u cols=%u hops=%u recs=%u slices=%u:",
                         ok ? 1 : 0, (int)st->input(), (int)(pl * 1000), (int)(pr * 1000),
                         rs.recording ? 1 : 0, rs.withSong ? 1 : 0, rs.waiting ? 1 : 0, (unsigned)rs.fromBar,
                         (unsigned)rs.frames, (unsigned)rs.sampleRate,
                         (int)st->job(), (unsigned)st->jobProgress(), st->previewing() ? 1 : 0,
                         (unsigned)st->previewFrame(), (unsigned)ov.start, (unsigned)ov.end,
                         (unsigned)ov.frames, (unsigned)ov.lo.size(), (unsigned)ov.envelope.size(),
                         (unsigned)recs.size(), (unsigned)s_slices.size());
        n = std::min(n, (int)sizeof(reply) - kTail);
        for (size_t i = 0; i < s_slices.size() && n < (int)sizeof(reply) - 40; ++i) {
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%u", i ? "," : "", (unsigned)s_slices[i]);
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        const std::string result = st->jobResult();
        const crosspad::CardFolder cf = st->folder();
        n += snprintf(reply + n, sizeof(reply) - kTail - n, " n=%u folders=%u more=%d file=%s err=%s result=%s first=%s dir=%s names=",
                      (unsigned)cf.size(), (unsigned)cf.folders, cf.truncated ? 1 : 0,
                      rs.file.c_str(), rs.error.c_str(), result.c_str(), recs.empty() ? "-" : recs.front().c_str(),
                      cf.path.empty() ? "-" : cf.path.c_str());
        n = std::min(n, (int)sizeof(reply) - kTail);
        for (size_t i = 0; i < cf.size() && n < (int)sizeof(reply) - 24; ++i) {
            n += snprintf(reply + n, sizeof(reply) - kTail - n, "%s%s%s", i ? "|" : "", cf.name(i), cf.isFolder(i) ? "/" : "");
            n = std::min(n, (int)sizeof(reply) - kTail);
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_pad_stats(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "PAD_STATS", 9) == 0) {
        handle_pad_stats();
        return true;
    }
    return false;
}

bool verb_pad_press(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    int &b = l.b;
    if (sscanf(cmd, "PAD_PRESS %d %d", &a, &b) >= 1 && a >= 0 && a < 16) {
        const uint8_t vel = (b >= 0 && b <= 127) ? (uint8_t)b : 127;
        pad_inject_reply_first(HilPadInject::Press, (uint8_t)a, vel, cdc_reply);
        return true;
    }
    return false;
}

bool verb_pad_release(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "PAD_RELEASE %d", &a) == 1 && a >= 0 && a < 16) {
        pad_inject_reply_first(HilPadInject::Release, (uint8_t)a, 0, cdc_reply);
        return true;
    }
    return false;
}

bool verb_pad_pressure(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    int &b = l.b;
    if (sscanf(cmd, "PAD_PRESSURE %d %d", &a, &b) == 2 &&
        a >= 0 && a < 16 && b >= 0 && b <= 255) {
        pad_inject_reply_first(HilPadInject::Pressure, (uint8_t)a, (uint8_t)b, cdc_reply);
        return true;
    }
    return false;
}

bool verb_pad_pan(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    int &b = l.b;
    if (sscanf(cmd, "PAD_PAN %d %d", &a, &b) == 2 &&
        a >= 0 && a < 16 && b >= 0 && b <= 127) {
        /* Left/right balance of a held pad as the STM would stream it:
         * 0 left .. 64 centre .. 127 right, to the active pad logic. */
        pad_inject_reply_first(HilPadInject::Pan, (uint8_t)a, (uint8_t)b, cdc_reply);
        return true;
    }
    return false;
}

bool verb_kit_list(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "KIT_LIST", 8) == 0) {
        handle_kit_list();
        return true;
    }
    return false;
}

bool verb_kit_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "KIT_STATUS", 10) == 0) {
        handle_kit_status();
        return true;
    }
    return false;
}

bool verb_kit_swap_log(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "KIT_SWAP_LOG", 12) == 0) {
        handle_kit_swap_log();
        return true;
    }
    return false;
}

bool verb_wave_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "WAVE_STATUS", 11) == 0) {
        handle_wave_status();
        return true;
    }
    return false;
}

bool verb_smpl_pace(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (strncmp(cmd, "SMPL_PACE", 9) == 0) {
        /* SMPL_PACE [ms]: how long the kit loader and the other bulk readers
         * wait for a native streaming pass to end; 0 = they do not give way.
         * Until reboot; the A/B for it. */
        if (sscanf(cmd + 9, " %d", &a) == 1 && a >= 0 && a <= 1000) g_port_pace_ms.store((uint32_t)a);
        char out[32];
        snprintf(out, sizeof(out), "SMPLPACE: ms=%u\r\n", (unsigned)g_port_pace_ms.load());
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_smpl_stall(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_STALL", 10) == 0) {
        /* SMPL_STALL [ON|OFF]: the slowest native note-on and STM pad path,
         * and who had the CPU meanwhile. */
        EXT_RAM_BSS_ATTR static char out[320];
        const char *arg = cmd + 10;
        while (*arg == ' ') arg++;
        native_sampler_stall_report(out, sizeof(out),
                                    strncmp(arg, "ON", 2) == 0 ? 1 : strncmp(arg, "OFF", 3) == 0 ? 0 : -1);
        cdc_reply(out);
        return true;
    }
    return false;
}

bool verb_smpl_last(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_LAST", 9) == 0) {
        handle_smpl_last();
        return true;
    }
    return false;
}
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
bool verb_smpl_pool(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_POOL", 9) == 0) {
        /* No prebuilt streamer in this image; the native engine's pool is in
         * SMPL_ENGINE (pool_blocks=, norings=). */
        cdc_reply("ERR only_native\r\n");
        return true;
    }
    return false;
}
#endif
bool verb_kit_load(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "KIT_LOAD %d", &a) == 1 && a >= 0) {
        if (!g_kit_scan_done.load()) {
            /* getKitCount() below reads the vector the scan is growing, and the
             * id a caller quotes came from a listing that is not final yet. */
            cdc_reply("ERR scanning\r\n");
            return true;
        }
        if (!crosspad::control::kitIdLoadable(a)) {
            cdc_reply("ERR bad kit id\r\n");
            return true;
        }
        s_pending_kit = a;
        cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
        lv_async_call_locked(async_kit_load, nullptr);
        return true;
    }
    return false;
}

bool verb_kit_delete(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    /* KIT_DELETE id name -- a kit of the user's own (USER: the Slicer's)
     * off the card and out of the list, as the kit browser's "delete this
     * kit". The name must be the id's, so an id from an old listing never
     * takes another kit; a LEGACY kit and the one playing are refused. The
     * removal runs on the LVGL thread, as the browser's; KIT_LIST says when
     * it is gone. */
    {
        EXT_RAM_BSS_ATTR static char name[64];
        if (sscanf(cmd, "KIT_DELETE %d %63[^\r\n]", &a, name) == 2) {
            auto *mgr = crosspad::getKitManager();
            const crosspad::KitInfo *kit = (mgr && g_kit_scan_done.load() && a >= 0 && a < mgr->getKitCount())
                                               ? mgr->getKit(a) : nullptr;
            const char *err = nullptr;
            if (!kit || kit->path.empty())           err = "ERR bad kit id\r\n";
            else if (kit->name != name)              err = "ERR name\r\n";
            else if (!kit->isUser())                 err = "ERR not_user\r\n";
            else if (a == mgr->getCurrentKitId())    err = "ERR playing\r\n";
            if (err) { cdc_reply(err); return true; }
            s_delete_kit = a;
            cdc_reply("OK\r\n");
            lv_async_call_locked(async_kit_delete, nullptr);
            return true;
        }
    }
    return false;
}

bool verb_pad_info(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "PAD_INFO %d", &a) == 1 && a >= 0 && a < 16) {
        /* What the kit model holds for a pad: a wrong stored path vs an
         * engine that refused it. */
        auto *mgr = crosspad::getKitManager();
        auto *kit = mgr ? mgr->getCurrentKit() : nullptr;
        if (!kit) {
            cdc_reply("ERR no kit\r\n");
            return true;
        }
        auto &pad = kit->pads[a];
        char reply[512];
        int n = snprintf(reply, sizeof(reply),
                         "PADINFO: idx=%d num=%u kit=%s vol=%d pan=%d mode=%u dirty=%d count=%u",
                         a, (unsigned)pad.number, kit->path.c_str(), pad.volume, pad.pan,
                         (unsigned)pad.playbackMode, pad.dirty ? 1 : 0,
                         (unsigned)pad.sampleCount);
        for (uint8_t i = 0; i < pad.sampleCount && n < (int)sizeof(reply) - 80; ++i) {
            n += snprintf(reply + n, sizeof(reply) - n, " | [%u] '%s' %u..%u",
                          (unsigned)i, pad.samples[i].sample.c_str(),
                          (unsigned)pad.samples[i].start, (unsigned)pad.samples[i].end);
        }
        snprintf(reply + n, sizeof(reply) - n, "\r\n");
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_pad_mix(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    int &b = l.b;
    /* PAD_MIX: the current kit's sixteen volumes, "PADMIX: kit=… max=…
     * loading=… name=…", a row per pad, "PADMIX: end count=16". PAD_MIX pad
     * vol: that pad's volume, in the kit's units (0..127, a pitched kit
     * 0..100), into the kit and straight to the engine -- the Pad Mixer
     * app's fader, not saved until KIT_SAVE. */
    if (strncmp(cmd, "PAD_MIX", 7) == 0 && (cmd[7] == 0 || cmd[7] == ' ')) {
        auto *mgr = crosspad::getKitManager();
        if (!g_kit_scan_done.load()) { cdc_reply("ERR scanning\r\n"); return true; }
        crosspad::KitInfo *kit = mgr ? mgr->getCurrentKit() : nullptr;
        if (!kit) { cdc_reply("ERR no kit\r\n"); return true; }
        const int kitId = mgr->getCurrentKitId();
        const int max = kit->engine == crosspad::KitEngine::Pitched ? 100 : 127;
        if (cmd[7] == ' ') {
            if (sscanf(cmd + 8, "%d %d", &a, &b) != 2 || a < 0 || a >= KIT_PADS || b < 0 || b > max) {
                cdc_reply("ERR args\r\n");
                return true;
            }
            if (crosspad_sampler::kit_load_task_is_loading()) { cdc_reply("ERR loading\r\n"); return true; }
            if (s_mix_kit.exchange(kitId) != kitId) {
                for (auto &p : s_mix_pending) p.store(0);   /* parked for a kit that is gone */
            }
            s_mix_pending[a].store(b + 1);
            cdc_reply("OK\r\n");   /* reply first: the post wakes LVGL work that preempts this task */
            if (!s_mix_posted.exchange(true)) lv_async_call_locked(async_pad_mix, nullptr);
            return true;
        }
        EXT_RAM_BSS_ATTR static char line[192];
        snprintf(line, sizeof(line), "PADMIX: kit=%d max=%d loading=%d name=%s\r\n", kitId, max,
                 crosspad_sampler::kit_load_task_is_loading() ? 1 : 0, kit->name.c_str());
        cdc_reply(line);
        for (int i = 0; i < KIT_PADS; ++i) {
            const crosspad::Pad &pad = kit->pads[i];
            const int pending = s_mix_kit.load() == kitId ? s_mix_pending[i].load() : 0;
            const char *sample = pad.sampleCount && !pad.samples.empty() ? pad.samples[0].sample.c_str() : "-";
            const char *slash = strrchr(sample, '/');
            snprintf(line, sizeof(line), "PADMIX: pad=%d vol=%d pan=%d sample=%s\r\n", i,
                     pending ? pending - 1 : pad.volume, pad.pan, slash ? slash + 1 : sample);
            cdc_reply(line);
        }
        cdc_reply("PADMIX: end count=16\r\n");
        return true;
    }
    return false;
}

bool verb_kit_save_status(VerbLine &l)
{
    char *cmd = l.cmd;
    /* KIT_SAVE: the current kit, pads and all, into its kit.json -- the
     * Sampler's save, on the LVGL thread. OK = queued; KIT_SAVE_STATUS says
     * "KITSAVE: busy=0|1 ok=-1|0|1 kit=N" once it is written. */
    if (strcmp(cmd, "KIT_SAVE_STATUS") == 0) {
        char reply[64];
        snprintf(reply, sizeof(reply), "KITSAVE: busy=%d ok=%d kit=%d\r\n", s_kit_save_busy.load() ? 1 : 0,
                 s_kit_save_ok.load(), s_kit_save_kit.load());
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_kit_save(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "KIT_SAVE") == 0) {
        auto *mgr = crosspad::getKitManager();
        if (!g_kit_scan_done.load()) { cdc_reply("ERR scanning\r\n"); return true; }
        if (!mgr || !mgr->getCurrentKit()) { cdc_reply("ERR no kit\r\n"); return true; }
        if (crosspad_sampler::kit_load_task_is_loading()) { cdc_reply("ERR loading\r\n"); return true; }
        if (s_kit_save_busy.exchange(true)) { cdc_reply("ERR busy\r\n"); return true; }
        s_kit_save_kit.store(mgr->getCurrentKitId());
        s_kit_save_ok.store(-1);
        cdc_reply("OK\r\n");
        lv_async_call_locked(async_kit_save, nullptr);
        return true;
    }
    return false;
}

bool verb_arena(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "ARENA", 5) == 0) {
        /* The two sample engines share one PSRAM block, so MEM's PSRAM figures
         * no longer say who holds what — everything inside the arena looks
         * allocated to the general heap whichever engine is up. This is the
         * only view of it. */
        /* native: the engine holds a kit, or (without the prebuilt streamer)
         * the arena is simply its own and empty until the next take. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        const char *owner = marcel_engine_running() ? "marcel"
                          : crosspad::pitchedKitActive() ? "pitched"
                          : native_sampler_active() ? "native" : "none";
#else
        const char *owner = crosspad::pitchedKitActive() ? "pitched" : "native";
#endif
        char reply[80];
        snprintf(reply, sizeof(reply), "ARENA: cap_kb=%u used_kb=%u owner=%s\r\n",
                 (unsigned)(engine_arena_capacity() / 1024),
                 (unsigned)(engine_arena_used() / 1024), owner);
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_inst_load(VerbLine &l)
{
    char *cmd = l.cmd;
    /* The instrument host (instrument_host.h), driven without Pitch Play's
     * screen: INST_LOAD <logical wav> builds a one-zone instrument, as picking
     * a sample from its list does; INST_STATUS says where the host is and how
     * long its last request took; INST_RELEASE hands the engine back. */
    if (strncmp(cmd, "INST_LOAD ", 10) == 0) {
        auto* h = crosspad::getPlatformServices().instrumentHost;
        /* On the heap: a KitInfo is over a kilobyte, and this frame is under every verb. */
        std::unique_ptr<crosspad::KitInfo> spec(new (std::nothrow) crosspad::KitInfo());
        if (!spec) { cdc_reply("INST: ERR no_mem\r\n"); return true; }
        spec->name = "hil";
        spec->engine = crosspad::KitEngine::Pitched;
        crosspad::InstrumentZoneSpec z;
        z.sample = cmd + 10;
        spec->instrument.zones.push_back(z);
        cdc_reply(h && h->requestLoad(*spec) ? "INST: OK\r\n" : "INST: ERR no_host\r\n");
        return true;
    }
    return false;
}

bool verb_inst_release(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "INST_RELEASE", 12) == 0) {
        auto* h = crosspad::getPlatformServices().instrumentHost;
        if (h) h->release();
        cdc_reply(h ? "INST: OK\r\n" : "INST: ERR no_host\r\n");
        return true;
    }
    return false;
}

bool verb_inst_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "INST_STATUS", 11) == 0) {
        auto* h = crosspad::getPlatformServices().instrumentHost;
        if (!h) { cdc_reply("INST: ERR no_host\r\n"); return true; }
        static const char* kStates[] = {"idle", "busy", "ready", "failed"};
        std::string msg = h->message();
        for (auto& c : msg) if (c == ' ') c = '_';
        EXT_RAM_BSS_ATTR static char reply[192];
        snprintf(reply, sizeof(reply), "INST: state=%s owned=%d beside=%d last_ms=%u msg=%s\r\n",
                 kStates[(int)h->state()], instrument_host_owns_engine() ? 1 : 0,
                 instrument_host_beside() ? 1 : 0,
                 (unsigned)instrument_host_last_ms(), msg.empty() ? "-" : msg.c_str());
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_pitched_status(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "PITCHED_STATUS", 14) == 0) {
        /* rt_max_us resets on read, like SMPL_PEAK's — a scenario samples it
         * per phase and wants the worst block of that phase, not of the boot.
         * Static, as the reply: the CDC loop's stack is not the place for the
         * status and its roots. */
        EXT_RAM_BSS_ATTR static crosspad::PitchedKitStatus st;
        crosspad::pitchedKitGetStatus(st);
        /* voices= is what is in force now -- the editor re-applies it -- cap=
         * the board's ceiling, shed= the steals an overloaded render forced
         * below it. The error text is already free of spaces (PitchedKitPort
         * sanitises it), so it stays one field. */
        const HilPitchedBoard board{
            crosspad::pitchedSampleRtMaxUs().exchange(0), st.lastError,
            audio_rt_busy_max(AUDIO_RT_BUSY_PITCHED, nullptr), g_rt_overruns.load(), st.kitMaxVoices};
        EXT_RAM_BSS_ATTR static char reply[kHilPitchedLineChars];
        hil_text_pitched_status(reply, sizeof(reply), st, board);
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_pitched_cap(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "PITCHED_CAP %d", &a) == 1 && a >= 1 && a <= 28) {
        /* Bench: the pitched voice cap (PitchedSampleNode::kVoiceCap), from
         * the next setParams on -- a load, or an edit in the kit editor.
         * Until the next restart. */
        if (auto *inst = crosspad::getPlatformServices().pitchedInstrument) inst->setVoiceCap((uint8_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}

bool verb_pitched_bench(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "PITCHED_BENCH", 13) == 0) {
        int iters = kBenchDefaultIters;
        sscanf(cmd, "PITCHED_BENCH %d", &iters);
        if (iters < kBenchMinIters) iters = kBenchMinIters;
        if (iters > kBenchMaxIters) iters = kBenchMaxIters;
        if (s_bench_busy) { cdc_reply("BENCH: ERR busy\r\n"); return true; }
        s_bench_iters = iters;
        s_bench_busy = true;
        /* Stack from PSRAM: 8 kB of internal RAM is more than the largest
         * free internal block once a kit is loaded, and this task never runs
         * with the flash cache disabled. */
        if (xTaskCreatePinnedToCoreWithCaps(pitched_bench_task, "pitched_bench", 8192,
                                            nullptr, 1, nullptr, 0,
                                            MALLOC_CAP_SPIRAM) != pdPASS) {
            s_bench_busy = false;
            cdc_reply("BENCH: ERR no task\r\n");
        }
        return true;
    }
    return false;
}
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
bool verb_smpl_poly(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "SMPL_POLY %d", &a) == 1 && a >= 0 && a <= 17 && marcel_engine_running()) {
        /* The prebuilt streamer holds the kit: its voice cap
         * (SampleStreamPlayer_SetMaxPolyphony()). Bench knob: it holds over
         * the port's cap of 16, across kit loads, until SMPL_POLY 0. Above
         * the native engine's SMPL_POLY, which would match first. */
        sample_player_port_set_poly_cap((uint8_t)a);
        cdc_reply("OK\r\n");
        return true;
    }
    return false;
}
#endif
bool verb_smpl_poly_2(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_POLY", 9) == 0) {
        /* The voice cap, so a polyphony sweep can see past the default 16 --
         * the engine's own ceiling is higher. `SMPL_POLY` alone reports it. */
        /* `SMPL_POLY <total> [streaming]` -- the second is the ceiling on
         * voices being fed off the card, which is the one that limits long
         * samples and has to be movable to find out why. */
        int tot = 0, strm = 0;
        char reply[80];
        const int n = sscanf(cmd + 9, "%d %d", &tot, &strm);
        if (n >= 1) {
            /* `SMPL_POLY 0`: both back to the engine's defaults (16/10), the
             * way 0 meant "back to the kit's budget" on the prebuilt one. A
             * take resets them too: a raise is for the kit it was made on. */
            /* The streaming budget first: it takes the loader's lock, and a
             * take holding it answers busy before anything has changed. */
            const uint8_t gotS = (n >= 2)   ? native_sampler_set_max_streaming((uint8_t)strm)
                                 : tot == 0 ? native_sampler_set_max_streaming(0)
                                            : native_sampler_max_streaming();
            if (gotS == NATIVE_SAMPLER_BUSY) { cdc_reply("ERR busy\r\n"); return true; }
            const uint8_t gotT = native_sampler_set_max_voices((uint8_t)tot);
            snprintf(reply, sizeof(reply), "SMPLPOLY: max_voices=%u max_streaming=%u\r\n",
                     (unsigned)gotT, (unsigned)gotS);
        } else {
            snprintf(reply, sizeof(reply), "SMPLPOLY: max_voices=%u max_streaming=%u\r\n",
                     (unsigned)native_sampler_max_voices(),
                     (unsigned)native_sampler_max_streaming());
        }
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_smpl_peak(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_PEAK", 9) == 0) {
        /* Peak rendered since the last read (int16 scale) plus free WAV
         * slots: a silent engine vs a bus that carried nothing out. The louder
         * of the two sample engines, so a pitched kit — where the prebuilt
         * engine is stopped and its node renders nothing — is not reported as silence.
         * Both are read-and-cleared, whichever wins. */
        int32_t nativePeak = 0;
        uint32_t nativeRtMaxUs = 0, nativeUnderruns = 0;
        native_sampler_peak_take(&nativePeak, &nativeRtMaxUs, &nativeUnderruns);
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        const int32_t marcelPeak = crosspad::marcelSamplePeak().exchange(0);
        const uint32_t marcelRtMaxUs = crosspad::marcelSampleRtMaxUs().exchange(0);
#else
        const int32_t marcelPeak = 0;
        const uint32_t marcelRtMaxUs = 0;
#endif
        const int32_t pitchedPeak = crosspad::pitchedSamplePeak().exchange(0);
        int32_t peak = marcelPeak > pitchedPeak ? marcelPeak : pitchedPeak;
        if (nativePeak > peak) peak = nativePeak;
        /* free=/poly=/rt_max_us= of the engine that holds the kit: the native
         * one when it does (poly= its voice cap), else the prebuilt streamer. */
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        const bool nat = native_sampler_active();
        const unsigned poly = nat ? native_sampler_max_voices() : marcel_engine_poly();
#else
        const bool nat = true;
        const unsigned poly = native_sampler_max_voices();
#endif
        const unsigned rtMax = nat ? nativeRtMaxUs : marcelRtMaxUs;
        const unsigned underruns = nat ? nativeUnderruns : g_engine_underrun.load();
        /* The library's GetLoad and GetMaxc accessors are not reported: they
         * read nothing useful here (`nan`), and on an engine
         * marcel_engine_stop() has stopped -- a board with no kit loaded or
         * on a pitched kit -- they read a null pointer.
         *
         * What is on this line is measured by the port: peak,
         * free slots, the voice cap, rt_max_us (MarcelSampleNode times
         * SampleStreamPly_Process alone), rxskip, noblock and underrun. */
        /* rt_busy_max_us: the whole render pass, I2S waits excluded (read and
         * cleared); overruns: passes longer than a block, since boot (main). */
        char reply[256];
        /* `unpaced` is the one that matters when a board "glitches": a render
         * pass whose block no output took means nothing was slowing the RT
         * loop down, and it then runs as fast as the core allows. Anything
         * but 0 is a fault, and it is read here because this verb is one of
         * the few the USB-audio profile can answer at all. */
        snprintf(reply, sizeof(reply), "SMPLPEAK: %d free=%d poly=%u rt_max_us=%u rxskip=%u noblock=%u underrun=%u unpaced=%u"
                 " rt_busy_max_us=%u overruns=%u\r\n",
                 (int)peak, hil_free_slots(), poly, rtMax,
                 (unsigned)audio_rx_skips(),
                 (unsigned)g_engine_noblock.load(), underruns,
                 (unsigned)audio_rt_unpaced_passes(),
                 (unsigned)audio_rt_busy_max(AUDIO_RT_BUSY_SMPL_PEAK, nullptr), (unsigned)g_rt_overruns.load());
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_smpl_pitch(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    int &b = l.b;
    if (sscanf(cmd, "SMPL_PITCH %d %d", &a, &b) == 2 &&
        a >= 0 && a < 16 && b >= -24 && b <= 24) {
        /* A pad's step, from the bench, through the same door the pad editor
         * uses: write the kit model and ask for that one pad back.
         *
         * It exists because a realtime step cannot be heard over a serial
         * cable and no kit on a card sets `pitch` at all -- so without it the
         * one feature this engine was built for has no way of being shown to
         * work on real hardware. On the prebuilt engine it does what it always
         * did: renders a transposed copy and reloads, which is the comparison. */
        crosspad::KitInfo* kit = crosspad::getKitManager()
                                     ? crosspad::getKitManager()->getCurrentKit() : nullptr;
        if (!kit || !kit->parsed) { cdc_reply("ERR no_kit\r\n"); return true; }
        kit->pads[a].pitch = b;
        kit->pads[a].dirty = true;
        /* The editor's own path: one number where the engine can take it, a
         * pad rebuild only where it cannot. Measuring anything else would
         * measure a route the instrument does not use. */
        const bool stepped = native_sampler_set_pad_pitch((uint8_t)a, b);
        char reply[64];
        snprintf(reply, sizeof(reply), "SMPLPITCH: pad=%d semi=%d engine=%s\r\n",
                 a, b, native_sampler_active() ? "native"
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
                       : "marcel");
#else
                       : "none");
#endif
        cdc_reply(reply);   /* reply first, then the hand-off */
        if (!stepped) {
            /* The rebuild is the kit loader's (the pad is marked modified
             * above), never this task's: a pad reload reads the card, may
             * render a copy, and on a pitched kit builds the whole bank.
             * KIT_STATUS / SMPL_ENGINE say when it is done. */
            lv_async_call_locked([](void*) { crosspad_sampler::kit_load_task_start(true, -1); }, nullptr);
        }
        return true;
    }
    return false;
}

bool verb_smpl_loops(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strcmp(cmd, "SMPL_LOOPS") == 0) {
        static char reply[96];
        native_sampler_loops(reply, sizeof(reply));
        cdc_reply(reply);
        return true;
    }
    return false;
}

bool verb_smpl_engine(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_ENGINE", 11) == 0) {
        /* Which of the two sample engines holds the kit. They share the PSRAM
         * arena, so this is a handover, not a fan-out -- and it is a runtime
         * choice so the same kit can be heard through both on one board.
         *
         * Reloads the kit on the way in: the native engine reads every
         * sample's header and its resident head off the card, which is the
         * same work a kit load does and takes about as long. */
        const char *arg = cmd + 11;
        while (*arg == ' ') ++arg;
        if (*arg == '\0' || *arg == '\r' || *arg == '\n') {
            /* Static, and room to spare: ~440 B on a busy kit, ~500 at the
             * counters' widest, and a truncated line loses its CRLF, which the
             * host reads as no reply at all. The main task's 4 kB stack is no
             * place for it. */
            EXT_RAM_BSS_ATTR static char reply[640];
            native_sampler_status(reply, sizeof(reply));
            cdc_reply(reply);
            return true;
        }
        if (strncmp(arg, "native", 6) == 0) {
            native_sampler_set_preferred(true);
            /* Answers at once; the take runs on its own task because it reads
             * the whole kit off the card. SMPL_ENGINE with no argument says
             * where it got to. */
            if (!native_sampler_take_kit()) { cdc_reply("ERR no_kit\r\n"); return true; }
            cdc_reply("OK loading\r\n");
            return true;
        }
        if (strncmp(arg, "marcel", 6) == 0) {
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
            /* And stop preferring it, or the next kit load takes it straight
             * back and the choice looks like it did not stick. */
            native_sampler_set_preferred(false);
            native_sampler_release();
            if (!marcel_engine_running()) marcel_engine_start();
            cdc_reply("OK marcel\r\n");
#else
            /* Not built (Kconfig CROSSPAD_SAMPLER_MARCEL): the native engine
             * is the only one; SMPL_ENGINE's engines= says so too. */
            cdc_reply("ERR only_native\r\n");
#endif
            return true;
        }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        cdc_reply("ERR usage SMPL_ENGINE [native|marcel]\r\n");
#else
        cdc_reply("ERR usage SMPL_ENGINE [native]\r\n");
#endif
        return true;
    }
    return false;
}
#if !CONFIG_CROSSPAD_SAMPLER_MARCEL
bool verb_smpl_bkg(VerbLine &l)
{
    char *cmd = l.cmd;
    int &a = l.a;
    if (sscanf(cmd, "SMPL_BKG %d", &a) == 1 && (a == 0 || a == 1)) {
        /* No prebuilt streamer in this image. */
        cdc_reply("ERR only_native\r\n");
        return true;
    }
    return false;
}
#endif
bool verb_smpl_preview(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_PREVIEW ", 13) == 0) {
        /* The preview voice directly: a silent preview vs a UI path that
         * never fires. */
        char clean[128];
        strncpy(clean, cmd + 13, sizeof(clean) - 1);
        clean[sizeof(clean) - 1] = '\0';
        for (char *q = clean; *q; ++q) {
            if (*q == '\r' || *q == '\n') { *q = '\0'; break; }
        }
        char *path = clean[0] == '/' ? clean + 1 : clean;
        /* The native engine's audition voice (the file browser's path), on
         * the Preview channel; SMPL_PREVIEW_STOP ends it. Started on the LVGL
         * thread, the browser's: the engine's request is a sequence-locked
         * record with one writer, and this verb was a second one. */
        if (native_sampler_active()) {
            char* fs = static_cast<char*>(malloc(160));
            if (!fs) { cdc_reply("ERR no_mem\r\n"); return true; }
            snprintf(fs, 160, "/sdcard/%s", path);
            cdc_reply("OK\r\n");   /* reply first */
            lv_async_call_locked([](void* p) {
                char* f = static_cast<char*>(p);
                if (!native_sampler_preview(f, 0, 0, 100, 0)) ESP_LOGW(TAG, "SMPL_PREVIEW %s refused", f);
                free(f);
            }, fs);
            return true;
        }
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
        if (!marcel_engine_running()) { cdc_reply("ERR stopped\r\n"); return true; }
        SampleStreamPlayer_Preview(path);
        cdc_reply("OK\r\n");
#else
        /* The native engine holds no kit (a pitched bank has the arena). */
        cdc_reply("ERR no_kit\r\n");
#endif
        return true;
    }
    return false;
}

bool verb_smpl_preview_stop(VerbLine &l)
{
    char *cmd = l.cmd;
    if (strncmp(cmd, "SMPL_PREVIEW_STOP", 17) == 0) {
        /* Through the same owner as the start, so a start and a stop sent in
         * that order happen in that order. */
        cdc_reply("OK\r\n");   /* reply first */
        lv_async_call_locked([](void*) { native_sampler_preview_stop(); }, nullptr);
        return true;
    }
    return false;
}
