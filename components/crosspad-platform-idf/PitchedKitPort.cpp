// SPDX-License-Identifier: GPL-3.0-or-later

#include "PitchedKitPort.hpp"

#include <cmath>

#include "PitchedSampleNode.hpp"
#include "engine_arena.hpp"

#include "crosspad/dsp/FftFactory.hpp"
#include "crosspad/instrument/PitchedInstrument.hpp"
#include "crosspad/instrument/PitchedKitLoader.hpp"
#include "crosspad/kit/KitPathUtils.hpp"

#include "bsp/esp-bsp.h"
#include "bsp_sdmmc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "i2s_driver.h"
#include "sdkconfig.h"

// The load-time resampler runs an 8192-point inverse FFT; esp-dsp sizes its
// twiddle table from this Kconfig choice, and a stale sdkconfig (generated
// before the pitched engine landed) still says 4096. sdkconfig.defaults,
// sdkconfig.v1 and sdkconfig.v2 all carry CONFIG_DSP_MAX_FFT_SIZE_8192=y.
#if CONFIG_DSP_MAX_FFT_SIZE < 8192
#error "CONFIG_DSP_MAX_FFT_SIZE must be 8192: your sdkconfig predates the pitched engine. Regenerate it from sdkconfig.defaults (delete the stale sdkconfig, or build with -DSDKCONFIG=sdkconfig.v2) or set CONFIG_DSP_MAX_FFT_SIZE_8192=y in menuconfig (Component config -> ESP-DSP)."
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdio>
#include <cstring>
#include <string>

/* Loading and unloading a bank, and its status, run on the kit loader or
 * the CDC task; this file is -O2 for the pitched node's render (its inline
 * mix() is compiled here). GCC's `cold` builds them for size and changes
 * nothing about what they do -- the image is PSRAM too (CLAUDE.md). */
#define CP_COLD __attribute__((cold))

namespace crosspad {

namespace {

constexpr const char* TAG = "PitchedKit";

/// How many times installBank() looks for the RT task before giving up. One
/// block is 64 frames -- 1.45 ms at 44.1 kHz -- so ten ticks is two orders of
/// magnitude of slack; past that the RT task is not running at all and waiting
/// longer only delays the kit change.
constexpr uint32_t kGateSpins = 10;

/// Held back from the bank budget for the resampler's transient: the source
/// copy plus up to ~200 kB of overlap-add scratch (8 kHz -> 44.1 kHz, the
/// widest ratio a kit ships), allocated from the same arena *while* the zones
/// already built are resident.
constexpr size_t kResamplerScratchReserve = 256 * 1024;

/// Work between two ticks given away in a bank build (giveWay()).
constexpr int64_t kGiveWayUs = 15000;

void gateYield() { vTaskDelay(1); }

// Zone PCM and the resampler's scratch both come out of the engine arena --
// the same megabytes the prebuilt streamer holds while a sampler kit is current,
// which is why a pitched kit has to stop it first (main/include/engine_handoff.h).
// Sixteen-byte aligned, so esp-dsp's aes3 FFT kernel reads the scratch
// directly instead of bouncing it through EspDspFft's copy path.
void* bankAlloc(size_t bytes) { return engine_arena_alloc(bytes); }
void  bankFree(void* p) { engine_arena_free(p); }   /* also frees a besideKits block */

/* A bank beside the kits' engines: the general PSRAM heap, never below what
 * the UI and the kits still need -- the arena's floor and room for the kits
 * to grow their heads and stream pool. */
constexpr size_t kBesideKeepBytes = 512u * 1024 + 1024u * 1024;
void* bankAllocBeside(size_t bytes) {
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < bytes + kBesideKeepBytes) return nullptr;
    return heap_caps_aligned_alloc(16, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
/* Outside the arena no reset ever takes its blocks: one generation for life. */
uint32_t bankGenerationBeside() { return 0; }
size_t besideBudget() {
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t keep = kBesideKeepBytes + kResamplerScratchReserve;
    return free > keep ? free - keep : 0;
}
/// The bank's memory generation (core SampleBank): a bank that outlives an
/// arena reset forgets its blocks rather than freeing them into the new heap.
uint32_t bankGeneration() { return engine_arena_generation(); }

// The FFT's twiddle table is ~32 kB, built once and kept for the life of the
// process, so it comes from the *general* PSRAM heap and never from the arena:
// engine_arena_reset() wipes the arena on every kit swap and would take the
// table with it.
CP_COLD void* fftAlloc(size_t bytes, size_t align) {
    if (align < 4) align = 4;
    const size_t rounded = (bytes + align - 1) & ~(align - 1);
    return heap_caps_aligned_alloc(align, rounded, MALLOC_CAP_SPIRAM);
}
void fftFree(void* p) { heap_caps_free(p); }

void portLog(const char* line) { ESP_LOGI(TAG, "%s", line); }

// State of the installed bank. Written only under the arena owner lock
// (EngineArenaOwner: a load, an unload, a reset's reclaim); the RT task never
// reads these -- it sees the bank through the instrument's atomic.
SampleBank* s_bank = nullptr;
bool        s_bankBeside = false;   ///< s_bank was built besideKits, outside the arena
PitchedLoadReport s_lastReport;
uint8_t     s_zones = 0;
float       s_roots[SampleBank::kMaxZones] = {};
/* Per zone, what the bank holds: frames of PCM, and the rate that PCM is at
 * -- the engine's where the loader converted, the file's where it did not
 * (Zone::rate carries the ratio either way). Pitch Play needs both to show a
 * loop point as a time rather than a number. */
uint32_t    s_frames[SampleBank::kMaxZones] = {};
uint32_t    s_rates[SampleBank::kMaxZones]  = {};
size_t      s_bytes = 0;
uint8_t     s_maxVoices = 0;
/// What the kit asked for. The instrument caps it (PitchedSampleNode::kVoiceCap).
uint8_t     s_kitMaxVoices = 0;

// Why a load failed, kept for PITCHED_STATUS: reloadPad() has no error channel
// back to the caller, so without this the only trace of a refused kit is a log
// line nobody on a bench is reading. Sanitized on the way in (see setLastError).
char        s_lastError[64] = {0};

/// Failure text handed to the caller and to the log, before it is trimmed into
/// s_lastError for the CDC field.
constexpr size_t kReasonLen = 128;

/// Store @p text as one CDC field: spaces become underscores so a reply stays
/// key=value all the way across, and an empty string means "the last load
/// worked".
CP_COLD void setLastError(const char* text) {
    if (!text || !text[0]) { s_lastError[0] = '\0'; return; }
    size_t i = 0;
    for (; text[i] && i < sizeof(s_lastError) - 1; ++i) {
        const char c = text[i];
        s_lastError[i] = (c == ' ' || c == '\t') ? '_' : c;
    }
    s_lastError[i] = '\0';
}

/// kit.json paths are logical (no mount prefix); stdio needs the real one.
CP_COLD std::string fullSamplePath(const std::string& kitPath, const std::string& sample) {
    std::string p = resolveKitSamplePath(kitPath, sample);
    if (p.empty()) return p;
    const std::string mount = BSP_SD_MOUNT_POINT;
    if (p.compare(0, mount.size(), mount) == 0) return p;
    if (p[0] != '/') p.insert(0, 1, '/');
    return mount + p;
}

/// Whatever backend core settles on -- esp-dsp when its table could be built,
/// the scalar radix-2 otherwise. Both are correct; the difference is ~14 ms per
/// 4096-point transform, once per converted zone at load time.
IFft& fft() {
    FftMemory mem;
    mem.alloc = fftAlloc;
    mem.free  = fftFree;
    mem.log   = portLog;
    return createFft(mem);
}

/// Publish @p bank and retire whatever it replaces. `nullptr` unloads.
CP_COLD void install(SampleBank* bank, const KitInfo* kit) {
    // Defaults come from InstrumentParams; the kit only overrides what it set.
    InstrumentParams params;
    if (kit) {
        if (kit->instrument.maxVoices) params.maxVoices = kit->instrument.maxVoices;
        params.attackMs  = static_cast<float>(kit->instrument.attackMs);
        params.releaseMs = static_cast<float>(kit->instrument.releaseMs);
    }
    s_kitMaxVoices = bank ? params.maxVoices : 0;

    bool safeToFree = false;
    PitchedInstrument& inst = getPitchedSampleNode().instrument();
    SampleBank* old = inst.installBank(bank, params, kit, kGateSpins, gateYield, &safeToFree);
    if (bank) inst.resetStats();

    s_bank      = bank;
    if (!bank) s_bankBeside = false;
    s_zones     = bank ? bank->zoneCount() : 0;
    s_bytes     = bank ? bank->bytes() : 0;
    s_maxVoices = bank ? inst.maxVoices() : 0;
    std::memset(s_roots, 0, sizeof(s_roots));
    std::memset(s_frames, 0, sizeof(s_frames));
    std::memset(s_rates, 0, sizeof(s_rates));
    const uint32_t engineRate = bsp_i2s_get_sample_rate();
    for (uint8_t i = 0; i < s_zones; ++i) {
        const auto& z = bank->zone(i);
        s_roots[i]  = z.rootMidi;
        s_frames[i] = z.length;
        s_rates[i]  = (uint32_t)lroundf((float)engineRate * (z.rate > 0.0f ? z.rate : 1.0f));
    }

    if (!old) return;
    if (old->stale()) {
        /* The second line of defence. Every arena reset takes the owner lock
         * and unloads an installed bank first (pitchedArenaReclaim), so a
         * stale bank here means a reset path that skipped both. Its blocks
         * went with that reset; the destructor forgets them (SampleBank). */
        ESP_LOGE(TAG, "previous bank is from arena generation %u, now %u: forgotten, not freed (%u kB)",
                 (unsigned)old->generation(), (unsigned)engine_arena_generation(),
                 (unsigned)(old->bytes() / 1024));
    }
    if (safeToFree) {
        delete old;
    } else {
        // The RT task never acknowledged the gate, so it may still be inside
        // mix() holding the previous bank's PCM. A leak beats a use-after-free
        // under a live render -- and it is not permanent: the next
        // engine_arena_reset() reclaims it with everything else.
        ESP_LOGE(TAG, "render gate timed out — leaking the previous bank (%u kB)",
                 (unsigned)(old->bytes() / 1024));
    }
}

/// Between blocks of a build: after kGiveWayUs of work, a tick to whoever is
/// waiting -- the UI, the CDC loop, the idle task the watchdog looks at. Every
/// block would add a tick per 4096 frames to a load; this adds ~1 in 16.
void giveWay() {
    static int64_t s_since = 0;
    const int64_t now = esp_timer_get_time();
    if (now - s_since < kGiveWayUs) return;
    vTaskDelay(1);
    s_since = esp_timer_get_time();
}

/// engine_arena_reset()'s borrower hook. The handoff (kit_engines_stop) and
/// every load unload the bank before a reset; this catches a reset path that
/// does not, and unloads properly -- the heap is still the bank's here, the
/// reset re-registers it only after its borrowers return. Runs under the
/// owner lock the reset holds.
CP_COLD void pitchedArenaReclaim(void*) {
    if (!s_bank || s_bankBeside) return;   /* a bank beside the kits is not the arena's */
    ESP_LOGW(TAG, "arena reset with a bank installed: unloading it first");
    pitchedKitUnload();
}

CP_COLD void ensureReclaimHook() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    engine_arena_set_reclaim(&pitchedArenaReclaim, nullptr);
}

} // namespace

size_t pitchedMaxBankBytes() {
    // The arena is the prebuilt streamer's memory while it is up; by the time a bank is built
    // it is empty and all of it is available.
    const size_t cap = engine_arena_capacity();
    return cap > kResamplerScratchReserve ? cap - kResamplerScratchReserve : 0;
}

CP_COLD bool pitchedKitLoad(const KitInfo& kit, char* err, size_t errLen, const PitchedLoadOptions& options) {
    if (err && errLen) err[0] = '\0';

    /* The arena's owner for the whole build: no native take (smpl_take), pad
     * rebuild (smpl_pad) or reset runs while the bank is carved and read.
     * Callers that hand the arena over first (kit_engines_stop) hold it
     * already, across both steps. */
    EngineArenaOwner owner;
    ensureReclaimHook();

    /* The previous bank goes before anything is carved. It used to be kept
     * playing until the new one was installed -- but the handoff before a
     * build resets the arena, so the old bank's blocks were the new bank's
     * by then, and freeing them after the swap was the CORRUPT HEAP of
     * kit_churn 2026-10-09 (HIL PITCHED -> HIL JAM P). The handoff unloads it
     * now; this is for a caller that did not hand over first. */
    if (s_bank) pitchedKitUnload();

    PitchedLoadEnv env;
    env.alloc        = options.besideKits ? bankAllocBeside : bankAlloc;
    env.free         = bankFree;
    env.engineRate   = bsp_i2s_get_sample_rate();
    env.resolvePath  = fullSamplePath;
    env.log          = portLog;
    env.maxBankBytes = options.besideKits ? besideBudget() : pitchedMaxBankBytes();
    env.maxZoneFrames = options.maxZoneSeconds > 0.0f
                      ? (uint32_t)(options.maxZoneSeconds * (float)env.engineRate) : 0;
    env.convertRate  = options.convertRate;
    env.normalize    = options.normalize;
    env.yield        = giveWay;
    env.generation   = options.besideKits ? bankGenerationBeside : bankGeneration;

    // Wider than s_lastError so the reason a caller logs is never the truncated
    // one: PITCHED_STATUS's field is a summary, the log is the diagnosis.
    char reason[kReasonLen];
    reason[0] = '\0';
    s_lastReport = PitchedLoadReport{};
    SampleBank* bank = nullptr;
    if (!pitchedBuildBank(kit, env, fft(), bank, reason, sizeof(reason), &s_lastReport)) {
        if (err && errLen) snprintf(err, errLen, "%s", reason);
        setLastError(reason);
        // Fragmentation, not arithmetic, is what fails a zone or a rate
        // conversion on this board, and core's message cannot see the heap.
        // Beside the kits that is the general PSRAM heap less what it keeps.
        if (options.besideKits)
            ESP_LOGE(TAG, "kit '%s' failed: %s (beside the kits: PSRAM free %u kB, largest %u kB, keeps %u kB)",
                     kit.name.c_str(), reason, (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                     (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                     (unsigned)(kBesideKeepBytes / 1024));
        else
            ESP_LOGE(TAG, "kit '%s' failed: %s (arena free %u kB)", kit.name.c_str(), reason,
                     (unsigned)((engine_arena_capacity() - engine_arena_used()) / 1024));

        // Nothing was swapped in, so the *previous* kit's bank is still
        // installed — and the caller is about to drive it through the new kit's
        // pad_notes. A pad would sound a zone from a kit that is no longer
        // loaded, so the bank goes.
        if (s_bank) pitchedKitUnload();
        return false;
    }

    install(bank, &kit);
    s_bankBeside = options.besideKits;
    setLastError(nullptr);
    ESP_LOGI(TAG, "kit '%s': %u zones, %u kB, maxVoices %u, attack %u ms, release %u ms",
             kit.name.c_str(), (unsigned)s_zones, (unsigned)(s_bytes / 1024),
             (unsigned)s_maxVoices, (unsigned)kit.instrument.attackMs,
             (unsigned)kit.instrument.releaseMs);
    return true;
}

const PitchedLoadReport& pitchedKitLastReport() {
    return s_lastReport;
}

CP_COLD void pitchedKitUnload() {
    if (!s_bank) return;
    EngineArenaOwner owner;   /* the bank's blocks go back to the arena */
    if (!s_bank) return;      /* another owner unloaded it while we waited */
    install(nullptr, nullptr);
    ESP_LOGI(TAG, "unloaded");
}

bool pitchedKitActive() {
    return s_bank != nullptr;
}

bool pitchedBankInArena() {
    return s_bank != nullptr && !s_bankBeside;
}

void pitchedKitApplyPadMix(const KitInfo& kit) {
    if (!s_bank) return;
    PitchedInstrument& inst = getPitchedSampleNode().instrument();
    for (uint8_t pad = 0; pad < PitchedInstrument::kPads && pad < KIT_PADS; ++pad) {
        inst.setPadMix(pad, kit.pads[pad].volume, kit.pads[pad].pan);
    }
}

CP_COLD void pitchedKitGetStatus(PitchedKitStatus& out) {
    out = PitchedKitStatus{};
    out.active    = s_bank != nullptr;
    out.zones     = s_zones;
    out.bankBytes = s_bytes;
    out.lastError = s_lastError;
    std::memcpy(out.roots, s_roots, sizeof(out.roots));
    std::memcpy(out.frames, s_frames, sizeof(out.frames));
    std::memcpy(out.rates, s_rates, sizeof(out.rates));

    control::readPitchedEngine(getPitchedSampleNode().instrument(), out);
    if (!s_bank) out.maxVoices = 0;   // with a bank, the editor may have changed it since
    out.kitMaxVoices = s_kitMaxVoices;
}


} // namespace crosspad
