// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file SamplerEnginePort.hpp
 * @brief The board's side of crosspad::SamplerEngine.
 *
 * Core owns the engine -- voices, residency, the stream window, the pitch.
 * This supplies the three things it will not know about: where bytes come
 * from (the SD port's handle cache), where memory comes from (the engine
 * arena the two sample engines take turns owning) and when the streaming
 * thread runs (audio_bg).
 *
 * It lives beside the prebuilt engine rather than instead of it. Only one of
 * the two holds the arena at a time, and which one is a runtime choice
 * (`SMPL_ENGINE` over CDC), so the same kit can be heard through both on the
 * same board without reflashing.
 */

#include <stddef.h>
#include <stdint.h>

namespace crosspad {
class IAudioNode;
class SamplerEngine;
}

#ifdef __cplusplus
extern "C" {
#endif

/// The mixer channel. Silent, and costs the render nothing, until a kit is in it.
crosspad::IAudioNode *native_sampler_node(void);

/// True while the native engine holds the current kit.
bool native_sampler_active(void);

/// Whether a kit load should land in the native engine. True in this build.
bool native_sampler_preferred(void);
void native_sampler_set_preferred(bool on);

/**
 * @brief Ask for the current kit to be taken into the native engine.
 *
 * Returns at once: the work -- stopping the prebuilt engine, resetting the
 * arena, reading every sample's header and its resident head off the card --
 * runs on a task of its own, because it is seconds long and the caller is
 * usually the CDC main task with a 4 kB stack. Poll native_sampler_state().
 *
 * The kit is copied here, on the asking thread (crosspad::TakeSpec: strings
 * and numbers, no card access), so the worker never reads the kit model the
 * UI frees when a newer kit retires it. Asked for while a take runs, it is
 * taken right after that one (newest wins); never refused for being busy.
 *
 * @return false when there is no kit, when it is a pitched kit (which has its
 *         own engine), or when the copy or the worker could not be had.
 */
bool native_sampler_take_kit(void);

/// 0 idle, 1 loading, 2 ready, 3 failed.
uint8_t native_sampler_state(void);

/// Hand the arena back and let the prebuilt engine have the kit again.
void native_sampler_release(void);

/**
 * @brief Rebuild one pad from the editor's model, in place.
 *
 * What a pad edit does while this engine holds the kit. Turning the pitch knob
 * is the whole point of a realtime step, so a change must not hand the kit
 * back to the engine that cannot do one. Voices already sounding on the pad
 * are left to finish: they carry their own cursor and gains, and cutting them
 * would make a knob turn audible as a click on the note before it.
 *
 * The pad is copied on the asking thread (crosspad::TakePadSpec), as a take's
 * kit is, and rebuilt only into the kit it was copied from.
 *
 * False when this engine does not hold the kit, in which case the caller does
 * what it always did.
 */
bool native_sampler_reload_pad(uint8_t pad);

/**
 * @brief Put one pad of the current kit into the engine, now, on this thread.
 *
 * The kit loader's door: a kit changed while the player keeps playing is
 * swapped this way, pad after pad, and the engine never stops. The pad's new
 * samples are read in beside the old ones, then the pad is pointed at them
 * in one step -- a hit before that plays the old sample, a hit after it the
 * new one, and a voice already sounding plays the sample it started on to
 * its end. The old samples go back once nothing reads them.
 *
 * Not holding the kit yet but preferred -- the first kit after boot -- it
 * takes the kit whole first and waits for that (a take already running is
 * waited for too), rather than let it go into the prebuilt engine and be
 * taken from there after the load. @p mayTake false (an app has the engine)
 * leaves that to the caller.
 *
 * Reads the card: call it from a worker, never from LVGL or the CDC loop.
 * False when this engine does not hold the kit, or the kit is pitched (it
 * has an engine of its own); the caller then does what it always did.
 */
bool native_sampler_swap_pad(uint8_t pad, bool mayTake);

/// Whether a hit on @p pad sounds now: this engine holds the current kit and
/// the pad has a sample in it. No card access, no lock -- the LVGL thread
/// asks it for the pad's LED.
bool native_sampler_pad_ready(uint8_t pad);

/// A pad's step, applied at once: no file read, no pad rebuilt. False when
/// this engine does not hold the kit.
bool native_sampler_set_pad_pitch(uint8_t pad, int semitones);

/// The note path, when native_sampler_active(). @p transpose and @p key: see
/// crosspad::SamplerEngine::noteOn() -- 16 levels of pitch; 0xFF is the pad's
/// own key.
void native_sampler_note_on(uint8_t pad, uint8_t velocity, int8_t transpose = 0, uint8_t key = 0xFF);
void native_sampler_note_off(uint8_t pad, uint8_t key = 0xFF);
/// A finger on @p touch before its velocity: crosspad::SamplerEngine::arm().
/// False when this engine does not hold the kit or there is nothing to arm.
bool native_sampler_arm(uint8_t touch, uint8_t pad, int8_t transpose, uint8_t key);
/// A finger's hit, on the caller's thread: the voice armed for @p touch if
/// it is this hit's, a note-on otherwise.
void native_sampler_play(uint8_t touch, uint8_t pad, uint8_t velocity, int8_t transpose, uint8_t key);
/// The touch on @p touch ended without a hit.
void native_sampler_disarm(uint8_t touch);
void native_sampler_all_notes_off(void);
bool native_sampler_pad_sounding(uint8_t pad);

/// Level and pan of a pad, live. Two calls because that is how they arrive
/// -- the Pad Mixer turns one of them -- and each keeps the other.
void native_sampler_set_pad_volume(uint8_t pad, int volume);
void native_sampler_set_pad_pan(uint8_t pad, int pan);

/// The Pad Mixer's meters: each pad's peak as this engine renders it,
/// measured only while on (it costs the render a pass over each voice).
void native_sampler_set_pad_metering(bool on);
/// A pad's peak since the last call, linear, 1.0 = full scale.
float native_sampler_take_pad_peak(uint8_t pad);

/* ── The pad editor's side ─────────────────────────────────────────────────
 *
 * The editor reached the prebuilt engine directly for these, so making this
 * one the default silenced them. Each returns false / -1 when this engine
 * does not hold the kit, and the caller then does what it always did. */

/// Silence a pad now, whatever its playbackMode says -- what the editor does
/// before pointing the pad at a different file.
void native_sampler_stop_pad(uint8_t pad);

/// Take the pad's playbackMode from the kit model (one-shot, hold, loop).
/// The editor's loop button, which writes the model and then says so.
bool native_sampler_sync_pad_mode(uint8_t pad);

/// Samples this engine could still hold, for the editor's "can another be
/// added" check. -1 when this engine does not hold the kit.
int native_sampler_free_slots(void);

/// A file's header, for the editor's length and rate fields. A sample of the
/// kit answers from the store; any other file reads the card on the calling
/// thread under the loader's lock, waited for 30 ms -- false during a take.
bool native_sampler_describe(const char *path, uint32_t *outFrames,
                             uint8_t *outChannels, uint32_t *outRate);

/**
 * @brief The current kit's sample rates, from the headers the store read when
 *        it loaded them -- no card access.
 *
 * @p samples distinct files the kit's pads play, @p offRate of them not at
 * the engine's rate (each interpolated in every voice: 66-73 us a voice at
 * 48 kHz against 18-21 at 44.1), @p rate the most common of those rates,
 * @p engineRate the render's. False while this engine does not hold the kit,
 * a take is reading it, or the loader holds the store for more than 30 ms.
 */
bool native_sampler_rate_report(uint16_t *samples, uint16_t *offRate, uint32_t *rate, uint32_t *engineRate);

/// Whether a sample this engine holds -- the kit's, or one a voice of the
/// outgoing kit still plays -- reads @p fsPath. True as well when the store
/// could not be looked at within 30 ms: the caller is about to remove the file.
bool native_sampler_reads(const char *fsPath);

/**
 * @brief The voice cap, at runtime.
 *
 * Exists to answer "how many stereo samples can sound at once" with a
 * measurement instead of a guess: the default is 16, so a sweep cannot see
 * past it, and the engine's own ceiling is higher (kMaxSounding). Clamped to
 * what the engine can actually hold; 0 is the default (16). Every take starts
 * from the defaults again. Returns what it ended up as, 0 when this engine
 * does not hold the kit.
 */
uint8_t native_sampler_set_max_voices(uint8_t n);
uint8_t native_sampler_max_voices(void);

/// The render is over its budget (audio_rt's running average): a note takes
/// a sounding voice's place instead of adding one (SamplerEngine::setOverloaded).
void native_sampler_set_overloaded(bool on);

/// The render over its whole block for good: one sounding voice of the
/// kit's engine let go (SamplerEngine::shedOne). False when it had none. The RT task's, between two passes.
bool native_sampler_shed_voice(void);

/// The streaming budget, separately: the ceiling on voices being fed off the
/// card. Raising it is how the wall that limits long samples gets measured.
/// 0 is the default (10). Takes the arena owner lock and the loader's lock
/// (it reserves pool pages), each tried for 20 ms -- a CDC verb never waits
/// out a take; NATIVE_SAMPLER_BUSY when either could not be had (retry).
#define NATIVE_SAMPLER_BUSY 0xFFu
uint8_t native_sampler_set_max_streaming(uint8_t n);
uint8_t native_sampler_max_streaming(void);

/**
 * @brief Voices sounding, how many read the card, and the ceiling that binds.
 *
 * For the status bar's meter. Every pointer is optional. With this engine not
 * holding the kit, all three are left at 0 -- the prebuilt one has slots, not
 * voices, and cannot say which are streaming.
 */
void native_sampler_voice_meter(uint8_t *active, uint8_t *streaming, uint8_t *limit, bool *full);

/// From audio_bg, once a pass. Does nothing while the engine is not current
/// and no audition is in flight.
void native_sampler_service_io(void);

/**
 * @brief Audition a WAV outside the kit (the file browser, SMPL_PREVIEW).
 *
 * @p fsPath is the filesystem path (kitFsPath()). One voice, its own ring of
 * four pool blocks (reserved with every kit), streamed whole from the card on
 * audio_bg and rendered on the "Preview" mixer channel
 * (native_sampler_mix_preview()). The next audition, a stop, an exit from the
 * browser or the next kit load ends it. Only while this engine holds a kit --
 * the arena is a pitched bank's otherwise -- so false then. Cheap, but one
 * thread only: the engine's request is a sequence-locked record with a single
 * writer. That is the LVGL thread (the browser); the CDC verb posts to it.
 */
bool native_sampler_preview(const char *fsPath, uint32_t start, uint32_t end, int volume, int pan);
void native_sampler_preview_stop(void);

/// The start-up sound, from app_main right after engine_arena_init(): the
/// arena is empty and nobody's, so the pool takes the audition's ring there
/// before any kit. The first kit load or pitched bank gives it back.
void native_sampler_boot_sound(const char *fsPath);

/// RT, from the "Preview" channel's node: adds the audition to @p out.
void native_sampler_mix_preview(float *out, uint32_t frames);

/// The Sampler's live scope: this engine's output, mono, newest last.
/// Enabling takes the tap's 32 kB ring once (PSRAM, refused rather than
/// aborted when PSRAM is short); writing it is a copy on the render thread.
void native_sampler_scope_enable(bool on);
uint32_t native_sampler_scope_snapshot(float *out, uint32_t maxSamples, uint32_t *sampleRate);

/// What a borrower must leave in the arena's largest free block while this
/// engine holds it: what the pool may still grow by, and two more heads at
/// this kit's head size (a pad edit that adds layers). See engine_arena_lend().
size_t native_sampler_arena_keep(void);

/// One line for SMPL_ENGINE: what is loaded, what it costs, what it has seen.
void native_sampler_status(char *out, uint32_t cap);

/// SMPL_LOOPS: each pad's Loop state (. idle, q waiting for the bar,
/// P playing, m muted), the launch counters and the bar clock.
void native_sampler_loops(char *out, uint32_t cap);

/// SMPL_STALL: one line per NativeStallClock record -- the slowest span since
/// the records were switched on, the calling task and core, that task's own
/// CPU time, audio_bg's and the core's idle time over it. @p on 1/0 switches
/// the records on/off and clears them; -1 only reports.
void native_sampler_stall_report(char *out, uint32_t cap, int on);

/// Frames a voice of this engine wanted before the streamer had them, since
/// the kit was taken (SMPLENG underrun=).
uint32_t native_sampler_underruns(void);

/// SMPL_PEAK's half: the loudest sample this engine put on its channel
/// (int16 scale) and its longest render call (us) since the last read, both
/// read and cleared (its own pair, so SMPL_ENGINE's rt_max_us is not
/// cleared), and its underruns since the kit was taken.
void native_sampler_peak_take(int32_t *peak, uint32_t *rtMaxUs, uint32_t *underruns);

/// This engine's loudest sample a side since the last read, 0..1 (the song's
/// meter of the kit track it plays). False while it holds no kit.
bool native_sampler_level_take(float *left, float *right);

#ifdef __cplusplus
}

/// Times a span of the caller's for SMPL_STALL (nothing while it is off).
class NativeStallClock {
public:
    static constexpr uint8_t kNote = 0;     ///< the engine's note-on / fire
    static constexpr uint8_t kPadPath = 1;  ///< an STM note through to the engine
    static constexpr uint8_t kRecords = 2;
    explicit NativeStallClock(uint8_t record);
    ~NativeStallClock();
    NativeStallClock(const NativeStallClock&) = delete;
    NativeStallClock& operator=(const NativeStallClock&) = delete;

private:
    uint8_t record_;
    bool on_;
    void* me_ = nullptr;
    void* bg_ = nullptr;
    uint8_t core_ = 0;
    int64_t t0_ = 0;
    uint32_t self0_ = 0, bg0_ = 0, idle0_ = 0;
};

#endif
