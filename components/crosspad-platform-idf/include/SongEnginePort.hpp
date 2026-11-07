// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

class AudioMixerEngine;

namespace crosspad {

class AudioSong;

/// Bring up the audio song engine: register its playback node on the mixer as
/// "Song" (speakers and phones; a bounce records the pads alone), start
/// its card worker, and publish it as PlatformServices::songEngine. @p kitA
/// and @p kitB are the kit's engines' channels and @p pitched the pitched
/// one's: an Arrange seq track's fader goes on the ones its notes play
/// through while the song plays.
void song_engine_init(AudioMixerEngine& mixer, uint8_t kitA, uint8_t kitB, uint8_t pitched);

class IdfAudioModule;
/// The pads tap a bounce records (IdfAudioModule::setPadChannels) and its
/// three channels -- the kit's engine, the Pitch instrument, the kit's second
/// engine (the prebuilt streamer's build only): a freeze takes the one channel
/// of its track's engine there and puts the three back after.
void song_engine_set_pads_tap(IdfAudioModule& module, uint8_t kit, uint8_t pitched, uint8_t kitB);

/// Who hears every song that opens (or is made), on the song engine's worker
/// with the song's lock held: copy what is needed and return. One listener.
void song_engine_set_open_handler(void (*fn)(const AudioSong& song));

/// Bounce pieces that went through stdio because the board's DMA stage was
/// not there, since boot (ASONG io_fb=).
uint32_t song_engine_io_fallbacks();

/// Before engine_arena_reset(): the song's rings may live in the arena's
/// slack. Stops playback, cancels a bounce, waits until both are given back.
void song_engine_release_arena();

/// Called by the RT render once per block with the pads tap (Sampler and
/// Pitched as mixed, IdfAudioModule::setPadChannels). Does nothing
/// unless a bounce is armed; then it hands the block to the bounce writer.
void song_engine_rt_capture(const float* padsBus, uint32_t frames);
/// RT: a bounce or a freeze wants the pads tap this block.
bool song_engine_capturing();
/// RT: the capture's ring is past half full (SongEngine::captureBacklogged()).
bool song_engine_capture_backlogged();
/// Whether a freeze may run off the clock (default yes); off, it runs in real
/// time -- ASONG OFFLINE, to hold one against the other.
void song_engine_set_offline(bool allowed);

} // namespace crosspad
