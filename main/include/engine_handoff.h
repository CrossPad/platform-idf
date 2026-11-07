// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <stdint.h>

/**
 * @file engine_handoff.h
 * @brief Handing the engine arena between the kits' sample engine(s) and the
 *        pitched instrument.
 *
 * The native engine (SamplerEnginePort) plays every sampler kit. The prebuilt
 * streamer below is built only with CONFIG_CROSSPAD_SAMPLER_MARCEL.
 *
 * Only one sample engine is current at a time, and the prebuilt streamer's
 * ~4.2 MB (inside the 4608 kB arena it was sized for) is most of the PSRAM pool. A pitched kit therefore stops it —
 * `SampleStreamPlayer_DeInit()` followed by `engine_arena_reset()`, which
 * reclaims the arena whether or not DeInit gave the blocks back — and a
 * sampler kit starts it again.
 *
 * `smpl_stream_ply_ready()` is this module's flag. Nothing may call into the
 * prebuilt library while it is false.
 */

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Whichever engine holds the arena lets go of it, and the arena is empty
/// after: the installed pitched bank, the native sampler's voices and
/// resident heads, the prebuilt streamer when it is built and running, the
/// song engine's borrowed rings -- each freed into the heap it came from,
/// *then* the reset. Under the arena owner lock (engine_arena_owner_lock);
/// a caller that builds a bank next holds EngineArenaOwner across both.
/// What a pitched kit or an app (instrument_host) calls before it builds a
/// bank. Was marcel_engine_stop() alone, which left the native engine's heads
/// in the arena -- and the engine rendering -- under a pitched bank.
void kit_engines_stop(void);

/// Back to the kits' engine with nothing loaded: the prebuilt streamer starts
/// when it is built; the native engine needs only an empty arena (a sampler
/// kit's load ends in native_sampler_take_kit()).
void kit_engines_idle(void);

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/// Whoever is holding the arena gives it back, now. Called at the top of
/// marcel_engine_start(): a borrower can own the arena outright, and Init
/// would allocate into memory it is playing from. The first borrower was the
/// looper's long mode, which is no longer in this firmware; the native
/// sampler's resident heads are the one that is (SamplerEnginePort).
typedef void (*engine_reclaim_fn)(void *ctx);
void engine_set_reclaim_hook(engine_reclaim_fn fn, void *ctx);

/// Init + the voice cap + ready flag. Idempotent; false means the library
/// refused to initialise and the flag stays down.
bool marcel_engine_start(void);

/// All notes off, DeInit, ready flag down, arena reset. Idempotent.
///
/// Runs under the mutation guard, so it waits out an in-flight render and an
/// in-flight `SampleStreamPlayer_BkgProc()` before the library is torn down.
void marcel_engine_stop(void);

/// Same flag `smpl_stream_ply_ready()` reports.
bool marcel_engine_running(void);

/// Free WAV slots, or -1 while the engine is stopped — the library's own
/// accessor walks state that DeInit has released.
int marcel_engine_free_slots(void);

/// Voices the engine may sound at once (SampleStreamPlayer_SetMaxPolyphony()):
/// the port's cap of 16, or the SMPL_POLY override.
uint8_t marcel_engine_poly(void);

/// Override the cap (1..17) as SMPL_POLY does; 0 goes back to the port's
/// cap. Held across kit loads (sample_player_port_set_poly_cap).
void marcel_engine_set_poly(uint8_t maxPoly);
#endif /* CONFIG_CROSSPAD_SAMPLER_MARCEL */

#ifdef __cplusplus
}
#endif
