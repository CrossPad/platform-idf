// SPDX-License-Identifier: GPL-3.0-or-later
//
// This board's side of the song engine (crosspad/song/SongEngine): its worker
// is a FreeRTOS task on core 0, its rings live in the engine arena's slack or
// in PSRAM above the UI's reserve, its projects on the SD card, its current
// project in NVS, and a bounce records the "USB rec" bus.

#include "CrosspadPlatform.hpp"
#include "SongEnginePort.hpp"
#include "SamplerEnginePort.hpp"
#include "engine_arena.hpp"
#include "PitchedKitPort.hpp"
#include "PitchedSampleNode.hpp"
#include "IdfAudioModule.hpp"

#include <crosspad-mixer/AudioMixerEngine.hpp>
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/sequencer/MetronomeNode.hpp"
#include "crosspad/sequencer/PatternSequencer.hpp"
#include "crosspad/song/SongEngine.hpp"

#include "bsp_sdmmc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "miniz.h"      // the ROM's inflate
#include "nvs.h"

#include <atomic>
#include <cstring>
#include <string>
#include <unistd.h>

/* Defined in main (sample_player_port.cpp). Makes a bulk card reader or
 * writer give way to the streamer while pads are sounding. */
extern void sample_player_port_yield_to_streamer(uint32_t weight);
/* The board's one 4 kB DMA-capable stage, a piece at a time (NULL: none);
 * urgent, the streamer steps aside while this waits. */
extern uint8_t* sample_player_port_stage_take(bool urgent);
extern void sample_player_port_stage_give(void);
extern bool sample_player_port_direct_io(void);

/* crosspad-core's data/live12_set.xml.gz, embedded by CMakeLists.txt. */
extern const uint8_t live12_set_gz_start[] asm("_binary_live12_set_xml_gz_start");
extern const uint8_t live12_set_gz_end[] asm("_binary_live12_set_xml_gz_end");

/* The clock and the loop recorder, defined in CrosspadPlatform.cpp. */
crosspad::MetronomeNode& getMetronome();
crosspad::PatternSequencer& getSequencer();

extern "C" bool audio_rt_offline(bool on, uint32_t timeout_ms);   // main/audio/audio_task.cpp
extern "C" bool audio_rt_live_recently(void);                     // main/audio/audio_task.cpp
extern "C" bool audio_rt_pressed_recently(void);                  // main/audio/audio_task.cpp

namespace crosspad {
namespace {

constexpr const char* TAG = "SongEngine";
constexpr const char* kRoot = "/sdcard/SONGS";
constexpr const char* kCardMount = "/sdcard";
constexpr const char* kNvsNs = "song";
constexpr const char* kNvsCurrent = "current";
/// A project name, as the engine accepts one.
constexpr size_t kMaxNameLen = 32;
/// Internal RAM an open needs: newlib's lock and the SDMMC DMA buffer.
constexpr size_t kMinInternalForOpen = 2048;
/// PSRAM the UI keeps whatever a ring would like: LVGL aborts on a failed
/// allocation, a ring only gets shorter. The launcher runs in well under
/// 150 kB with a kit loaded, and an open app's next kit list or image decode
/// needs its share of that.
constexpr size_t kUiReserveBytes = 128 * 1024;
/// What a ring segment past its minimum leaves free: a bounce asks for 1 MB
/// and takes what is above this, the floor the sample engines keep too
/// (engine_arena.cpp).
constexpr size_t kOptionalReserveBytes = 512 * 1024;
constexpr uint32_t kWorkerStack = 8192;
/// Above LVGL (4) on its core: a busy screen -- the Sequencer's grid redraws
/// nearly back to back while a scene plays -- starved the worker at 3 until
/// the bounce ring overflowed. It spends its time waiting on the card, so the
/// UI loses next to nothing.
constexpr UBaseType_t kWorkerPriority = 5;
constexpr BaseType_t kWorkerCore = 0;
/// The board's one DMA-capable stage (sample_player_port_stage_*): a bounce
/// piece is one stage, one CMD25.
constexpr uint32_t kStageBytes = 4096;
constexpr uint32_t kBytesPerFrame = 4;   // stereo int16
/// Up to 1 MB, ~5.9 s of card stall. A card stalls a write for ~0.5 s now
/// and then, and the writes right after one are slow too: with 128 kB
/// (0.74 s) a 2-minute bounce failed "card too slow" at 33-77 % in three runs
/// out of four (song stress test, 2026-10-18), while the card averaged ~3x
/// the 176 kB/s a bounce needs. Taken only for the bounce, and past the
/// ring's minimum only above the engines' floor (allocRingSegment()).
constexpr uint32_t kBounceRingFrames = 262144;
/* One ring for all the tracks: four rings were 128 kB of PSRAM, more than
 * the board has free with an app open, and play simply did not start.
 *
 * ~1.5 s ahead, not the ~370 ms it was: with eight tracks and fingers on the
 * pads one read of the card took up to 860 ms behind the sampler's streamer
 * and a kit load, and a kit loaded, a block copied or a take punched in under
 * playback emptied it (hil_track_transitions.py, 2026-10-27). ~370 ms was
 * enough for 08.10.2026's UI moves, ~170 was not.
 * The ring takes what memory gives down to the prime, so a board short of
 * PSRAM plays as it did; the prime, and with it the wait before the first
 * note, is the same 4096 frames as before. */
constexpr uint32_t kSongRingFrames = 65536;   // 256 kB, ~1.5 s ahead

class BoardSongPlatform final : public ISongEnginePlatform {
public:
    BoardSongPlatform() {
        wake_ = xSemaphoreCreateBinary();
    }

    bool startWorker(void (*entry)(void*), void* arg) override {
        /* PSRAM stack: card and memory work only, never a flash write.
         * Long-lived, so its stack is created once. */
        return xTaskCreatePinnedToCoreWithCaps(entry, "song_io", kWorkerStack, arg, kWorkerPriority,
                                               nullptr, kWorkerCore,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
    }
    void joinWorker() override {}   // the engine lives as long as the board runs
    void wake() override { xSemaphoreGive(wake_); }
    void waitWake(uint32_t ms) override {
        xSemaphoreTake(wake_, ms == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(ms));
    }
    void sleepMs(uint32_t ms) override { vTaskDelay(ms ? pdMS_TO_TICKS(ms) : 1); }
    int64_t nowUs() override { return esp_timer_get_time(); }
    SongEngineSizes sizes() override {
        SongEngineSizes s;
        s.bounceRingFrames = kBounceRingFrames;
        s.songRingFrames = kSongRingFrames;
        s.chunkFrames = kStageBytes / kBytesPerFrame;
        return s;
    }

    void* allocRingSegment(size_t bytes, uint32_t missingSegs) override {
        /* The engine arena's slack first (engine_arena_lend_ring(): past what
         * a kits' engine may still claim, or beside a pitched bank with room
         * left for the next one), given back before every reset by
         * song_engine_release_arena(). Then the PSRAM heap, above the UI's
         * reserve -- or, for a segment past the ring's minimum, above the
         * engines' floor. */
        if (void* p = engine_arena_lend_ring(bytes)) return p;
        const size_t need = missingSegs ? missingSegs * bytes + kUiReserveBytes : bytes + kOptionalReserveBytes;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < need) {
            return nullptr;
        }
        return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    }
    void freeRingSegment(void* p) override { engine_arena_free(p); }
    void* allocScratch(size_t bytes) override { return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM); }
    size_t freeMemoryBytes() override { return heap_caps_get_free_size(MALLOC_CAP_SPIRAM); }

    bool storageReady() override {
        /* An fopen that cannot abort: a failed allocation in internal RAM is
         * an abort, not NULL. */
        return bsp_sdcard_get_handle() != nullptr &&
               heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >= kMinInternalForOpen;
    }
    /* Worker only: f_getfree walks the FAT. */
    uint64_t freeStorageBytes() override {
        uint64_t total = 0, free = 0;
        return esp_vfs_fat_info(kCardMount, &total, &free) == ESP_OK ? free : UINT64_MAX;
    }
    void yieldIo() override { sample_player_port_yield_to_streamer(1); }
    /* A piece of the take, from the engine's buffer in PSRAM, which the S3's
     * SDMMC cannot DMA from: through stdio every sector was its own CMD24
     * through a 512 B bounce, plus a read of the sector a piece ended in.
     * Copied to the stage and handed to the descriptor, a 4 kB piece that
     * ends on 4 kB of the file is one CMD25. fflush() first, outside the
     * stage: whatever the header left in the stream goes before it, and
     * nothing waits for the stage holding the FILE's lock. A failed flush is
     * the card's error, not a missing stage: the write fails, uncounted.
     * Urgent: the take's ring is draining while this waits. */
    bool writeIo(FILE* f, const void* data, size_t bytes) override {
        if (bytes <= kStageBytes) {
            if (std::fflush(f) != 0) return false;
            /* The sector layer bounces: the piece goes from the ring as it is. */
            if (sample_player_port_direct_io()) return write(__sfileno(f), data, bytes) == (ssize_t)bytes;
            if (uint8_t* stage = sample_player_port_stage_take(true)) {
                std::memcpy(stage, data, bytes);
                const bool ok = write(__sfileno(f), stage, bytes) == (ssize_t)bytes;
                sample_player_port_stage_give();
                return ok;
            }
        }
        ioFallbacks_.fetch_add(1, std::memory_order_relaxed);
        return std::fwrite(data, 1, bytes, f) == bytes;
    }
    uint32_t ioFallbacks() const { return ioFallbacks_.load(std::memory_order_relaxed); }
    const char* songsRoot() override { return kRoot; }

    std::string loadCurrent() override {
        char name[kMaxNameLen + 1] = "";
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READONLY, &h) == ESP_OK) {
            size_t len = sizeof(name);
            if (nvs_get_str(h, kNvsCurrent, name, &len) != ESP_OK) name[0] = '\0';
            nvs_close(h);
        }
        return name;
    }
    void saveCurrent(const std::string& name) override {
        nvs_handle_t h;
        if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
        nvs_set_str(h, kNvsCurrent, name.c_str());
        nvs_commit(h);
        nvs_close(h);
    }

    /* Inflated straight into the string with the ROM's tinfl, its state on
     * the heap: it is ~11 kB and the worker's stack is 8. Only with the UI's
     * reserve left over, as for a ring -- a string that cannot be had is an
     * abort, not an empty export. */
    std::string liveSetTemplate() override {
        const uint8_t* gz = live12_set_gz_start;
        const size_t n = (size_t)(live12_set_gz_end - live12_set_gz_start);
        /* One plain member: no name or extra field, then raw deflate, CRC32
         * and the length (make_live12_set.py writes it so). */
        if (n < 18 || gz[0] != 0x1f || gz[1] != 0x8b || gz[2] != 8 || gz[3] != 0) return {};
        const size_t size = gz[n - 4] | gz[n - 3] << 8 | gz[n - 2] << 16 | (size_t)gz[n - 1] << 24;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < size + sizeof(tinfl_decompressor) + kUiReserveBytes ||
            heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size + 1) {
            return {};
        }
        auto* d = static_cast<tinfl_decompressor*>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
        if (!d) return {};
        std::string xml(size, '\0');
        tinfl_init(d);
        size_t in = n - 18, out = size;
        auto* dst = reinterpret_cast<uint8_t*>(&xml[0]);
        const tinfl_status st = tinfl_decompress(d, gz + 10, &in, dst, dst, &out,
                                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        heap_caps_free(d);
        if (st != TINFL_STATUS_DONE || out != size) return {};
        return xml;
    }

    /* A bounce records the pads tap (IdfAudioModule::setPadChannels), not the
     * USB rec bus: its level, mute and sends are the stream's, and nothing on
     * the mixer has to change for a take. */
    void beginPadsCapture() override {}
    /* A freeze: its engine's channel alone on the tap, so fingers on the
     * armed track's engine stay out of the take -- and on no output, so a
     * freeze that runs in real time (someone listening, a host streaming)
     * is never heard. */
    void beginRouteCapture(uint8_t route) override {
        if (!tap_) return;
        const uint8_t ch = route == 2 ? tapCh_[1] : tapCh_[0];
        tap_->setPadChannels(ch, 0xFF, 0xFF);
        AudioMixerEngine* m = tap_->getMixerEngine();
        if (m && ch != 0xFF) {
            m->setChannelTapOnly(ch, true);
            tapOnlyCh_ = ch;
        }
    }
    void endPadsCapture() override {
        if (!tap_) return;
        tap_->setPadChannels(tapCh_[0], tapCh_[1], tapCh_[2]);
        if (tapOnlyCh_ != 0xFF) {
            if (AudioMixerEngine* m = tap_->getMixerEngine()) m->setChannelTapOnly(tapOnlyCh_, false);
            tapOnlyCh_ = 0xFF;
        }
    }
    bool padsPlayedRecently() override { return audio_rt_pressed_recently(); }
    /* Off the clock for a freeze: audio_task's offline render (the outputs
     * fade out and back in around it). */
    bool beginOffline() override {
        /* Not while someone listens: off the clock the outputs are silent. */
        if (!offlineAllowed_.load() || audio_rt_live_recently()) return false;
        sequencer_poll_from_render(true);         // notes on the render's blocks, not the task's 2 ms
        if (audio_rt_offline(true, 500)) return true;
        sequencer_poll_from_render(false);
        return false;
    }
    void endOffline() override {
        audio_rt_offline(false, 500);
        sequencer_poll_from_render(false);
    }
    void setPadsTap(IdfAudioModule& m, uint8_t kit, uint8_t pitched, uint8_t kitB) {
        tap_ = &m;
        tapCh_[0] = kit;
        tapCh_[1] = pitched;
        tapCh_[2] = kitB;
    }

    /* A trim on the engines' channels, beside the Mixer's faders: the song's
     * fader for its seq track, gone again when it stops. */
    void setSeqGain(SeqSource source, float gain) override {
        if (!mixer_) return;
        const uint8_t* chs = source == SeqSource::Pitch ? pitchCh_ : kitCh_;
        for (int i = 0; i < 2; ++i) {
            if (chs[i] != kMixerInvalidChannelId) mixer_->setChannelTrim(chs[i], gain);
        }
    }
    /* A seq track's pan the same way: a trim pan on its engine's channels,
     * on top of the Mixer's pan and never saved with the desk. */
    void setSeqPan(SeqSource source, float pan) override {
        if (!mixer_) return;
        const uint8_t* chs = source == SeqSource::Pitch ? pitchCh_ : kitCh_;
        for (int i = 0; i < 2; ++i) {
            if (chs[i] != kMixerInvalidChannelId) mixer_->setChannelTrimPan(chs[i], pan);
        }
    }
    /* Each engine's own peak, as its node saw it: the pads tap is both. */
    bool seqLevel(uint8_t route, float& left, float& right) override {
        if (route == 0) return native_sampler_level_take(&left, &right);
        if (route == 2) {
            left = right = (float)pitchedMeterPeak().exchange(0, std::memory_order_relaxed) / 32768.0f;
            return true;
        }
        return false;
    }
    void setSeqChannels(AudioMixerEngine& mixer, uint8_t kitA, uint8_t kitB, uint8_t pitched) {
        mixer_ = &mixer;
        kitCh_[0] = kitA;
        kitCh_[1] = kitB;
        pitchCh_[0] = pitched;
    }

    void songOpened(const AudioSong& song) override {
        if (openHandler_) openHandler_(song);
    }
    void setOpenHandler(void (*fn)(const AudioSong&)) { openHandler_ = fn; }

    void log(bool warning, const char* msg) override {
        if (warning) ESP_LOGW(TAG, "%s", msg);
        else         ESP_LOGI(TAG, "%s", msg);
    }

private:
    SemaphoreHandle_t wake_ = nullptr;
    AudioMixerEngine* mixer_ = nullptr;
    void (*openHandler_)(const AudioSong&) = nullptr;
    uint8_t kitCh_[2] = {kMixerInvalidChannelId, kMixerInvalidChannelId};
    uint8_t pitchCh_[2] = {kMixerInvalidChannelId, kMixerInvalidChannelId};
    uint8_t tapOnlyCh_ = 0xFF;                  ///< the freeze's engine, off the outputs while it records
    IdfAudioModule* tap_ = nullptr;
public:
    std::atomic<bool> offlineAllowed_{true};    ///< ASONG OFFLINE: off, a freeze runs in real time (A/B)
private:
    uint8_t tapCh_[3] = {0xFF, 0xFF, 0xFF};   // the pads tap's channels: kit, pitched, the kit's second engine
    std::atomic<uint32_t> ioFallbacks_{0};   // pieces written through stdio: no stage (ASONG, CDC task)
};

SongEngine* s_engine = nullptr;
BoardSongPlatform* s_platform = nullptr;

} // namespace

void song_engine_init(AudioMixerEngine& mixer, uint8_t kitA, uint8_t kitB, uint8_t pitched)
{
    static BoardSongPlatform platform;
    s_platform = &platform;
    platform.setSeqChannels(mixer, kitA, kitB, pitched);
    static SongEngine engine(platform, getMetronome(), getSequencer());
    s_engine = &engine;
    /* Second line for the rings: every handoff releases them by name before it
     * resets the arena, and this catches a reset path that forgets to. A ring
     * left in a wiped arena is written by RT into another owner's memory, then
     * freed as a foreign pointer. Idempotent: no rings held, nothing waited. */
    engine_arena_set_reclaim([](void*) { song_engine_release_arena(); }, nullptr);
    const auto ch = mixer.addChannel(&engine, "Song");
    if (ch != kMixerInvalidChannelId) {
        /* Speakers and phones. Not the USB rec bus: what a stream records is
         * the Mixer's to decide. A bounce never hears it -- it records the
         * pads tap -- so the song cannot end up inside its own clips. */
        for (uint8_t o = 0; o < IdfAudioModule::NUM_STREAM_OUTPUTS; ++o) mixer.setRouteEnabled(ch, o, true);
    }
    getPlatformServices().songEngine = &engine;
}

void song_engine_set_open_handler(void (*fn)(const AudioSong& song))
{
    if (s_platform) s_platform->setOpenHandler(fn);
}

void song_engine_set_pads_tap(IdfAudioModule& module, uint8_t kit, uint8_t pitched, uint8_t kitB)
{
    if (s_platform) s_platform->setPadsTap(module, kit, pitched, kitB);
}

uint32_t song_engine_io_fallbacks()
{
    return s_platform ? s_platform->ioFallbacks() : 0;
}

void song_engine_release_arena()
{
    if (s_engine) s_engine->releaseRings();
}

void song_engine_rt_capture(const float* padsBus, uint32_t frames)
{
    if (s_engine) s_engine->rtCapture(padsBus, frames);
}

bool song_engine_capturing()
{
    return s_engine && s_engine->capturing();
}

void song_engine_set_offline(bool allowed)
{
    if (s_platform) s_platform->offlineAllowed_.store(allowed);
}

bool song_engine_capture_backlogged()
{
    return s_engine && s_engine->captureBacklogged();
}

} // namespace crosspad
