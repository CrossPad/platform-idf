// SPDX-License-Identifier: GPL-3.0-or-later

#include "SongEnginePort.hpp"
#include "engine_handoff.h"

#include "PitchedKitPort.hpp"
#include "SamplerEnginePort.hpp"
#include "engine_arena.hpp"

#include "esp_log.h"

#include <inttypes.h>

void kit_engines_stop(void)
{
    /* The arena's owner across the whole handoff, and -- for a caller that
     * builds a bank next -- held by that caller across the build too
     * (recursive): no native take or pad rebuild starts between the last
     * owner letting go and the next one filling it. */
    EngineArenaOwner owner;
    /* Every owner's allocations go back *before* the reset, each into the
     * heap that handed them out. The pitched bank first: it was left
     * installed here, the reset below wiped it, and the next load's swap
     * freed its blocks into the new heap -- CORRUPT HEAP in multi_heap_free
     * (kit_churn --rapid, HIL PITCHED -> HIL JAM P, 2026-10-09). A bank
     * beside the kits -- a song's Pitch track's -- is not the arena's: the
     * reset leaves it, and the next bank build unloads it itself. */
    if (crosspad::pitchedBankInArena()) crosspad::pitchedKitUnload();
    /* The native engine: its heads are arena memory and its voices read
     * them. Releasing never needs it to be current (SamplerEnginePort). */
    native_sampler_release();
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    marcel_engine_stop();    /* its own song release + reset when it was up */
#endif
    crosspad::song_engine_release_arena();
    engine_arena_reset();
}

void kit_engines_idle(void)
{
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    crosspad::song_engine_release_arena();
    engine_arena_reset();
    marcel_engine_start();
#else
    kit_engines_stop();
#endif
}

#if CONFIG_CROSSPAD_SAMPLER_MARCEL

#include "MarcelSampleNode.hpp"
#include "SamplePlayerAccess.hpp"

#include <ml_smpl_stream_ply.h>

static const char *TAG = "EngineHandoff";

/* sample_player_port.cpp: what the library's memory hooks have moved, so
 * DeInit's semantics can be read off the console rather than assumed. */
extern uint32_t g_marcel_malloc_bytes, g_marcel_malloc_count;
extern uint32_t g_marcel_fast_bytes, g_marcel_fast_internal_bytes;
extern uint32_t g_marcel_free_bytes, g_marcel_free_count;
extern uint32_t g_marcel_free_untracked, g_marcel_blocks_lost;
extern void sample_player_port_forget_blocks(void);
extern void sample_player_port_flush_files(void);
extern void sample_player_port_close_library_files(void);
extern void sample_player_port_engine_started(void);
extern void sample_player_port_set_poly_cap(uint8_t cap);

/* The library's memory, chosen here: every sample it sets up takes a first
 * block of kStartBlockBytes and stream blocks of kBlockBytes, all from the
 * arena (SampleStreamPlayer_MallocSlowMemory) -- ~84 kB a sample (HIL SINE A:
 * 16 samples, 1343 kB), so its 24 sample slots stay near 2 MB, under half of
 * what the arena gives above the UI's floor. 16 kB is the native engine's
 * stream block too: one multi-sector command through the card's sector
 * bounce. */
static constexpr uint32_t kStartBlockBytes = 32 * 1024;
static constexpr uint32_t kBlockBytes = 16 * 1024;

bool marcel_engine_running(void)
{
    return crosspad::marcelEngineReady().load(std::memory_order_acquire);
}

int marcel_engine_free_slots(void)
{
    return marcel_engine_running() ? (int)SampleStreamPlayer_GetFreeWavCnt() : -1;
}

static engine_reclaim_fn s_reclaim = NULL;
static void *s_reclaimCtx = NULL;

void engine_set_reclaim_hook(engine_reclaim_fn fn, void *ctx)
{
    s_reclaim = fn;
    s_reclaimCtx = ctx;
}

/* The voice cap the port gives SampleStreamPlayer_SetMaxPolyphony() after
 * every Init(). These two are other names for the same knob: SMPL_PEAK
 * reports what is in force, SMPL_POLY overrides it. */
extern uint8_t sample_player_port_poly(void);

extern "C" uint8_t marcel_engine_poly(void) { return sample_player_port_poly(); }

extern "C" void marcel_engine_set_poly(uint8_t maxPoly)
{
    if (maxPoly > 17) maxPoly = 17;
    sample_player_port_set_poly_cap(maxPoly);   // 0: back to the port's cap
}

bool marcel_engine_start(void)
{
    if (marcel_engine_running()) return true;

    /* The arena first: a borrower can hold all of it, and Init below
     * allocates into it. Nothing is asked -- whoever is loading a kit is
     * already past asking. */
    if (s_reclaim != NULL) s_reclaim(s_reclaimCtx);

    engine_arena_init();

    const uint32_t bytesBefore = g_marcel_malloc_bytes;
    const uint32_t countBefore = g_marcel_malloc_count;
    const uint32_t fastBefore = g_marcel_fast_bytes;
    const uint32_t fastInternalBefore = g_marcel_fast_internal_bytes;

    if (!SampleStreamPlayer_Init(kStartBlockBytes, kBlockBytes))
    {
        SampleStreamPlayer_ErrorMsg("Error: SampleStreamPlayer_Init failed!");
        return false;
    }
    /* The voice cap and the descriptors (sample_player_port.cpp), before the
     * engine is published. */
    sample_player_port_engine_started();

    crosspad::marcelEngineReady().store(true, std::memory_order_release);

    ESP_LOGI(TAG, "voice cap %u", (unsigned)marcel_engine_poly());
    ESP_LOGI(TAG, "started: Init(%" PRIu32 ", %" PRIu32 ") took %" PRIu32 " B in %" PRIu32
             " blocks (fast %" PRIu32 " B, %" PRIu32 " B of it internal); arena %u/%u kB used",
             kStartBlockBytes, kBlockBytes,
             g_marcel_malloc_bytes - bytesBefore, g_marcel_malloc_count - countBefore,
             g_marcel_fast_bytes - fastBefore, g_marcel_fast_internal_bytes - fastInternalBefore,
             (unsigned)(engine_arena_used() / 1024),
             (unsigned)(engine_arena_capacity() / 1024));
    return true;
}

void marcel_engine_stop(void)
{
    if (!marcel_engine_running()) return;

    /* Published first. The RT node and the BG refill both check it: the node
     * contributes silence from the next block, and the refill skips — it reads
     * the flag *after* taking the file mutex, so it can never observe "up" and
     * then find the library torn down under it. */
    crosspad::marcelEngineReady().store(false, std::memory_order_release);

    const uint32_t allocBefore = g_marcel_malloc_bytes;
    const uint32_t freedBefore = g_marcel_free_bytes;
    const uint32_t freeCntBefore = g_marcel_free_count;

    {
        /* Gate + file mutex + streamer: waits out an in-flight
         * SampleStreamPly_Process() and an in-flight SampleStreamPlayer_BkgProc()
         * before the library's buffers go back to the arena. See
         * SamplePlayerAccess.hpp. */
        crosspad::SamplePlayerEngineGuard guard;
        for (uint8_t note = 0; note < SAMPLE_STREAM_PLY_CH_CNT; ++note)
        {
            SampleStreamPlayer_NoteOff(note);
        }
        SampleStreamPlayer_WipeOut();
        SampleStreamPlayer_DeInit();
        /* The library closed its files in WipeOut()/DeInit(); what it left
         * open goes now, and the cache's idle handles with it -- the mount
         * only has 32 descriptors. */
        sample_player_port_close_library_files();
        sample_player_port_flush_files();
    }

    const uint32_t alloced = allocBefore;
    const uint32_t freed = g_marcel_free_bytes - freedBefore;
    ESP_LOGW(TAG,
             "DeInit: hooks saw %" PRIu32 " B alloc total, %" PRIu32 " B freed here "
             "(%" PRIu32 " blocks), outstanding %" PRIu32 " B; untracked frees %" PRIu32
             ", untracked blocks %" PRIu32,
             alloced, freed, g_marcel_free_count - freeCntBefore,
             alloced - g_marcel_free_bytes, g_marcel_free_untracked, g_marcel_blocks_lost);

    /* Whatever DeInit did or did not hand back, the arena is empty after this
     * — so what the prebuilt library's DeInit() keeps does not matter here. The song engine's rings borrow its slack, so they go first. */
    crosspad::song_engine_release_arena();
    engine_arena_reset();
    sample_player_port_forget_blocks();
    g_marcel_malloc_bytes = g_marcel_malloc_count = 0;
    g_marcel_free_bytes = g_marcel_free_count = 0;
}

#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */
