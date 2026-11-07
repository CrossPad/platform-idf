# Live ARM: one instrument loaded, the rest frozen, nothing waits

Status: built and measured on branch `feat/one-live-instrument` (on top of
`feat/more-tracks`), 2026-10-28. Core 2.32.0, arrange 0.28.0.

## Intent

Arrange is meant to be a small, fast DAW a player performs with: up to eight
tracks, the armed track's instrument live at full strength, every other track
playing its freeze. A player moves ARM from track to track **while the song
plays** and never waits: the pads play the new track's kit within a second,
nothing in the song drops out or doubles, and the freezes that keep the other
tracks cheap are made in the background.

On `feat/more-tracks` that failed in three ways:

1. ARM to a kit track with a kit of its own never gave the pads its kit while
   the song played: the one second-kit engine moved only with the song stopped.
2. A track with no current freeze and no engine was silent (`PARKED`).
3. The song's ring is mixed ~1.5 s ahead, the sequencer's map changed at once:
   on ARM the track left fell out of the song for 1.5 s and the track armed
   sounded twice.

## Goals

- ARM to any kit track while playing: its kit under the fingers in <= 1 s
  (board clock), at once when it is already loaded.
- No track silent, no track twice, no note on the wrong engine, across any
  sequence of ARM moves and dubs while playing.
- Freezes made while playing, without a stop and without a box on the screen.

## Non-goals

- More than two kit slots; the pads' kit (route 0) and Pitch keep their own
  engines as before.
- Mute and fader latency of frozen and audio tracks (still the ring's ~1.5 s).
- The auto-freeze box over the timeline while stopped.

## Design

### Two kit slots

`AudioSong::kitSlots[2]` replace `kitSlot`: slot 0 is `PlatformServices::
secondKit` on route 1 ("Kit 2"), slot 1 `::thirdKit` on route 3 ("Kit 3";
`PatternSequencer::kRoutes` 4, held bits 4 x 16 = 64). `liveRoute(t)` is the
route of the slot a track holds; `engineRoute(t)` what a render or edit plays
through; the freeze key keeps `seqRoute()` so renders do not depend on slots.

Policy, `crosspad::kitSlotsNext()` (pure, host-tested), every Arrange tick,
playing or stopped:

- the track the fingers play (seq view's, else armed) takes a slot at once:
  free, else a frozen or empty holder, else one whose freeze keeps failing,
  else any -- never the one the fingers just left while another will do;
- a track that is not wanted gives its slot back when it plays nothing, or,
  stopped, when frozen; playing, a frozen holder stays cached (ARM back to it
  is not a load);
- a stale track nobody plays takes a free slot, else a cached one, so it is
  heard and can be frozen or harvested.

Arrange asks the slot's engine for the kit immediately (the song keeper's
300 ms step was a third of the wait).

### Change-over at the ring

`SongEngine::pushSeqMap()` compares which tracks play their freeze
(`Arrangement::frozenMask`) with the last map. For tracks that changed while
playing, `PatternSequencer::setSongMapThen(now, later, ring_.used())`: a track
going to its freeze keeps its old notes, a track coming back waits, until the
song reaches the audio mixed after the change. Other tracks' notes change at
once. `SongSpan::track` says whose a span is.

### Harvest

`ISongEngine::harvestTrack(t)`: the pieces of `t`'s freeze plan
(`freezePlan()`, named by `pieceKey()`) recorded from that kit engine's own
output (`Kit2Node` copies its block; `song_engine_rt_harvest()` applies the
Mixer channel's level and pan, not the song's trim) as the song passes through
each piece in one go, from where the piece keeps audio. The song worker
writes the WAV beside the stream. When every piece is on the card,
`freezeTrack()` finds them all and renders nothing. A jump, a wrap inside a
window, a mute or a full ring drop the piece in hand; ARM on the track, an
edit, its engine changing or a stop call the harvest off. Arrange harvests the
next track a freeze would make while the song plays.

### Memory and card

- `SamplerEngineBase` takes its voices from its owner; slot engines are
  `SamplerEngineWithVoices<12>` (4448 B internal, against 10208): the 9 kB voice
  array in one internal block could not be had on a fragmented heap.
- A slot keeps its engine objects 30 s after it holds nothing, retries no_mem
  with backoff, streams at most 4 of 8 voices, takes at most 600 kB of heads.
- `sd_sector_io` caches single sectors (FATFS directory/FAT reads): every open
  walked `/sdcard/crosspad/kits` again.
- `sequences.json` is not written while the song plays (a write was followed
  by a 0.7 s read of the song's ring).
- The boot update check waits for the kit scan and load (newlib aborted in
  `kit_scan` on a FILE lock, ~1 boot in 5).

## Verification

Host: core `tests/` -- `[slots]`, `[route]`, `[harvest]`, `[songmap]`,
`[scenes]`; all 1197 cases pass.

Board (`tools/`): `hil_live_arm.py` (ARM round four kit tracks under playback
with STM pads: ready time, silent tracks, notes on the right engine),
`hil_live_handover.py` (note counts of both engines around an ARM),
`hil_live_jam.py` (three kit tracks dubbed in turn under playback, then STOP),
plus `hil_track_transitions.py`, `hil_track_capacity.py`,
`hil_torture_song.py`. Numbers: `docs/subsystems/audio-song.md`.

## Since

- Mute/fader of frozen and audio tracks: ~0.3 s (the ring re-mixed from 300 ms
  ahead, core 2.36). A kit track no engine holds plays its stale freeze
  instead of silence (`hil_live_jam` 0 silent). See `docs/subsystems/audio-song.md`.

Later (core 2.33-2.38, Arrange 0.29-0.36): Pitch pad map in the song and its freeze key, export to WAV, ruler and
block gestures, part letters, names, metronome kept, undo during a freeze,
record a block again, insert/delete bars, move/duplicate tracks, hold a
header to mute -- see `docs/subsystems/audio-song.md` ("Arrange as a
performer uses it").

## Open

- A harvest waits for each piece's window to come round: one to two loop
  passes (the stale freeze plays meanwhile).
- Three kit engines at once stream more than the card's sixteen voices only
  if the pads' kit streams eight; heavy kits not measured.
- `hil_track_transitions` `edit` still shows 1-3 sample-engine underruns per
  run under 8 hits/s.
- One of four `hil_live_arm` runs on the final image had five loads over the
  budget (heads 0.8-2.3 s, the card slow that run); the other three were
  <= 0.95 s.
- `hil_track_transitions` `edit`: a `song_io` read of ~0.7 s with no write
  in the window (the scenes no longer write under the song) -- absorbed by the
  ring, cause not found.
- A Pitch instrument built beside the kits can find no PSRAM (`zone_0: no
  memory (389938 B)`, once in a 300-action torture): kit slots cached while
  the song plays hold their heads. Arrange now caches no slot while the
  instrument host is Failed, and the keeper asks for it again later; not
  re-measured under the torture.
- `beat_session`, `beat_to_song`, `song_elements` fail on this card on
  `feat/more-tracks` itself: DRUMS is filed by folder and the card has no
  `/crosspad/drums`.
