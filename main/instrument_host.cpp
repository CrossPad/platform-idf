// SPDX-License-Identifier: GPL-3.0-or-later
#include "instrument_host.h"

#include "PitchedKitPort.hpp"
#include "SamplerEnginePort.hpp"
#include "SongEnginePort.hpp"
#include "engine_arena.hpp"
#include "engine_handoff.h"
#include "lv_async_safe.h"
#include "song_sounds.h"

#include "crosspad-sampler/logic/kit_load_task.hpp"
#include "crosspad/instrument/IInstrumentHost.hpp"
#include "crosspad/kit/IKitManager.hpp"
#include "crosspad/kit/KitPathUtils.hpp"
#include "crosspad/kit/PortableKitLoader.hpp"
#include "crosspad/platform/PlatformServices.hpp"
#include "crosspad/song/ISongEngine.hpp"

#include "bsp_sdmmc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {

const char* TAG = "inst_host";

constexpr const char* kLibrary = "/crosspad/pitchplay";
/* SD I/O and the bank build, as on the kit loader: 8 kB from PSRAM. */
constexpr uint32_t kStack = 8192;
/* Below the UI (LVGL drv, 4, also on core 0): a bank build is seconds of CPU
 * when its samples need a rate conversion, and the screen must keep drawing
 * "Loading" meanwhile. */
constexpr UBaseType_t kPriority = 3;
/// How long a load waits for a kit load already under way.
constexpr int kKitLoadWaitTicks = 300;   // x 100 ms

enum class Job : uint8_t { None, Load, Open, Save };

std::string fs(const std::string& logical)
{
    return std::string(BSP_SD_MOUNT_POINT) + logical;
}

std::string parent(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

std::string base(const std::string& path)
{
    const size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

/// mkdir -p, on the mount.
bool make_dirs(const std::string& logical)
{
    std::string p = fs(logical);
    for (size_t i = std::strlen(BSP_SD_MOUNT_POINT) + 1; i <= p.size(); ++i) {
        if (i < p.size() && p[i] != '/') continue;
        const std::string part = p.substr(0, i);
        if (mkdir(part.c_str(), 0775) != 0 && errno != EEXIST) return false;
    }
    return true;
}

constexpr size_t kCopyChunk = 4096;

bool copy_file(const std::string& fromFs, const std::string& toFs)
{
    FILE* in = std::fopen(fromFs.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(toFs.c_str(), "wb");
    if (!out) { std::fclose(in); return false; }
    /* PSRAM, and only while a copy runs: it was a static 4 kB of internal
     * RAM held for good. stdio moves the bytes through each FILE's own
     * buffer, so this one never meets the SDMMC DMA. */
    char* buf = static_cast<char*>(heap_caps_malloc(kCopyChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buf) { std::fclose(in); std::fclose(out); std::remove(toFs.c_str()); return false; }
    bool ok = true;
    size_t n;
    while ((n = std::fread(buf, 1, kCopyChunk, in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    if (std::ferror(in)) ok = false;
    heap_caps_free(buf);
    std::fclose(in);
    if (std::fclose(out) != 0) ok = false;
    if (!ok) std::remove(toFs.c_str());
    return ok;
}

/// The open song plays a Pitch track beside kit tracks: its instrument is
/// built beside the kits' engines (PitchedLoadOptions::besideKits), not in
/// their place.
bool song_wants_beside()
{
    crosspad::ISongEngine* se = crosspad::getPlatformServices().songEngine;
    if (!se || se->openState() != crosspad::SongOpState::Ready) return false;
    std::lock_guard<std::recursive_mutex> l(se->songMutex());
    const crosspad::AudioSong& song = se->song();
    /* Any kit track, live or not: one that is not plays its freeze now and
     * takes the kits' engine back when it is armed. */
    bool kits = false;
    for (int t = 0; t < crosspad::AudioSong::kTracks; ++t) kits = kits || song.seqRoute(t) == 0;
    return song.hasPitchTrack() && kits;
}

class InstrumentHost final : public crosspad::IInstrumentHost {
public:
    bool requestLoad(const crosspad::KitInfo& spec) override { return submit(Job::Load, &spec, nullptr); }
    bool requestOpen(const std::string& path) override { return submit(Job::Open, nullptr, &path); }
    bool requestSave(const crosspad::KitInfo& spec) override { return submit(Job::Save, &spec, nullptr); }

    void release() override
    {
        {
            std::lock_guard<std::mutex> l(mu_);
            pending_ = Job::None;   // not started: not wanted any more
            /* No worker yet: nothing was ever loaded, there is nothing to give back. */
            if (!task_) return;
        }
        releasePending_.store(true);
        xSemaphoreGive(wake_);
    }

    State state() const override { return state_.load(); }

    crosspad::KitInfo instrument() const override
    {
        std::lock_guard<std::mutex> l(mu_);
        return instrument_;
    }

    std::string message() const override
    {
        std::lock_guard<std::mutex> l(mu_);
        return message_;
    }

    std::string libraryRoot() const override { return kLibrary; }

    /* Straight off the installed bank, through the port's status. Zero where
     * nothing is loaded, which is what the interface promises; the caller
     * (Pitch Play's loop points) treats frames == 0 as "cannot say". */
    ZoneAudio zoneAudio(uint8_t zoneIdx) const override
    {
        ZoneAudio out;
        if (zoneIdx >= crosspad::SampleBank::kMaxZones) return out;
        crosspad::PitchedKitStatus st;
        crosspad::pitchedKitGetStatus(st);
        if (!st.active || zoneIdx >= st.zones) return out;
        out.frames     = st.frames[zoneIdx];
        out.sampleRate = st.rates[zoneIdx];
        return out;
    }

    bool owns() const { return owned_.load(); }
    bool beside() const { return beside_.load(); }

    /// Built in the kits' place for a song that plays it beside them -- asked
    /// for while the song was still opening, when song_wants_beside() could
    /// not tell: given back, so the kits play again and the next request
    /// builds it beside them.
    void moveBeside()
    {
        if (owned_.load() && state_.load() == State::Ready && song_wants_beside()) release();
    }
    uint32_t lastMs() const { return lastMs_.load(); }

    bool start()
    {
        wake_ = xSemaphoreCreateBinary();
        return wake_ != nullptr;
    }

private:
    /* The worker and its 8 kB PSRAM stack exist from the first request on,
     * not from boot: a board that never opens Pitch Play never pays for them
     * (and its TCB is internal RAM). Under mu_. */
    bool ensure_task_locked()
    {
        if (task_) return true;
        return xTaskCreatePinnedToCoreWithCaps(task, "inst_host", kStack, this, kPriority,
                                               &task_, 0, MALLOC_CAP_SPIRAM) == pdPASS;
    }

    bool submit(Job job, const crosspad::KitInfo* spec, const std::string* path)
    {
        {
            /* Newest wins: a sample picked while another loads replaces
             * whatever was waiting, and the one loading is not interrupted. */
            std::lock_guard<std::mutex> l(mu_);
            if (!ensure_task_locked()) {
                message_ = "No memory for the instrument's worker";
                state_.store(State::Failed);
                return false;
            }
            pending_ = job;
            if (spec) spec_ = *spec;
            if (path) path_ = *path;
            message_.clear();
        }
        // A load asked for before an earlier release was carried out keeps
        // the engine: it is the same app, back again.
        releasePending_.store(false);
        state_.store(State::Busy);
        xSemaphoreGive(wake_);
        return true;
    }

    static void task(void* self)
    {
        static_cast<InstrumentHost*>(self)->run();
    }

    void run()
    {
        for (;;) {
            xSemaphoreTake(wake_, portMAX_DELAY);
            for (;;) {
                Job job;
                crosspad::KitInfo spec;
                std::string path;
                {
                    std::lock_guard<std::mutex> l(mu_);
                    job = pending_;
                    pending_ = Job::None;
                    if (job != Job::None) {
                        spec = spec_;
                        path = path_;
                    }
                }
                if (job != Job::None) {
                    const int64_t t0 = esp_timer_get_time();
                    const bool ok = job == Job::Load ? load(spec)
                                  : job == Job::Open ? open(path)
                                                     : save(spec);
                    /* Before Ready is published: whoever takes the
                     * instrument then takes the path the song kept. A save
                     * is kept too -- the library's copy replaces the song's. */
                    if (ok) keep_in_song(job != Job::Open);
                    lastMs_.store((uint32_t)((esp_timer_get_time() - t0) / 1000));
                    std::lock_guard<std::mutex> l(mu_);
                    // Busy until the last request is through: a caller that
                    // saw Ready here would take an instrument already replaced.
                    if (pending_ == Job::None) {
                        state_.store(ok ? (owned_.load() || beside_.load() ? State::Ready : State::Idle)
                                        : State::Failed);
                    }
                    continue;
                }
                if (releasePending_.exchange(false)) {
                    give_back();
                    continue;
                }
                break;
            }
        }
    }

    /// The open song's Pitch track keeps what it plays (SongSounds::keepInstrument);
    /// @p changed: built or saved from edits, not opened as its file is.
    void keep_in_song(bool changed)
    {
        crosspad::KitInfo spec;
        {
            std::lock_guard<std::mutex> l(mu_);
            spec = instrument_;
        }
        song_sounds_keep_instrument(spec, [this](const crosspad::KitInfo& s) { return save(s); }, changed);
    }

    void fail(const std::string& why)
    {
        ESP_LOGW(TAG, "%s", why.c_str());
        std::lock_guard<std::mutex> l(mu_);
        message_ = why;
    }

    /// The engine is the app's from here; true the first time. A kit load
    /// already running finishes first, or it would start the kits' engine
    /// again under us. The caller then hands the arena over
    /// (kit_engines_stop) under the owner lock it holds for the build --
    /// taken *after* this wait: a pitched kit's load needs that lock to finish.
    bool take_engine()
    {
        if (owned_.exchange(true)) return false;
        for (int i = 0; i < kKitLoadWaitTicks && crosspad_sampler::kit_load_task_is_loading(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return true;
    }

    bool load(crosspad::KitInfo spec)
    {
        if (spec.instrument.zones.empty()) {
            fail("No samples in the instrument");
            return false;
        }
        spec.engine = crosspad::KitEngine::Pitched;
        /* A song's Pitch track beside its kit tracks: the kits keep the
         * engine, and the bank is built beside them. Not once an app holds
         * the engine already -- it has the arena to itself. */
        const bool beside = !owned_.load() && song_wants_beside();
        const bool fresh = beside ? false : take_engine();
        /* The arena's owner across the handoff and the build, as a pitched
         * kit's load holds it (engine_arena.hpp): a native take a kit load
         * left running finishes first, and none runs under the bank. */
        EngineArenaOwner owner;
        /* Whichever kits' engine holds the arena -- the native engine's heads,
         * the prebuilt streamer, a kit's pitched bank -- lets go, and the bank
         * gets all of it. An app already holding the engine normally has only
         * its own bank in there, which pitchedKitLoad() unloads before it
         * builds -- but not after a load of its that failed: the native engine
         * may have taken a kit since (a take is dropped only while a bank is
         * installed), and a bank is never built beside its heads. */
        if (!beside && (fresh || native_sampler_active())) kit_engines_stop();
        char err[96] = {};
        /* No rate conversion: a 44.1 kHz sample on a 48 kHz engine was 8 s of
         * "Loading" for 4 s of audio; kept at its rate it is the card read. */
        crosspad::PitchedLoadOptions options;
        options.maxZoneSeconds = kMaxZoneSeconds;
        options.convertRate = false;
        /* Takes off the mics peak far below full scale: played as recorded,
         * an instrument made of them was all but silent beside the kits. */
        options.normalize = true;
        options.besideKits = beside;
        if (!crosspad::pitchedKitLoad(spec, err, sizeof(err), options)) {
            beside_.store(false);
            fail(err[0] ? err : "The samples could not be loaded");
            return false;
        }
        beside_.store(beside);
        crosspad::PitchedKitStatus st;
        crosspad::pitchedKitGetStatus(st);
        for (size_t i = 0; i < spec.instrument.zones.size() && i < st.zones; ++i) {
            auto& z = spec.instrument.zones[i];
            if (z.root < 0) z.root = st.roots[i];
        }
        const auto& report = crosspad::pitchedKitLastReport();
        std::lock_guard<std::mutex> l(mu_);
        instrument_ = spec;
        message_ = report.warning;
        return true;
    }

    bool open(const std::string& path)
    {
        crosspad::PortableKitLoader reader(BSP_SD_MOUNT_POINT);
        crosspad::KitInfo spec;
        spec.path = path;
        const auto result = reader.loadKit(spec);
        if (!result.success) {
            fail(result.error.empty() ? "The instrument could not be read" : result.error);
            return false;
        }
        if (spec.engine != crosspad::KitEngine::Pitched) {
            fail("Not an instrument: " + path);
            return false;
        }
        return load(spec);
    }

    bool save(crosspad::KitInfo spec)
    {
        const std::string dir = parent(spec.path);
        if (dir.empty() || !make_dirs(dir + "/SAMPLES")) {
            fail("Cannot make " + dir);
            return false;
        }
        spec.engine = crosspad::KitEngine::Pitched;
        /* Every zone's sample inside the instrument: a take the Recorder
         * reuses or a file someone deletes must not take a zone with it. */
        std::vector<std::string> taken;
        for (size_t i = 0; i < spec.instrument.zones.size(); ++i) {
            auto& z = spec.instrument.zones[i];
            const std::string from = crosspad::resolveKitSamplePath(spec.path, z.sample);
            if (from.compare(0, dir.size() + 1, dir + "/") == 0) {
                z.sample = from.substr(dir.size() + 1);
                taken.push_back(base(from));
                continue;
            }
            std::string name = base(from);
            for (const auto& t : taken) {
                if (t == name) { name = std::to_string(i + 1) + "_" + name; break; }
            }
            if (!copy_file(fs(from), fs(dir + "/SAMPLES/" + name))) {
                fail("Cannot copy " + base(from));
                return false;
            }
            taken.push_back(name);
            z.sample = "SAMPLES/" + name;
        }
        crosspad::PortableKitLoader writer(BSP_SD_MOUNT_POINT);
        if (!writer.saveKit(spec)) {
            fail("Cannot write " + spec.path);
            return false;
        }
        std::lock_guard<std::mutex> l(mu_);
        instrument_ = spec;
        return true;
    }

    void give_back()
    {
        /* Beside the kits nothing was taken from them: the bank goes only
         * once the song no longer plays a Pitch track beside them. */
        if (beside_.load()) {
            if (song_wants_beside()) return;
            crosspad::pitchedKitUnload();
            beside_.store(false);
            state_.store(State::Idle);
            return;
        }
        if (!owned_.load()) return;
        crosspad::pitchedKitUnload();
        owned_.store(false);
        state_.store(State::Idle);
        auto* kits = crosspad::getKitManager();
        if (kits && kits->getCurrentKit()) {
            /* The kit as it was: reloadPad() on pad 0 resets the arena and
             * starts the kits' engine, or builds the kit's own bank if it is
             * pitched. */
            lv_async_call_locked([](void*) { crosspad_sampler::kit_load_task_start(false, -1); }, nullptr);
            return;
        }
        /* No kit since boot: there is nothing to reload, but the kits' engine
         * was running and a kit picked later expects it to be. */
        kit_engines_idle();
    }

    SemaphoreHandle_t wake_ = nullptr;
    TaskHandle_t task_ = nullptr;
    mutable std::mutex mu_;
    Job pending_ = Job::None;
    crosspad::KitInfo spec_;
    std::string path_;
    crosspad::KitInfo instrument_;
    std::string message_;
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> owned_{false};
    std::atomic<bool> beside_{false};   ///< the bank is beside the kits' engines (besideKits)
    std::atomic<bool> releasePending_{false};
    std::atomic<uint32_t> lastMs_{0};
};

InstrumentHost* s_host = nullptr;

} // namespace

void instrument_host_init()
{
    if (s_host) return;
    static InstrumentHost host;
    if (!host.start()) {
        ESP_LOGE(TAG, "worker not started; no app can borrow the engine");
        return;
    }
    s_host = &host;
    crosspad::getPlatformServices().instrumentHost = &host;
}

bool instrument_host_owns_engine()
{
    return s_host && s_host->owns();
}

bool instrument_host_beside()
{
    return s_host && s_host->beside();
}

void instrument_host_move_beside()
{
    if (s_host) s_host->moveBeside();
}

uint32_t instrument_host_last_ms()
{
    return s_host ? s_host->lastMs() : 0;
}
