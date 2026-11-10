# A kit picked outside Arrange: the player decides what it does to the song

Date: 2026-11-07. Status: design, not built.

## Why

Two things bite a player who leaves Arrange to change the kit.

1. **The kit is silent.** An open song with a Pitch track and no kit track
   builds its instrument in the engine arena (`song_wants_beside()` is false
   without a kit track, `main/instrument_host.cpp`). A kit picked in the Sampler
   then loads into the model, but the native engine drops the take because "a
   pitched bank holds the arena" (`SamplerEnginePort.cpp`, take refusal). The
   Sampler shows the kit, the preview voice plays (it is outside the engine),
   and the pads are silent with no message. Seen on the bench 2026-11-07:
   `SMPL_ENGINE engine=none kit=-1`, `ARENA owner=pitched`,
   `INST owned=1 beside=0`, song "Song 18" = one seq track on route 2 (Pitch),
   armed, no kit track.
2. **The song changes behind the player's back.** A kit loaded while a song is
   open is written into the song at once: `platform_onKitLoadFinished` →
   `song_sounds_kit_loaded()` → `SongSounds::kitLoaded()` →
   `AudioSong::kitHeld(path)`, which sets `song.kit` and the live kit track's
   `kit`. The live track's freeze no longer matches, and back in Arrange it is
   rendered again on the new kit. Sometimes the player wants that (the same
   notes on a new sound), sometimes they wanted a new part on the new kit, and
   the old track is already gone.

## Goals

- A kit the player picks always sounds on the pads.
- A kit picked outside Arrange never changes the open song by itself. On the
  next entry into Arrange the player chooses, every time:
  - **Replace**: the live kit track plays the new kit (today's behaviour:
    `kitHeld()`, the track renders again).
  - **New track**: the old track keeps its kit and its freeze; a new kit track
    with the new kit is added, empty and armed, so the next REC records into it.
- The choice cannot be skipped: no Cancel button, no tap outside, no swipe.
- **Power click on the prompt = nothing happens.** The prompt closes, Arrange
  does not open, the song stays as it was, the decision stays pending, and the
  prompt comes back on the next entry into Arrange.

## Non-goals

- No prompt for a kit picked inside Arrange through its own kit browser
  (`pick_kit_for()` / `kit_picked()` in `crosspad-arrange/src/arrange_tracks.cpp`,
  ASONG `TRACKKIT`, ASONG `SEQTRACK <kit>`): that pick already says which track.
- No prompt for kits the song itself loads (`SongSounds::step()` → `loadKit()`
  when a song opens or its live kit changes).
- No change to how Replace renders or how a new kit track freezes the old live
  track: both already exist and are reused as they are.
- No prompt for a kit picked while editing one kit track: CLIP EDIT on kit
  track N, then the Sequencer and the Sampler (EDIT or the selector) reached
  from there. That pick is track N's, as today (`kitHeld()` on the track CLIP
  EDIT armed and handed over). The context lasts from CLIP EDIT until the
  player is back in Arrange. *(Decided by Mateusz.)*

## Anti-goals (fails even if it "works")

- The song's kit silently reverting to the old one while the player is still in
  the Sampler with the new kit (see Constraints: `SongSounds::step()` reloads
  `song.kit` whenever it differs from the current kit).
- A prompt that blocks the encoder or the power button with no way out, or one
  that appears in apps other than Arrange.
- The Pitch instrument and the kit fighting over the arena in a loop (kit loads,
  instrument re-requested, kit dropped again).

## Behaviour

### Part 1: the picked kit plays

When the player picks a kit while the open song's Pitch instrument holds the
arena, the instrument moves beside the kits instead of keeping the kits out.
The kit takes the arena and the Pitch instrument is rebuilt beside it
(`PitchedLoadOptions::besideKits`). The rule becomes: the song's instrument is
built beside the kits whenever a sampler kit is current or wanted, not only
when the song has a kit track.

If the instrument does not fit beside the kit (PSRAM), the kit wins: the kit
plays, and the Pitch track gets the existing "Pitch sound did not load: …"
toast and retry (`SongSounds::step()`, `instRetry_`). It must not loop.

Pitch Play and the Sequencer in the foreground keep their current exemption
(`song_sounds_start()` timer, `SongSounds::step()`'s `foregroundIs(...)`).

### Part 2: the prompt on the way into Arrange

**Pending state.** A kit load that finishes while a song is open, that the song
did not ask for, and whose path differs from the live kit track's kit, sets a
pending "kit outside Arrange" with that kit's path instead of calling
`kitHeld()`. A later pick replaces the pending path (the newest wins). While it
is pending:

- the song is not written;
- `SongSounds::step()` must not reload `song.kit` over the player's kit (today
  it would, as `kit != have`);
- the kits' engine plays the new kit on the pads; the song's live kit track
  plays its existing freeze -- the old sound -- when the song plays (it is not
  live on the engine any more). *(Decided by Mateusz.)* If it has no freeze
  yet, it is silent until the choice is made. Sequencer edits and takes still
  write the track's scene as today; the freeze then goes stale and the stale
  one plays, as a kit track the engine does not play already does. Nothing
  renders until the choice. *(Builder's call how to express "not live" in the
  engine; see below.)*

**Why it exists: the song is protected, not the pick.** A player may not
remember a project is open (it opens again at boot) and change the kit just to
play; that must never break the project. So the pending state lives in RAM
only: a reboot or opening another song drops it, and the song then loads its
own kit, unchanged, as it does today. The kit picked before the reboot is not
brought back. *(Decided by Mateusz: the point is that the previous project is
not spoiled by an accidental change.)*

A song with **no kit track at all** has nothing to replace: nothing is pending.
The song-level `song.kit` is written as today (no track is touched, nothing
renders), so `SongSounds::step()` keeps the player's kit instead of reloading
or unloading it. *(Claude's call: a kit track appears only when the player adds
one in Arrange.)* The "untouched new song adopts the loaded kit" rule in
`SongSounds::step()` stays.

A song with kit tracks but no live one (`liveKit` not a kit track, e.g. the
live track was deleted) counts as having kit tracks: Replace targets the
lowest-numbered kit track. *(Claude's call.)*

Host loads (CDC `KIT_LOAD`, `crosspad-hil kit push`) count as a pick like any
other. HIL scenarios that load a kit with a song open and then open Arrange
answer the prompt through the new verb (Left to the builder). *(Claude's call.)*

A kit picked through the selector swiped open over Arrange counts as outside:
the selector takes Arrange off the screen (`kit_gate` destroys the running app),
so the prompt comes when Arrange is back. Arrange's own kit picks (a kit track's
kit) set the track's kit before the load and never wait. *(Claude's call.)*

The CLIP EDIT context is core's `ArrangeFocus` (held from CLIP EDIT, or from the
Sequencer adopting the armed track, through EDIT to the Sampler); a pick on
the MODE map or going home already clears it (crosspad-gui `mode_overlay.cpp`).

**The prompt.** Opening Arrange while a decision is pending (from the launcher,
the pad map, MODE, `APP_OPEN Arrange`, or the app history) shows a modal
before Arrange is built. Natural seam: the orchestrator's pre-launch hook
(`AppOrchestrator` `config.pre_launch`, today `kit_gate_pre_launch`), which can
refuse the launch.

Contents (English, as the rest of the UI):

- Title: the new kit's name.
- Line: "Track N plays <old kit>. What should <new kit> do?"
- Button **Replace kit on track N**.
- Button **New track**: greyed out with "No room: all tracks or all eight
  scenes are in use." when `freeTrack()` or a free scene is missing (the same
  condition as `make_seq_track()`).
- Power click: close, do not open Arrange, keep it pending (Goals). The player
  stays on the screen they were on. `AppOrchestrator::onAppSelected()` destroys
  the previous app before `pre_launch` runs today, so the check has to happen
  before that destroy (or the previous app is restarted); an empty container
  or a jump home is a bug. *(Claude's call, from "nothing happens".)*

Reachable by touch and encoder; the encoder group is the modal's while it is up.

**Replace**: exactly today's effect: `AudioSong::kitHeld(path)`, commit, open
Arrange. The live track renders again as it does now.

**New track**: through the same path Arrange's ADD TRACK → KITS uses
(`make_seq_track(SeqSource::Kit, path)` + `seq_track_made()`): the song stops if
it plays, the old live track is frozen first, the new track gets the kit, is
armed, and becomes the live kit track; open Arrange with the new track selected.
That code is static in `arrange_tracks.cpp` today; the builder decides whether
it moves to core or is reached through Arrange on its first frame.

## Constraints

- `SongSounds::kitLoaded()` already ignores a load the song asked for
  (`song.kit != lastKit_`, `song.kit == cur->path`): the same test tells "the
  player picked it" from "the song loaded it". Arrange's own kit picks set the
  track's kit before the load, so they also pass as "the song's".
- Core owns `SongSounds` and `AudioSong` (portable, host tests in
  `components/crosspad-core/tests/`); the prompt is crosspad-gui or app UI; the
  pre-launch hook is installed in `main/gui/gui.cpp`. The PC simulator links the
  same core and must keep building.
- LVGL rules from CLAUDE.md: the modal is built on the LVGL thread; a kit-load
  callback (`kit_load` worker) must not touch LVGL directly.
- Version bumps: core minor (new state/API), crosspad-gui or crosspad-arrange
  minor, platform-idf floors in `main/include/crosspad_deps.h` if they move.
- Docs move with the code: `docs/subsystems/audio-song.md` (kit tracks, live
  kit) and `docs/subsystems/pitched-instrument.md` (beside rule).

## Left to the builder

- Where the pending state lives (`SongSounds` is the natural owner) and how it
  is exposed (a query + accept/replace/new calls).
- How the live track is "not live" while pending (e.g. engine treats it as
  frozen) without writing the song.
- Where the modal is implemented (crosspad-gui component vs Arrange) and whether
  `make_seq_track()` moves to core.
- A CDC/HIL verb to read and answer the pending choice (`ASONG` line or a new
  verb), so a scenario can test it without a finger.
- Tests: core host tests for the pending state and the beside rule; a HIL
  scenario: open a song with a kit track, leave Arrange, load another kit,
  enter Arrange, check the prompt, answer each way, check `ASONG`/`SMPL_ENGINE`.
