// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file engine_arena.hpp
 * @brief The PSRAM the sample engines hold, taken as they need it.
 *
 * Every block comes from the general PSRAM heap with a header that links it
 * into one list, and none is handed out where it would take the heap below
 * the UI's floor (engine_arena.cpp, kUiFloorBytes): a kit holds what it needs
 * and gives it back, nothing is reserved at boot. Until 2026-10-18 this was a
 * single block of 4.3-5.4 MB with a `multi_heap` over it, sized for the
 * prebuilt streamer whatever the kit.
 *
 * `engine_arena_reset()` frees the whole list, which drops **every**
 * allocation in one step, whatever an engine's teardown left behind. Only legal when neither engine holds arena memory: after
 * `marcel_engine_stop()` or `pitchedKitUnload()`.
 *
 * Not for the RT task: every entry point takes a mutex and may call into the
 * heap.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Creates the locks and logs the floor; nothing is reserved. Idempotent,
/// always true.
bool engine_arena_init(void);

/// Sixteen-byte aligned (esp-dsp reads bank PCM and resampler scratch directly).
/// Null, with a WARN, where the block would take PSRAM below the UI's floor.
void *engine_arena_alloc(size_t bytes);

/// engine_arena_alloc() without the WARN: for a caller for whom no room is an
/// expected answer (the native engine's heads and pages, a ring's slack).
void *engine_arena_try_alloc(size_t bytes);

/// Frees a pointer from engine_arena_alloc() -- or a plain PSRAM heap block,
/// which a ring that fell back to the general heap hands here too.
void engine_arena_free(void *ptr);

/// Free every allocation on the list. See the file comment.
///
/// Takes the owner lock (engine_arena_owner_lock) for the whole reset, so it
/// waits out a native take or a pitched build another task is in the middle
/// of, and bumps engine_arena_generation().
///
/// Whoever borrowed the arena's slack is asked to let go first (see
/// engine_arena_set_reclaim): a pointer from before the reset has been freed
/// already, and freeing it again corrupts the heap -- which is exactly how the
/// looper found this out.
void engine_arena_reset(void);

/// Called at the top of engine_arena_reset(): give back anything borrowed
/// from the arena, now. **Every borrower registers its own** -- up to eight,
/// appended, duplicates ignored. It used to be one slot, which was enough
/// while the looper's long mode was the only borrower and silently wrong once
/// the native sampler's resident heads became the second. The song engine's
/// rings are released by their own call instead, because they are released on
/// more occasions than this one.
typedef void (*engine_arena_reclaim_fn)(void *ctx);
void engine_arena_set_reclaim(engine_arena_reclaim_fn fn, void *ctx);

/// How many times the arena has been wiped (engine_arena_reset()). A block
/// handed out in one generation is not the heap's in the next: an owner that
/// outlives a reset compares this before it frees (the pitched bank does,
/// through SampleBank's MemoryGenerationFn).
uint32_t engine_arena_generation(void);

/// The arena's *owner* lock: whoever fills the arena as its owner -- a native
/// take, a native pad rebuild, a pitched bank build or unload, the handoff
/// between them (kit_engines_stop) -- holds it for the whole of that, and
/// engine_arena_reset() takes it too. So at most one owner is at work at a
/// time, and the arena is never reset under a bank or a take in progress
/// (kit_churn 2026-10-09: a pitched-to-pitched load reset the arena under the
/// installed bank, then freed it -- CORRUPT HEAP).
///
/// Recursive. Lock order: this one first, then the native engine's
/// LoaderLock; nothing that holds LoaderLock may take this. Not for the RT
/// task or the render. @p waitMs 0 waits for as long as it takes (logging
/// every 5 s); false only on a bounded wait that ran out.
bool engine_arena_owner_lock(uint32_t waitMs);
void engine_arena_owner_unlock(void);

/// What the engines could hold now: what they hold plus the PSRAM above the
/// UI's floor. Moves with the rest of the board's use of PSRAM.
size_t engine_arena_capacity(void);

size_t engine_arena_used(void);

/// Whether a borrower (the song engine's rings, the sample tools' preview
/// ring) may take the arena's slack now: while the native engine holds a kit
/// -- its take leaves the pool's ceiling plus 512 kB of the arena unplanned
/// (SamplerEnginePort.cpp) -- or beside the prebuilt streamer, whose
/// unclaimable stream blocks share one and leave ~1.2 MB idle. Never with a
/// pitched bank: its next load is budgeted against what is left here.
///
/// Every borrower gives its memory back before the arena is reset: the song
/// rings by song_engine_release_arena(), the tools' ring by its reclaim hook.
bool engine_arena_lendable(void);

/// A borrower's allocation: the arena only, and only while lendable() and
/// with what the owner may still claim left in the **largest free block**
/// beside the loan (the native engine's pool can grow to its ceiling and a
/// pad edit can load two more heads; crosspad::arenaMayLend()). Null
/// otherwise; the borrower then takes the general PSRAM heap under its own
/// rules.
void *engine_arena_lend(size_t bytes);

/// A ring segment from the arena's slack: engine_arena_lend() while a kits'
/// engine holds the arena; beside a pitched kit or an instrument, whose bank
/// owns it and whose next one is built beside it, only where 512 kB stay
/// free after it. Null: the general PSRAM heap's turn. Freed with
/// engine_arena_free() and given back before every reset, as a loan.
void *engine_arena_lend_ring(size_t bytes);

/// The largest single block the arena could hand out now, the floor kept.
size_t engine_arena_largest_free(void);

#ifdef __cplusplus
}

/// RAII over engine_arena_owner_lock(): waits as long as it takes.
class EngineArenaOwner {
public:
    EngineArenaOwner() { engine_arena_owner_lock(0); }
    ~EngineArenaOwner() { engine_arena_owner_unlock(); }
    EngineArenaOwner(const EngineArenaOwner&) = delete;
    EngineArenaOwner& operator=(const EngineArenaOwner&) = delete;
};
#endif
