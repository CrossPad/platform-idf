// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SamplePlayerAccess.hpp
 * @brief How the prebuilt sample player is shared between the three threads that
 *        reach into it.
 *
 * The library is a prebuilt blob with global state, and three threads use it:
 *
 *   - **audio_rt** (core 1, prio 19) renders through SampleStreamPly_Process()
 *   - **audio_bg** (core 0, prio 18) refills voices through
 *     SampleStreamPlayer_BkgProc()
 *   - **kit_load** (core 0, prio 1) and the **LVGL thread** restructure voices
 *     through WipeOut()/SetupSample()/GetInfo() — a kit change, a pad edit, a
 *     sample audition
 *
 * Nothing coordinated them, and changing kits while the pads keep playing
 * crashed the board within seconds, two different ways (both reproduced with
 * `crosspad-hil run kit_churn --rapid`):
 *
 * 1. **Two mutators in the library at once.** A header read is one sequence
 *    against the library's global state: open → seek → read → close.
 *    Interleave two and each reads bytes out of the other's file. A header parsed from
 *    the wrong file yields a zero-valued field which the player then divides
 *    by — `IntegerDivideByZero` inside SampleStreamPlayer_GetInfo, one line
 *    after it printed "error invalid format tag!".
 *
 * 2. **Voices freed while being rendered.** WipeOut()/SetupSample() release
 *    and re-point a voice's buffers on core 0 while core 1 is midway through
 *    mixing that same voice — `LoadProhibited` at EXCVADDR 0x4 inside
 *    SampleStreamPly_Process().
 *
 * Two different problems, so two mechanisms, and deliberately not one big lock:
 *
 * **FileGuard** is an ordinary priority-inheriting mutex around one library
 * call. Held per call rather than for a whole pad reload, because audio_bg is
 * what keeps voices fed and blocking it for the length of an SD-backed reload
 * is an audible dropout. Priority inheritance lifts the prio-1 loader instead
 * of leaving audio_bg stuck behind it.
 *
 * **RenderGate** is *not* a mutex. The RT task must never block on a thread
 * doing SD I/O — that is a guaranteed underrun. Instead the mutator asks the
 * renderer to stand down and waits for it to actually be out; while gated,
 * MarcelSampleNode renders silence. The kit change has already stopped every
 * voice by this point, so there is nothing to hear anyway, and the gate is
 * held per pad, not across the whole load, so the other fifteen keep sounding.
 *
 * **StreamHold** keeps audio_bg out for one SetupSample()/WipeOut(): BkgProc()
 * opens, reads and closes the files of the samples such a call changes. Per
 * call too, like the file mutex.
 */

#include <atomic>
#include <cstdint>

#include "sdkconfig.h"

namespace crosspad {

// ── The player's one global file slot ───────────────────────────────────────

/// Create the file mutex. Call once, before any task that touches the player
/// exists. Locking before this is a no-op — nothing is running to race with.
void samplePlayerAccessInit();

void samplePlayerFileLock();     ///< Recursive: nesting guards is safe.
void samplePlayerFileUnlock();
/// Take the slot only if it frees up within `ms`. For the LVGL thread: a kit
/// load paces its reads to the streamer, so one SetupSample() can hold the
/// slot for half a second, and a panel that waits for it freezes the UI.
bool samplePlayerFileTryLock(uint32_t ms);

/// Scoped guard around one sample-player library call that touches a file.
/// For mutators — the kit loader, the pad editor, an audition — against each
/// other; the streamer reads through a slot of its own.
class SamplePlayerFileGuard {
public:
    SamplePlayerFileGuard() { samplePlayerFileLock(); }
    ~SamplePlayerFileGuard() { samplePlayerFileUnlock(); }

    SamplePlayerFileGuard(const SamplePlayerFileGuard&) = delete;
    SamplePlayerFileGuard& operator=(const SamplePlayerFileGuard&) = delete;
};

/// Like SamplePlayerFileGuard, but gives up: `ok()` says whether the call
/// may proceed. The caller answers "not now" and is asked again later.
class SamplePlayerFileTryGuard {
public:
    explicit SamplePlayerFileTryGuard(uint32_t ms) : held_(samplePlayerFileTryLock(ms)) {}
    ~SamplePlayerFileTryGuard() { if (held_) samplePlayerFileUnlock(); }
    bool ok() const { return held_; }

    SamplePlayerFileTryGuard(const SamplePlayerFileTryGuard&) = delete;
    SamplePlayerFileTryGuard& operator=(const SamplePlayerFileTryGuard&) = delete;

private:
    bool held_;
};

/// audio_bg only, around one SampleStreamPlayer_BkgProc(). The streamer has
/// its own file slot in the port (sample_player_port.cpp), so it no longer
/// waits on the mutators' file mutex: a kit load's 30..200 ms library calls
/// used to stall every refill and run the voices that kept sounding dry.
/// What it still yields to is the engine teardown — see
/// SamplePlayerEngineGuard. Returns false when that is waiting: skip this
/// pass, the caller's own delay is the backoff.
///
/// A plain mutex would not do there either. FreeRTOS wakes the highest-
/// priority waiter and has no anti-starvation, so audio_bg at prio 18,
/// looping with a 1 ms delay, re-takes a mutex before the prio-5 loader is
/// ever scheduled — and priority inheritance only applies once the loader
/// *holds* it. Yielding on demand is what keeps the stop bounded.
bool samplePlayerStreamBegin();
void samplePlayerStreamEnd();

/// Everyone else who must keep the streamer out: the engine teardown, which
/// frees the buffers BkgProc() fills.
void samplePlayerStreamExcludeLock();
void samplePlayerStreamExcludeUnlock();

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/// True while someone holds the streamer out (the exclusion above): a reader
/// that would wait for the streamer's pass to end has no pass to wait for.
bool samplePlayerStreamHeld();

/// The streamer out for a change of samples. The library's files are the
/// port's handles; BkgProc() opens, reads and closes them for the voices it
/// refills, and a mutator that changes the samples (SetupSample, WipeOut)
/// keeps it out meanwhile.
class SamplePlayerStreamHold {
public:
    SamplePlayerStreamHold() { samplePlayerStreamExcludeLock(); }
    ~SamplePlayerStreamHold() { samplePlayerStreamExcludeUnlock(); }

    SamplePlayerStreamHold(const SamplePlayerStreamHold&) = delete;
    SamplePlayerStreamHold& operator=(const SamplePlayerStreamHold&) = delete;
};
#endif

// ── Excluding the RT renderer while voices are restructured ─────────────────

namespace detail {
// `gated` is the mutator's request to stand down; `rendering` is the renderer
// saying it is inside Process(). Both are seq_cst and the renderer publishes
// `rendering` *before* reading `gated`, so the two cannot both conclude they
// have the player: if the renderer sees gated==false it had already stored
// rendering==true, which the mutator is guaranteed to observe.
inline std::atomic<bool>& samplePlayerGated() {
    static std::atomic<bool> gated{false};
    return gated;
}
inline std::atomic<bool>& samplePlayerRendering() {
    static std::atomic<bool> rendering{false};
    return rendering;
}
}  // namespace detail

/// RT side. Returns false when a mutator holds the gate — render silence and
/// do not call into the player. Never blocks.
inline bool samplePlayerRenderBegin() {
    detail::samplePlayerRendering().store(true, std::memory_order_seq_cst);
    if (detail::samplePlayerGated().load(std::memory_order_seq_cst)) {
        detail::samplePlayerRendering().store(false, std::memory_order_seq_cst);
        return false;
    }
    return true;
}

inline void samplePlayerRenderEnd() {
    detail::samplePlayerRendering().store(false, std::memory_order_seq_cst);
}

/// Mutator side. Blocks until the renderer is out of the player, then keeps it
/// out until the gate is released. Must not be called from the RT task.
void samplePlayerGateEnter();
void samplePlayerGateExit();

/// How long the gate has been held: every gated block is a block of silence
/// from MarcelSampleNode, so the hold time of a kit load is its audible cost.
/// `maxUs` is the longest outermost hold since the previous call (read and
/// cleared); `totalUs` and `count` are cumulative.
void samplePlayerGateStats(uint32_t* maxUs, uint32_t* totalUs, uint32_t* count);

/// Scoped gate around a structural change to the player's voices.
class SamplePlayerRenderGate {
public:
    SamplePlayerRenderGate() { samplePlayerGateEnter(); }
    ~SamplePlayerRenderGate() { samplePlayerGateExit(); }

    SamplePlayerRenderGate(const SamplePlayerRenderGate&) = delete;
    SamplePlayerRenderGate& operator=(const SamplePlayerRenderGate&) = delete;
};

/// All at once: a structural change that also opens, reads and closes files,
/// which is what SetupSample() and WipeOut() are. Order matters — the
/// streamer out first, then the gate, so a mutator never holds the file mutex
/// while waiting for the renderer.
class SamplePlayerMutationGuard {
public:
    SamplePlayerMutationGuard() = default;

private:
#if CONFIG_CROSSPAD_SAMPLER_MARCEL
    SamplePlayerStreamHold stream_;  // first: taken before, released after the rest
#endif
    SamplePlayerRenderGate gate_;
    SamplePlayerFileGuard  file_;
};

/// The whole engine: renderer out, streamer out, file slot held. For
/// SampleStreamPlayer_DeInit() and nothing smaller — a per-pad change holds
/// the streamer out for one call at a time.
class SamplePlayerEngineGuard {
public:
    SamplePlayerEngineGuard() { samplePlayerStreamExcludeLock(); }
    ~SamplePlayerEngineGuard() { samplePlayerStreamExcludeUnlock(); }

    SamplePlayerEngineGuard(const SamplePlayerEngineGuard&) = delete;
    SamplePlayerEngineGuard& operator=(const SamplePlayerEngineGuard&) = delete;

private:
    SamplePlayerMutationGuard mutation_;
};

}  // namespace crosspad
