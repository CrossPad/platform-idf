# Audio song — bounce, arrange, play back

The sequencer records the pads as a pattern; the audio song turns patterns
into audio and lays them out as a song. Three apps sit on it — **Sequencer**
(`components/crosspad-sequencer`, repo `crosspad-pattern-seq`) bounces a
scene, **Arrange** (`crosspad-arrange`) lays clips on up to eight tracks, **Song**
(`crosspad-song`) arranges *scenes* (not audio) as blocks with repeats —
plus **Pad Mixer** (`crosspad-pad-mixer`) for per-pad levels.

## Where things live

| Piece | Where | Portable |
|---|---|---|
| Song model (`AudioSong`: tracks, clips, bpm, metre, load/save) | core `crosspad/song/AudioSong.hpp` | yes |
| The interface apps use (`ISongEngine`) | core `crosspad/song/ISongEngine.hpp`, `PlatformServices::songEngine` | yes |
| WAV writer | core `crosspad/audio/WavWriter.hpp` | yes |
| Scene arrangement (song blocks, song mode) | core `PatternSequencer`, `sequences.json` under `"song"` | yes |
| The engine (rings, clip reader, bounce, playback, worker) | core `crosspad/song/SongEngine.hpp`, behind `ISongEnginePlatform` | yes |
| The board's hooks (`song_io` task, arena/PSRAM rings, `/sdcard/SONGS`, NVS `song/current`, USB rec bus) | `components/crosspad-platform-idf/SongEnginePort.cpp` | S3 only (`CROSSPAD_HAS_SONG_ENGINE`) |
| The PC's hooks (`std::thread`, `malloc`, `<sdcard_path>/SONGS`, `SONGS/.current`, "Rec (pads)" bus) | crosspad-pc `src/song/PcSongEnginePort.cpp` | PC |
| Per-pad level/pan for any engine | core `IPadMix`, set by the Sampler (`sampler_pad_mix()`) | yes |

The PC simulator runs the same engine (since core 1.24.0): its
mixer renders a third bus nothing plays -- sampler and pitched only -- and
`PcAudioModule` hands it to `SongEngine::rtCapture()` after every render. On
PC the loop recorder hears played pads through `PadManager::setInputTap()`,
since its pads come in by more ways than the board's inputs.

`ISongEnginePlatform`, what a platform provides:

| Hook | Board | PC |
|---|---|---|
| `startWorker` / `joinWorker` | FreeRTOS `song_io`, PSRAM stack, prio 5, core 0 / never joined | `std::thread` / joined |
| `wake` / `waitWake` / `sleepMs` / `nowUs` | binary semaphore, `vTaskDelay`, `esp_timer` | condition variable, `sleep_for`, `steady_clock` |
| `allocRingSegment` / `freeRingSegment` | the engine arena's slack, else PSRAM above the 128 kB UI reserve | `malloc` |
| `allocScratch` / `freeMemoryBytes` | PSRAM | `malloc` |
| `storageReady` / `yieldIo` | SD handle + internal RAM for an open / give way to the streamer | `sdcard_path` set / nothing |
| `songsRoot` / `loadCurrent` / `saveCurrent` | `/sdcard/SONGS`, NVS `song`/`current` | `<sdcard_path>/SONGS`, `SONGS/.current` |
| `beginPadsCapture` / `endPadsCapture` | nothing: the bounce records the pads tap, not a bus | "Rec (pads)" bus at unity, unmuted; restored |

## On the card

```
/sdcard/SONGS/<project>/song.json
/sdcard/SONGS/<project>/sequences.json
/sdcard/SONGS/<project>/clips/<kit>_S<scene>_<nn>.wav
```

The sequencer's scenes are the song's: `<project>/sequences.json`,
read when the song opens and written back when they change
(`SequenceStorePort.cpp`), so opening another song opens its grooves and a new
song starts with none. The switch waits while the sequencer runs. Changes not on
the card yet land in the last song's folder first, unless that folder went with
a delete or a rename. A kit change keeps the groove and plays it on the new kit's
pads, tempo included. Older firmware kept one `/sdcard/crosspad/sequences.json`
for every song; a song from then with no file of its own gets a copy of that
one: the song open at the first boot (marked by `/sdcard/crosspad/sequences.adopted`), and
any song with a seq track. Every other song starts empty. A board that never had
the shared file adopts its booting kit's, once. `SEQ` reports `file=`;
`crosspad-hil run seq_kit_change`.

`song.json` is written beside itself (`.tmp`) and swapped in with the old one
kept as `.bak` until the new one is in place; `AudioSong::load()` falls back to
`.tmp`, then `.bak`. A file that does not parse, or was written by newer
firmware (`version` above `AudioSong::kVersion`, 5), is **Unreadable**: the engine does not open it and
nothing saves over it. The open project's name is in NVS `song/current`.

Deleting (`requestDelete()`, Arrange → Songs → Edit → Delete song, asked
once more) removes the open project's folder -- `song.json` and every clip --
and opens the first project left, or a new `Song 1`. The engine drops the song
from memory and cancels a queued save before the first file goes, so nothing
writes the folder back; it only removes a folder straight under the songs root
whose name is the song's.

## The song's sounds: its kit and its instrument

A song names what it is played with, as a DAW's project keeps its instruments
(core 1.60). `"kit"` in `song.json` is the kit's logical path
(`KitInfo::path`, `/crosspad/kits/<folder>/kit.json`); `"instrument"` is Pitch
Play's instrument, from the card's root, or `pitch/instrument.json` relative to
the song's folder when it lives there (a renamed song takes it along). Neither
is part of undo. Each is written only when set, so older firmware reads the
file as before. The board side is `main/song_sounds.cpp`.

- **Opening a song brings the board to it.** The worker reports every open
  through `ISongEnginePlatform::songOpened()`. A 300 ms LVGL timer then takes
  one step at a time, and waits while the boot's kit scan, a kit load or the
  instrument host is busy, because each of them changes what the next step
  sees.
- **The song's kit.** The kit is loaded by path (`findKitIdByPath()`; kit ids
  are not stable). A kit that is already loaded is not loaded again.
- **A song with no kit leaves none.** The native sampler and a pitched kit are
  released, the arena is reset, the prebuilt streamer (when built) is
  restarted empty, and
  `IKitManager::clearCurrentKit()` runs: the state of a boot with no kit.
- **A kit missing from the card** gets a toast once per open and is then
  treated as no kit. Neither kind of unload happens with the Sampler in front,
  because it reads the kit it shows.
- **A Pitch track** (`AudioSong::hasPitchTrack()`) with an instrument opens that
  instrument in the instrument host. It is published in `InstrumentPlayState`,
  where Arrange, the Sequencer and the Recorder look. A song with no instrument
  gives the host's engine back, which reloads the kit, unless Pitch Play or the
  Sequencer is in front. The kit and the instrument still take turns on the one
  arena.
- **What is picked is written in.** When a kit load finishes
  (`platform_onKitLoadFinished`), the kit goes into the open song, followed by a
  commit. An instrument the host opens, builds or saves while the song has a
  Pitch track goes in too.
- **An instrument built from a sample and never saved** is saved into
  `<song>/pitch/instrument.json` first, with its samples beside it
  (`instrument_host.cpp`, `keep_in_song()`), so the song never names a file
  that is not there. Browsing samples in a song without a Pitch track writes
  nothing.
- **`doOpen()` writes the open song's pending save before it reads the next
  one.** Otherwise a kit picked within `kSaveDelayMs` of opening another
  song would be lost, and so would any edit made in that window.

- **What the Pitch pads play is the song's** (core 2.34,
  `AudioSong::pitchPads`, `"pitch_pads"` in `song.json`): Pitch Play's key,
  scale, octave, transpose, whether the chord list is on, and the notes those
  put on each pad. A Pitch take is pad numbers, so without this a song opened
  after a restart played its bass at Pitch Play's default octave and froze it
  there. SongSounds puts the song's map onto `InstrumentPlayState` as the song
  opens; once the song settled, a map Pitch Play publishes is written into it
  (`keepPitchPads()`). Pitch Play takes up a map someone else published when it
  opens (`InstrumentPlayState::transpose`, `chordsOn`). The map's notes are in a
  Pitch track's freeze key: a new octave is a new freeze.
- **An open while the song's sounds load is not kept** (core 2.33.1): Arrange's
  sound keeper, still on the last song's instrument path, can ask for it before
  SongSounds asks for this song's, and the host would write the last song's
  instrument into this one. `keepInstrument()` keeps a plain
  open only once `SongSounds::pending()` is false; the keeper asks for nothing
  while it is true.

When testing with `ASONG OPEN`, a kit picked by hand on the board lands in
whichever song is open at that moment; `KIT_SWAP_LOG` keeps the loads, and the
console's `song_sounds:` lines say which were the song's.

## Threads

| Thread | Does | Never |
|---|---|---|
| audio RT | `mix()` plays the song ring into the "Song" mixer channel; `rtCapture()` copies the pads tap into the bounce ring | allocate, lock, log, touch the card |
| `song_io` (prio 5, core 0, PSRAM stack, created once) | every card operation: open, list projects, list clips, save, fill the song ring, write the bounce ring; allocates and frees both rings | run on anyone else's behalf synchronously |
| LVGL / CDC | edits `song()` under `songMutex()`, then `commit()`; asks for everything else | wait for the card |

Every `ISongEngine` call that needs the card is a **request**: it returns at
once, and the answer appears in `openState()`, `projects()`/`clips()` with
`listGeneration()`, `playing()`, `bounceStatus()`. Apps poll these from their
UI timer.

**Ring ownership.** Only `song_io` allocates and frees `ring_` (playback) and
`bRing_` (bounce). Before freeing, it publishes the state that keeps RT out
(`play_ = Idle`, `bArmed_ = false`) and waits on `rtGen_`: RT increments it on
entry to and exit from `mix()`/`rtCapture()`, so an odd value means a block is
inside, so a Play pressed right after Stop cannot race the free.

## Budgets

- **PSRAM.** Rings come in 16 kB segments (the largest free block is small
  after a few apps). They take the engine arena's slack first. With a pitched
  bank owning the arena they take it only where 512 kB stays free after them,
  because the next instrument is built beside the one playing. On the heap a ring never
  takes the last `kUiReserveBytes` (128 kB). LVGL aborts on a failed
  allocation; a ring only gets shorter. Bounce ring
  32–128 kB, song ring up to 256 kB (~1.5 s ahead, down to the 4096-frame
  prime where memory is short), and while the song plays a 16 kB read-ahead
  per track (core 2.31, `ClipReader::setAhead()`, past the ring's minimum: none
  where memory is short). A bounce also drops LVGL's image cache first.
- **FAT descriptors** (`max_files` 20, shared with the sample player's cache,
  the streamer, `kit.json` and the waveform loader). Playback holds one file
  per track *only while that track has a block under the chunk being mixed*;
  a refused open is counted in `openFailures()`.
- **Image size.** The four apps plus this engine grew `.ext_ram.dummy` by one
  64 kB page (2 162 656 → 2 228 192 B on 2026-09-26); see the PSRAM section
  of `CLAUDE.md`.

## How many tracks

Measured on a v2 board (2026-10-27) with `tools/hil_track_capacity.py` (a song built a
track at a time, played 30 s at each count with chords on the pads) and
`tools/hil_track_transitions.py` (CAP_MIXED, eight tracks, pads struck from
the STM at 8 hits/s while a change is made, `--tracks N` mutes the audio tracks
past N -- a muted track is not read).

- **Playing, nothing changed: eight tracks are safe.** Eight audio tracks of
  eight different recordings, and Drums live + second kit + Pitch + five audio
  tracks: no underrun of the song, the engine or the second kit, render 466 and
  530 us average a block of 1451, the song ring never below half.
- **Changes under playback are not yet safe at any count.** A kit loaded, the
  second kit's track given another kit, blocks copied or moved, a PUNCH: runs
  of 6 of each ended clean at 4 tracks once and with 50--900 song underruns
  another time, on the same firmware. What fills the card: the sampler's
  streamer (`audio_bg`) 50--60 % of its time at 8 hits/s, the song ~25 % after
  the read-ahead (~40 % before it), a kit load 3--4 s per six loads; single
  reads stall up to 860 ms. Core 0 sits at 0 % idle while Arrange draws a
  playing song (LVGL 35--60 %), so the task watchdog prints IDLE0 backtraces
  from its ISR every 5 s, ~90 ms each at 115200 baud. With six tracks or more
  and two kits plus a Pitch instrument, the sampler's pool could not grow
  (thousands of `norings`) and a Pitch instrument did not fit beside the kits
  (`no memory` with 1.2 MB free: the beside build keeps 1.5 MB).
- What keeps the card's time for the song: the read-ahead and
  `streamAheadFrames()` (core 2.31), a kit load's 64 kB heads read in 16 kB
  pieces that wait for the song's ring (`sample_player_port.cpp`), the song
  ring (up to 256 kB), the card's bounce buffer of 16 sectors
  (`sd_sector_io.cpp`, 8 kB internal RAM), a take that does not wait for the
  song's lock, a freeze that gives way once.
  ASONG_DBG's `under_at` says what the worker was doing at each underrun.
- **The card caches single sectors** (`sd_sector_io.cpp`,
  [kit-swap.md](kit-swap.md)): opening a file walks `/sdcard/crosspad/kits`
  (~190 kits, dozens of directory sectors) a sector a command, for every
  reader. `hil_track_transitions.py --reps 5` on CAP_MIXED, the same
  firmware, A/B: with the cache two runs of three clean in every kind
  (kit_swap, arm_kit2, trackkit, edit, punch), longest read 25-73 ms, opens
  4-8 ms; without it `edit` 130 song and 5 engine underruns, `punch` 4, reads
  to 135 ms, opens to 23 ms. The third run with it: `edit` 154 song and 13
  engine underruns, behind one read by the sample streamer of 678 ms and a
  song read of 747 ms, right after writes by a task SD_TRACE files under
  `other` -- the card itself busy after a write, not a queue. `SD_CACHE 0|1`
  for the A/B. Edits under playback stay open.

## Bounce

`bounceScene(scene, track)`: stops playback, switches count-in off, selects
the scene, starts the pattern, and records the pads tap from the block the
clock starts in for exactly the scene's length. Count-in and the active scene
go back afterwards; the transport stays stopped. The clip lands after the
last block of `track`; an empty song adopts the scene's tempo, and a clip at
another tempo is kept with a warning (no time-stretch).

The pads tap is the Sampler and Pitched channels as the mix has them -- fader,
pan, mute, solo -- summed after the render (`AudioMixerEngine::renderTap`,
`IdfAudioModule::setPadChannels`), whatever each is sent to. Not the USB rec
bus: the Mixer can send the inputs there (MONITOR IN, the STREAM preset), and
a mic on IN1 would end up in every bounce.

The take goes to the card in pieces that end on 4 kB of the file -- the first
is 1013 frames, filling up the 44 B header's sector, then 1024 -- through
`ISongEnginePlatform::writeIo`. The engine's buffer is in PSRAM, which the
S3's SDMMC cannot DMA from: through stdio every sector is its own CMD24 plus a
read of the sector a piece ends in, and under a streaming kit the drain falls
to ~0.63x real time ("card too slow"). The board's writeIo copies a piece to
its 4 kB DMA stage and writes the descriptor -- one CMD25. The stage is
shared with the streamer, which (prio 18, same core) would give it back and
take it again before `song_io` ever ran, so a piece waiting for it is urgent,
and the streamer steps aside a tick after its own piece. A failed `fflush` before a
piece fails the piece (a card error), it is not an `io_fb`. `ASONG` keeps how
the last take kept up: `chunks`, `chunk_avg_us`, `chunk_max_us`, `drain_kbps`
(the bus's own 176 while the card keeps up; what the card could take is
4096000 / `chunk_avg_us`), and `io_fb` (pieces through stdio, since boot).
`seq_store` does not write `sequences.json` while a bounce runs. In the
USB-audio profile the request is SysEx `F0 7D 1D 0C scene track F7` (the
console logs `BOUNCE s t started|refused`), read back with `ASONG` in CDC.

## Seq blocks with notes of their own, and takes onto them

A seq block holds its own notes (`SongClip::notes`, a `SeqPattern`: bars and
`SeqNote`s in ticks, `kSeqPpq` 96 to the quarter, sorted). A pattern is never
changed once made, only replaced. Every copy of a block shares one: a DUP, a cut's
two halves, a region pasted, every undo step. An edit (a dub, the seq view) puts
a new one on the block it was made on, so a copy is a clip of its own from its
first edit and costs nothing until then. In song.json each block's notes are one
string, `"notes": {"bars": 2, "ev": "0,1,100 96,3,90,48 …"}`
(`tick,pad,vel[,len[,perf]]`), not an array of arrays. ArduinoJson keeps every
value as a node, and a few thousand notes were hundreds of kB of PSRAM while the
song was read or written. A song with such blocks is written as `version` 2.
Older firmware then calls it Unreadable rather than open it and drop the notes;
a song without them stays 1.

A block with no notes plays its track's scene. Such
blocks get the scene's notes as their own (`AudioSong::adoptSceneNotes()`)
before a take or the seq view changes the track. A track that played its scene
over the whole song gets it as one block over the song.

**Playback.** The song map's spans carry each block's notes as samples
(`PatternSequencer::SongSpan::notes`, `loop`). They are converted once per
pattern and tempo (`songNotes()`, a cache of 32), so a fader's commit converts
nothing. `fireSong()` plays them round their own length, the swing put on as for
a scene -- the track's own: swing is a scene's (core 2.43,
`PatternSequencer::setSceneSwing()`, kept per scene in `sequences.json`), each
seq track plays a scene of its own, and a span carries it
(`SongSpan::swingScene`). A take onto the song comes back off the armed track's
swing, and a freeze is made again when it changes (`SceneLookup::swing`,
`pieceKey()`; straight is left out of both keys). SWING in the seq view sets
the track's. A song with only seq blocks is as long as they are: the "at least one
pass of the scene" length is only for a track that plays its scene everywhere.

**Takes** (`ISongEngine::recordSeq(fromBar, Record|Overdub, beats)`, Arrange's
REC and DUB on the top bar through `SongTransport`, which is
`crosspad::songTransport()`), snapped to the Sequencer's grid -- Q on Arrange's
context row, between SYNC and LOOP, sets it there too (Arrange 0.42). The song plays from `fromBar` after `beats` of
count-in, the pattern's PRE. The sequencer is locked to it, and
`PatternSequencer::beginSongTake()` keeps every hit at the song position it was
played at. That position is the map's `mapPos_` plus how far the clock moved
since the last poll, and a hit stamped just before a poll reads as a hair behind,
not a loop ahead. Hits are quantised and unswung as a scene's take is, never
wrapped, with their lengths. Nothing goes into a scene. The worker moves them out
every pass (`kTakeRoom` 256 between two). The take ends at STOP or
`seqPunch(None)`:

- **REC** lays blocks of its own notes from `fromBar` to the end of the bar its
  last note is in, wherever STOP was pressed: a note on the last
  quarter of bar four is a four-bar sequence, even with the stop in bar five or
  later. The seq blocks under it go (`AudioSong::putSeqTake()`):
  one reaching in is cut at the start, one reaching past plays on from the end.
  A REC shorter than the loop it was played in goes round to the loop's end:
  the region it was taken round, or -- the song looping as a
  whole -- the song's end over bars the track has free. It is one block whose
  pattern is the shortest that holds the notes and divides the loop, so one bar
  played into a four-bar loop is that bar four times, not a bar and then
  silence; the next laps dub into that pattern. The seq view's x2 lengthens
  such a block once the notes outgrow it.
  A REC is not heard under itself (`setSongMapMuted()`). Nothing played, nothing
  changes; neither do hits that all came before the bar it started on (in the
  count-in): they lay no empty block over what is there.
- **A take is laid in blocks the seq view can open**
  (`AudioSong::putSeqTakeNotes()`): one per `kSeqBlockBars` (8)
  bars from where it started, halved down to a bar while more than
  `kSeqBlockNotes` (192) notes fall in one. These are a scene's
  `PatternSequencer::kMaxBars` and `kMaxEvents` (a `static_assert` keeps them
  equal), so CLIP EDIT opens every one (`openSeqEdit()` refuses a block longer
  than a scene). A note held over the line between two blocks ends on it.
- **DUB** adds its notes to the blocks they fell under, each a new pattern (its
  copies keep theirs), at the bar of the block's notes they sound on
  (`dubSeqNotes()`). Notes under no block become blocks, whole bars from the
  first to the last, never over the next block, cut as a REC is. A DUB into a
  block can still take it past 192 notes, and the seq view then refuses it.
- **The seq view logs what it does** (`SongEngine` tag): which block it opened
  and how many notes went into the scene, a block it refused as too big, and
  every write that leaves a block with fewer notes than it had. That last line
  is the trace of notes going missing.
- **LOOP over a region**: the take starts at the region and goes round it. Each
  lap is a pass (`songTakeLaps()`; a note held over the wrap ends there). A REC's
  first pass with notes replaces the region, and every pass after it is dubbed
  in. Each lands at the wrap, so the next lap plays it.
- **LOOP on, no region** (core 2.26): a take that starts inside a song with
  something in it goes round the whole song as a region's does -- laps, the
  first pass with notes replacing to the bar of its last note (not the rest of
  the song), the next dubbed in. Punched in on the last beat of a four-bar
  groove, a part played from bar one lands on the groove and the song stays
  four bars.
- **Past the song's end** -- LOOP off, a take from a bar past the end, or a new
  song's first take -- the song runs on with the take (`kTakeHorizonBars` 999,
  in `flatten()` while it is open) instead of wrapping to bar one. After it the
  song ends with its blocks again. A take punched out while the song plays on
  is heard from the next pass: the sequencer's map wraps a position past the
  song's new end (`PatternSequencer::fireSong()`, core 2.26), as the ring does.
- **Punch**: `seqPunch(Record|Overdub)` while the song plays with the sequencer
  locked to it (`seqPunchable()`) starts one from the bar it is in.
  `seqPunch(None)` lands it and plays on. PLAY in a take does the same, as the
  pattern's PLAY does.
- A song with no seq track gets one, playing the kit, on the first free track,
  with a scene of its own for the seq view. A track made for the take does not
  adopt the scene that is up.

A take is one undo step. Undo is refused while one runs. Host tests: `[notes]`,
`[songtake]`.

**The seq view of a block** (`openSeqEdit(track, bar, scene)`, `syncSeqEdit()`,
`closeSeqEdit()`). The Sequencer opened on an Arrange track edits one block:
CLIP EDIT's picked block, or the block under Arrange's playhead (from MODE:
`songTransport().startBar()`). Its notes are put in the track's scene and looped
there alone. What changes there goes back as the block's own pattern on the
screen's next tick; `PatternSequencer::editGeneration()` says whether anything
did, and a take still recording is handed over when it closes. A block as long as
its notes follows x2 and /2, never over the next block. On a bar with no block,
the first notes made become one there. A block longer than a scene (8 bars, 192
events) is not opened. Pitch Play on a Pitch track claims the song transport, so
its REC is Arrange's. Host tests: `[seqedit]`.

## Kit tracks, a kit each, on one kits' engine

A song may hold any number of kit tracks, each with a kit of its own, and
**a Pitch track beside them** (core 2.6) -- Drums, Slice, Pitch and
audio tracks in one song. The Pitch track's instrument is built beside the
kits' engine (`PitchedLoadOptions::besideKits`: the general PSRAM heap, not the
arena, so a kit take's arena reset leaves it), and the instrument host keeps it
there while the song has a kit track (`instrument_host_beside()`). A pitched
kit loaded from the kit browser still takes the arena alone. Which engine a seq
track plays through is its **route** (`AudioSong::seqRoute()`):

| Route | Track | Plays through |
|---|---|---|
| 0 | every kit track, in turn | the kits' engine -- the native engine, "Sampler" -- holding the live kit track's kit (`AudioSong::liveKit`, whose kit is `AudioSong::kit`: the one the pads and the Sampler play) |
| 2 | the Pitch track | Pitch Play's instrument (`SeqTrackSound::playPitch()` from the sequencer's sink); its scene over the whole song is laid as route-2 notes, the clock's scene being route 0's |

Routes 1 and 3 (the second and third kit engines, "Kit 2"/"Kit 3", of older
cores) are gone since core 2.42: one engine, sixteen voices, and the kit a
track plays is the one its seq view's EDIT edits.

**Up to eight tracks** (`AudioSong::kTracks`; four on screen, the rest scroll
under them). Every seq track but the armed one is frozen, so more tracks cost
card streams, not engines: one Pitch track (route 2) and any number of kit
tracks, each on a scene of its own (eight scenes, `kSeqScenes`). The kit tracks
take turns on the one kits' engine: `AudioSong::liveKit` names the track it
plays now and its kit is the song's (`setLiveKit()`); `liveRoute()` is -1 for
the others, which play their freeze and are `PARKED` (silent) until they have
one. Arrange gives the engine to the armed kit track (`live_kit()` in
`arrange_freeze.cpp`) only once the track it leaves is frozen -- that track's
kit is the one loaded, so it is frozen first (the song stopped, Arrange up)
and heard from its freeze right after; then the new kit loads. A new kit track
stops the song for that; an ARM while it plays waits for the stop; the note
says which. Not waited for: a holder with nothing to freeze, whose kit is not
in the engine, or whose freeze failed. CLIP EDIT on a kit track arms it and
hands it the engine at once. With the armed track live and another kit track still to
freeze (a tempo turned, a kit saved, a song from before), the engine goes to
that one while the song is stopped, Arrange is up and the player has let the
board be for `kIdleBakeMs` (no pad, touch or encoder; `player_away()`), it is
frozen, and the engine comes back. It stays on the track it went to until that
track's kit is in (at most 30 s, `kVisitLoadMs`, then the track's retry
backoff), so the armed track does not take it back before the kit arrives
(arrange 0.42.1). Under the fingers it stays with the armed track, so kits are
not loaded one after another under the player. Not saved: a load gives it to the armed kit track, else the
first one, and the song's kit becomes that track's. A song with more than four
tracks, or more than one kit track with a kit, is `version` 6. `ASONG_TRK`
lists every track's route, live route and freeze, the armed track and
`live_kit=`. Eight, not sixteen, because the worker reads every sounding track
from the card at once (176 kB/s each, one descriptor each, from the mount's 32
shared with the sampler).

A kit track always has its kit (`SongTrack::kit`): `addSeqTrack()` gives it the
one named, or the song's, and the first kit track becomes the live one. A song
from before core 2.42 opens with the song's kit on the track that played it
(its `kit` was empty) and the second kit's on its own track. Every new seq
track plays only under its blocks (`seqBlocks`) and starts with none: an empty
lane until a take or the seq view gives it notes. A file with two seqs or a
track's own kit is `version` 3; one that claims more than one Pitch track
keeps the first. A track's kit is part of an undo step; the song's is not.

- **The sequencer routes every note.** `SongEngine::flatten()` lays every seq
  track on the one song map, each span with its track's route
  (`PatternSequencer::SongSpan::route`); the board's sink is a `RoutedSink` and
  sends route 2 to the instrument, route 0 through the pads. A kit track that
  is not live has no span: its freeze plays instead.
- **ARM** (`AudioSong::armed`, the dot after S and M on a seq track's header in
  Arrange, which only shows it -- a touch on the header opens the track's
  menu, where ARM, MUTE and SOLO are keys): REC and DUB take onto the armed track (`armedSeqTrack()`: the
  armed one, else the first seq track), the REC's own mute is only that
  track's route (`setSongRoutesMuted()`), and the pads play its kit once the
  kits' engine holds it. A new seq track is armed.
- **REC onto an audio track** (core `ArrangeAudioTake`, driven by
  `SongTransport`): ARM may name an audio track (`armedAudioTrack()`), and REC
  then records that track's input (`SongTrack::input`, `"input"` in song.json:
  `mic` -- not written --, `line`, `mic+line`, `usb`, `pads`) instead of the
  pads. Stopped, it is the sample tools' take lined up with the song
  (`startRecordingWithSong`, after the count-in, the codecs' delay taken off),
  from where PLAY would start; with the song playing, from its next bar line;
  in a song with nothing in it yet, a take on its own from the playhead's bar.
  REC, STOP or PLAY ends it, and Arrange's timer (`poll()`) lays it on the
  track as one block from the bar it went in at (`requestImport`; blocks that
  start inside it go, one reaching in is cut). The WAV stays in the
  recordings folder. There is no DUB onto audio. ADD TRACK > AUDIO makes an
  empty audio track, armed, and opens nothing; a long press on an audio
  track's header picks its input (and arms it), with MIX in the same sheet.
  The playhead is red while it records. With LOOP on the song goes round
  under the take (its region, or the whole song) and the take is cut into its
  passes: every complete one -- the first from the bar it went in at to the
  loop's end, each after it the whole loop -- is imported as a clip of its
  own, in turn, so the last is on the track and the others are in the song's
  clips (EDIT, a clip from the library). What was sung after the last
  complete pass is dropped; stopped before the first got round, the take is
  laid as it is.
- **The seq view of a kit block** (CLIP EDIT) plays its scene on route 0 with
  the track live; its EDIT opens the Sampler on the track's kit, or says it is
  still loading. A kit loaded there is the live track's
  (`SongSounds::kitLoaded()` -> `AudioSong::kitHeld()`, while core's
  `ArrangeFocus` names the track; a pick on the MODE map or going home clears it). Opened from MODE, the
  Sequencer is the armed kit track.
- **A kit loaded anywhere else** while a song with kit tracks is open is the
  pads', not yet the song's: the track it would replace keeps its kit and plays
  its freeze (`AudioSong::liveKit` -1), the song is not written, and the next
  entry into Arrange asks first (`SongSounds::kitChoiceTrack()`, the platform's
  pre-launch hook in `main/gui/gui.cpp`): **Replace on T<n>** (the track plays
  the kit, as before) or **New track** (a kit track of its own, made as ADD
  TRACK > KITS makes one, `new_kit_track()`). The power button answers nothing:
  Arrange stays closed and the screen it came from comes back. The question is
  kept in RAM only; another song opening or a reboot drops it, and the song
  loads its own kit. A song with no kit track takes the kit as its own, as
  before. With a Pitch track, the instrument is built beside a loaded kit
  (`song_wants_beside()`), so the kit is never kept out of the arena by it.
- **The kit loader.** ADD TRACK > KITS > Kit opens the board's kit browser over
  Arrange (core `IKitPicker`, `PlatformServices::kitPicker`; the browser in
  pick mode, `KitSelectorConfig::onPicked`, lists only the kits, reads PICK
  on its LOAD key and hands the kit back instead of loading it, and Arrange
  comes back when it closes). The pick is the track's kit
  (`AudioSong::setTrackKit()`), and the song's too while the track is live
  (`song_sounds.cpp` sees the change and loads it as an opening song's). Once
  Arrange is back the new track is picked and armed, and nothing opens: it is
  empty, REC records the pads into it and CLIP EDIT opens its seq view. A
  Pitch track is made the same way, except that with no instrument picked yet
  it opens Pitch Play, where one is. A kit track's menu opens the loader again
  to change it. A kit track with no name of its own shows its kit's.
- **MIX and the meters**: the live kit track's fader trims "Sampler"
  (`setSeqGain(Kit)`), `seqLevel(route)` reads each engine's own peak. A block
  with notes of its own MOVEs between kit tracks.
- **Bouncing a kit track** records the kits' engine's channel; Arrange asks
  for it to be armed first when another track is live, and waits for its kit
  to be loaded.

## The seq track through Arrange's MIX

A seq track's notes play through the pads' engines, not a stream of the song,
so its fader is put on those engines' mixer channels while the song plays: a
trim beside the Mixer's own fader (`AudioMixerEngine::setChannelTrim()`, not
saved with the desk). The trim goes on Sampler and Sampler2 for a kit track
and on Pitched for a Pitch track, through `ISongEnginePlatform::setSeqGain()`
from `applySeqSound()`. It is 1 again at stop, so a bounce, which stops the
song first, records at the Mixer's level and the track's fader stays on the
bounced clip. Pads played along while the song plays go through it too, as
they would through a DAW track.

Its M, and S on another track, silence its notes as they play: the song map is
muted, so the pads a player hits stay theirs, and unmuting mid-song brings the
notes back. That is why a seq track runs the sequencer with the song even
while muted, and why a track that plays its scene everywhere is laid on the
song as one span. `ASONG VOL t pct`, `ASONG MUTE t 0|1`, `MIX_LVL` (`trim=`).

**Pan, and MIX as a DAW's mixer (core 2.21, mixer 0.18, arrange 0.23).** Each track has a pan (`SongTrack::pan`, -1..+1, `"pan"` in song.json
left out at centre, carried by undo, a merge and the Live export), a balance
as the Mixer's: the side it moves away from is turned down. An audio track's
clips are panned in the song's render, before its meter. A seq track's pan
goes to its engine's channels the way its fader does: a trim pan beside the
Mixer's own pan (`AudioMixerEngine::setChannelTrimPan()`, the two multiplied,
not saved with the desk), through `ISongEnginePlatform::setSeqPan()` /
`setKitPan()`, 0 again at stop. Arrange's MIX strip is, top to bottom:
the track's number and kind (or name) in its colour, PAN (C / L25 / R40, a
tap on the reading centres it), the fader with the track's stereo meter beside
it (`ISongEngine::trackLevels`, after the fader and pan, the lanes' -48..0 dB:
green, amber from -6, red from -1), the dB, then M and S. Measured on the
board: an audio track soloed at L50 played 0.077 left, 0.000 right on the
"Song" channel; a Pitch track at R50, 0.001 / 0.195 on Out 1/2.

## Freeze: the tracks not being played cost a file, not an engine

The render has 1451 us a block and a four-source song (Drums, Slice, Pitch,
audio) takes ~850 of them, so what fits live is bounded by the engines, not
the tracks. A seq track can therefore be **frozen** (core 2.9,
`ISongEngine::freezeTrack()`): a bounce of its engine's channel alone
(`ISongEnginePlatform::beginRouteCapture()`; the board puts that one channel
on the pads tap, so fingers on the armed track's engine stay out) into
`clips/.freeze/FRZ%08X.wav` (named by what it plays, below), kept in `SongTrack::freeze` with `freezeKey`.
The track keeps its notes and stays a Drums, Slice or Pitch track.

**Only two passes of a looping block are rendered** (core 2.11,
`freezePlan()`): the first as the block starts and the second as every later
pass sounds -- tails of the pass before in it -- and the song plays the second
as often as the block goes round, as audio blocks with an `offset` into the
file (`SongTrack::freeze` is a list of them). A block no longer than two passes
is rendered whole. Each render plays a bar in before what it records, so what
rang on from the block before is in it, and a block followed by a gap gets a
bar of what rings on. A 2-bar take round 64 bars freezes in 4 bars of real
time, not 64. Not exact where a voice outlives a pass by more than a pass, or
where a tail rings into the next block's own (it is cut there, as the next
block starts its own render).

**Only what changed is rendered** (core 2.12): each render is named by what
it plays -- the notes in its window where they fall, its run-in and length,
the sound (name and `SongTrack::freezeSalt`), route and tempo
(`pieceKey()`) -- in `clips/.freeze/FRZ<key>.wav`. A block the same as another
in the track, or as it was at the last freeze, is found there, not rendered:
eighteen passes of one pattern and one with an overdub render two blocks'
worth, and after an edit only the edited block -- and the one after it, whose
run-in it is -- renders again. A render is written as `.part` and named when
whole; a cancelled freeze keeps the renders it finished, and the next one does
the rest. Renders no track's freeze names are removed when a freeze lands.
`ASONG` reports `pieces=done/needed`.

**A freeze -- and since 2.14 every bounce -- renders off the clock** (core 2.13, `ISongEnginePlatform::
beginOffline()`): the outputs fade out, and `audio_rt` runs passes of the clock
and the captured channel only (`IdfAudioModule::processOffline()`, the mixer's
`renderOnly()`), written to no I2S and so paced by nothing but what it waits
on -- the capture's writer past half its ring (`captureBacklogged()`), a
streaming voice less than 8192 frames ahead (`SamplerEngine::streamLead()`),
and the event task with the notes the last pass posted (the Sampler's take
plays from there, `esp_event_bus_pending()`). The sequencer is polled by the
render after every pass instead of by `seq_poll`'s 2 ms of wall clock
(`sequencer_poll_from_render()`), which offline was many blocks: a note lands
on the block after the one it falls in, and two offline renders are the same
to the sample (real time differs from itself by a block either way, the poll's
jitter). Measured: 8 bars in ~3 s against 16.5 s. `ASONG OFFLINE 0` freezes in
real time instead, `ASONG FREEZE t` freezes a track now -- for an A/B.
**A lead that does not grow is not waited for.** A voice that finds no free
stream ring plays its head and is let go only within 4096 frames of the head's
end (`SamplerEngine::kRingRetryFrames`), short of the 8192 the render waits
for. After 300 ms of a lowest lead that does not grow the render goes on a
pass a tick (about real time) until every lead is back over 8192, and logs
`offline: a voice's lead stuck`; such a freeze still renders faster than real
time (measured 11 s against 26 s), the same audio to 0.1 dB a bar.
**Not while someone listens.** Off the clock the outputs are
silent and a press lands in the take wherever the render happens to be, so a
take goes off the clock only when nobody has pressed anything in the last
2 s (`PadManager`'s input tap: the pads, MIDI, the HIL's hits -- never the
sequencer's playback) and no host is streaming into the board
(`usb_audio_port_playback_streaming()`). A press, or a host stream starting,
during one drops it to real time from that pass on (`audio_rt_note_live()`):
the file stays one continuous render, the outputs come back, and what is
played from then lands where it is heard. A set played over a bounce
(`live_set`) needs that: off the clock the pads would land in the take at the
wrong place and the host's stream would be silent for the length of it.
**Never heard (core 2.29, mixer 0.19).** `beginRouteCapture()` puts the captured engine's channel on the tap only
(`AudioMixerEngine::setChannelTapOnly()`: rendered, metered, recorded, sent
to no output) until `endPadsCapture()`, off the clock or not. That would also
silence a player on the same engine and put their notes into the take, so a
freeze of the engine the pads play (the armed seq track's live route, the
pads' kit without one) waits while the pads are played
(`ISongEnginePlatform::padsPlayedRecently()`, the same 2 s as above, a host
stream not counted): refused with "waiting: the pads play its engine", or
given up at a press mid-take, and Arrange tries it again later (its backoff,
2 s doubling to 60 s). A BOUNCE asked for is not given up (core 2.30): it
waits for the pads to be quiet before it takes their engine, and a press
during a render starts that render again.
**A seq track's BOUNCE is a freeze turned into audio** (core 2.14): the same
plan and renders, found on the card where they are -- a track frozen as it is
bounces with nothing to render -- and the renders become its audio blocks.
Arrange shows a freeze in its processing box (FREEZING TRACK n, render k of N,
%), as it shows a time stretch. Those blocks name their renders where they are,
`.freeze/FRZ….wav`, and `AudioSong::load()` keeps such a name
(`is_song_clip_name()` takes both forms, core 2.30).

While it is **not armed** and the key still matches -- its bars, blocks and
notes, its kit or instrument path and the tempo (`seqKey()`) -- `flatten()`
lays the file on the track as an audio block and leaves its notes out of the
sequencer's map; an audio stream is ~8 us a block against ~150-400 for an
engine. Anything that moves the key plays it live again (`FreezeState::Stale`);
undo is outside the key on purpose, so undoing an edit makes the freeze match
again. An instrument built or saved from edits zeroes the Pitch track's key
(`SongSounds::keepInstrument(..., changed)`); an edit to a kit's kit.json is
not seen (the key names the path).

**Arrange freezes on its own** (`auto_freeze()`, `crosspad::autoFreezeNext()`,
core 2.26): every seq track ARM is not on that is not frozen as it is -- the one
ARM just left, a new track added, one gone stale since (a tempo turned, a kit
loaded, notes changed) -- is frozen one at a time with the song stopped, with
Arrange on screen (the seq view and Pitch Play use the sequencer a freeze
takes). A seq track of blocks with none left (`AudioSong::seqPlaysNothing()`)
needs none. A freeze that failed is tried again later, twice as long each time
(`RetryBackoff`, 2 s to a minute). Measured in beat_session: STOP to FREEZING
1.5 s, frozen 1.7 s; a 120 -> 110 BPM turn, both kit tracks frozen again 6.5 s
after. PLAY during a freeze cancels it and
plays; the next stop finishes it from the renders already made. The header
says `FRZ <name>` in ice blue, `FREEZING 2/5 40%` while it is made. The transport bar's `CPU nn%`
(crosspad-gui 2.7, `IGuiPlatform::getDspLoad()`) is the render's average
load: amber from 75 %, red from 90 % or for a second after a late block. At
85 % the engines already stop adding voices (`audio_task.cpp`, shed).

Not yet: a frozen Pitch track's bank stays loaded (its memory and its idle
~28 us a block).

## Elements (arrangements inside arrangements)

An **element** is a song of its own that another song plays as one block
(core 2.25.0, Arrange 0.25.0; design:
`docs/design/2026-10-24-nested-song-elements-design.md`). It is a
project folder -- `song.json`, `clips/`, `sequences.json` -- and the song that
uses it plays a **bake** of it, a WAV mixed down at the song's tempo, so the
engine never runs more than the one song's tracks.

```
<songsRoot>/<project>/...                  the song
<songsRoot>/.elements/E%08X/...            an element: a project folder, one per ref
<songsRoot>/.bake/B%016llX.wav             bakes, shared by every song
```

The library is the folder, outside every song: an element made in one song is
listed (`requestElements()`, `elements()`: ref, title, length, the elements its
blocks use) for the "element from the library" picker of any other.
`requestOpenElement(ref)` opens one as the song, creating the folder when
missing, and does **not** write NVS `song/current`: a reboot inside an element
opens the song it was reached from.

**The block.** An element block is a `SongClip` with `element` set to the ref
(`.elements/E%08X`, the only shape a path built from it may take,
`is_element_ref()`), `file` the bake's name (empty until baked), `frames` one
pass of the element at the bake's tempo, `from` the element bar the block
starts on; `bar`/`bars` are the grid as for any block, and a block longer than
the element loops it. `song.json` is `version` 5 only when it holds an element
block or a `title` (an element's display name; a song has none), so a song
without them stays readable by firmware from before, which calls a newer song Unreadable.

**The bake's key** (`ElementHash.hpp`): `element_bake_key()` is FNV-1a 64 over
`element_content_hash()`, the tempo rounded to 0.01 BPM, the sample rate and
`kRenderVersion` (bumped when the mix-down changes what it writes). The content
hash covers what is heard: per heard track its kind, stretch and volume
(quantised to 1/10000), per block its file, place, cut (`offset`, `frames`,
`from`) and notes -- an audio file with its size and time on the card, an
element block by the bake it plays, a seq track by its freeze files and sizes --
then length, metre, and the kit's and the instrument's files (a path from the
card's root, `/crosspad/kits/...`, read under the mount, `kitFsPath()`). A silenced track
counts as its index and a marker. Not the tempo (it is in the key), names, the
armed track or the launcher's sets. Equal keys are the same sound: a bake
already in `.bake/` is found, not written (`ElementBake::cached`).
`requestBakeElement()` mixes the open song down, `[0, playEnd)`, seq tracks from
their freeze; it fails, with `editReport()` saying why, when a seq track is not
frozen or a stretched block is not fitted yet. The answer is `bakeResult()`.
A block whose pass is shorter than a bar of one beat at 400 BPM (`kMaxBpm`) is
no bake -- a `song.json` cut short -- and plays nothing; a bake shorter than the
bar it plays in (made before the song slowed down) still goes round.
The library's list (`requestElements()`) reads every element's `song.json`, so
while the song streams it waits for the stop, as the save does.

**Enter and leave** (`ElementNav`, core `crosspad/song/ElementNav.hpp`; Arrange
`src/arrange_elements.cpp`). Every move is a job of phases that Arrange's timer
walks every ~50 ms with `poll()`, requests to the engine and reads of its state
only, so nothing waits for the card on the LVGL thread; the screen shows a
shield and progress while one runs.
- **One tempo, the song's**: taken when the first element is entered; every
  element under the song is fitted to it and baked at it.
- CLIP EDIT on an element block **enters**: stop, open the element
  (`requestOpenElement`), and when it was made at another tempo fit it to the
  song's -- its audio tracks that keep their speed (`StretchMode::Off`) become
  time-stretched, so a recording follows the song.
- The power click (or a crumb) **leaves**: stop; put the element back at the
  song's tempo when it was turned inside it (the top bar's BPM, a host's MIDI
  clock, an undo -- the top bar is not refused inside an element, the leave
  undoes it); wait for the level's kit and instrument to be loaded
  (`SongSounds::settled()`, through `PlatformServices::songSounds`; bounded,
  ~1 min), so a freeze never renders the kit of the level opened before; freeze
  every seq track that is not frozen, wait for the fitted copies of stretched
  blocks (bounded: the wait gives up when nothing more is fitted for a number
  of polls), bake, reopen the parent, and **relink every block of that
  element** in it to the bake's file and length. A failed bake stops the job
  where it is: the engine stays on the element and `path()` as it was.
- **A song opened behind its back** -- the kit browser's SONGS, `ASONG OPEN`,
  another app while Arrange is closed: idle, `path()` starts over at the song
  the engine has open (a leave from the element it last knew would bake that
  song into the element's blocks); during a job, the job stops before it
  freezes, bakes or relinks anything in it ("another song was opened
  meanwhile").
- **Insert new** (`insertNew`): a 4-bar element, titled `ELEMENT n`, laid on an
  audio (or a still unused) track and entered; a seq track is refused. **Insert
  from the library** (`insertExisting`): laid, entered and left at once, so it
  is baked at the open song's tempo. An insert that fails leaves its block on
  the track, not baked, and the toast says so.
- **Cycles are refused at insertion**: an element on the breadcrumb, or one
  whose own blocks hold one that is, cannot be inserted (`ElementInfo::uses`).
- **Effective tempo**: a song never given a tempo has `bpm` 0 and plays the
  default 120 (`kDefaultBpm`); `ElementNav` compares and fits by that value.
- **Crumbs**: inside an element the context row shows `SONG > ... > ELEMENT`
  (at most three, `...` for the first when deeper); a crumb leaves up to its
  level (`leaveTo`).

The worker takes a request made while it was handling another at once, without
waiting for the next wake-up, which a chain of requests -- open, freeze, bake --
would otherwise stall on.

### Making an element, and baking from the card (core 2.25.0, Arrange 0.25.0)

**From a region** (`requestElementFromRegion(fromBar, toBar)`): every track's
blocks in the region, counted from bar 0, the tracks' settings (kind, source,
scene, seq blocks, stretch, kit, volume, pan, mute, solo), the song's kit
and instrument (a song-relative instrument goes with its `pitch/` folder), the
recordings copied into the element's `clips/`, a seq block that played its scene
given its notes first, the region's length, the song's tempo, no freezes (leaving
the element freezes again). The song itself is not changed: Arrange then lays an
element block over the region on a **host track**: the lowest audio track the
region touches that holds nothing outside it -- its volume, pan and mute are set
neutral, since the bake already carries every source track's; its solo stays --
else the lowest unused track; with neither, Make is refused and nothing changes.
**From a song** (`requestElementFromSong(name, openElements)`): a
library copy of the project, titled with its name, its clips and all, not its
freezes or fitted copies; a song that holds an element of the breadcrumb is
refused before anything is copied. A failed job takes its half-made folder off
the card (`requestElementDiscard`).

**Baking from the card.** Besides the open song mixed on the engine, the
worker mixes a bake from files already on the card (`BakedView.hpp`,
`SongEngineDiskBake.cpp`). The **baked view** is a song read off the card with
every element block replaced by the bake it plays and every seq track by its
freeze, what a mix needs and nothing else; what it lacks (a seq track not
frozen, a file missing, an element with no bake) is named in words for the
block's why. The **resolver** (`ElementResolver.hpp`) names each element's bake
as it is now, children first, so a change deep in a nest changes the key of every
element above it: the content hash hears a child by its own key, and a nest that
leads back to itself is reported, not followed. Two hashes exist and must not be
confused: the *content hash* names the bake (what is heard), the *edit hash*
(`element_edit_hash()`, with the project's `sequences.json`) answers "did the
user change this element" for the question and the drafts below; an element's
identity is its ref, not its title.

The **relink pass** runs after every open and every commit that moves an
element block or the tempo: a block whose element's bake is on the card is
linked to it, one that can be mixed from files on the card is queued for a
background bake, the rest keep playing the bake they have.
`elementClipState()` says which, per block:

| State | The block |
|---|---|
| Fresh | plays the bake of its element as it is now |
| Stale | plays an older bake; the element cannot be baked from the card yet (`elementClipWhy()` says what it lacks) |
| Baking | plays an older bake while the new one is made |
| Missing | has no bake on the card, plays nothing |

**The context stem.** Inside an element the song around it is one WAV
(`requestContext()`, `ContextMix`): one pass of the element long, **the focus
silent everywhere it occurs** (not only on the way down to it), everything else
mixed from files already on the card -- recordings, freezes, other elements'
bakes, an older bake where a newer is not made yet -- at unity, no meter, no
fader, going round with the element. It is cached under a key of what its parts
are with the focus silent, so editing the element never changes it and one asked
for again is found; it is asked for again when the focus grows (the pass is
longer). `contextState()` is None (nothing of the element is heard in the song),
Baking, Ready, or **Incomplete** when something below is missing, with
`contextWhy()` naming it; the stem then plays what could be baked. Arrange shows
a **mini-map** of the song around the focus and **SOLO** (`contextSolo()`) mutes
the stem: it is neither heard nor read from the card.

**Copy or replace.** An element is shared: any number of songs, and elements,
play one ref. Editing it inside one song would change the others, so the first
edit takes a **snapshot** (`requestSnapshot`: `song.orig.json` and
`sequences.orig.json` in the element's folder; one already there is the original
and stays). When the user leaves, `ElementNav` asks the question if the edit hash
moved (a snapshot byte for byte equal to the element is not a draft and is
dropped): **Make a copy** keeps the original for the others and takes the edit
as a new element, **Replace** keeps the edit for everyone and drops the snapshot.
The copy's cascade (`ElementCopyPlan`, from the reverse index
`requestUses()`/`usesIndex()`): every element of the song's tree that plays a ref
going away is either copied, because something outside this song hears it too,
or, heard only through the song, re-pointed where it is, up to the song; the
same goes for **other occurrences of the element in the song**, which all take
the copy. A copy's title is counted on (`REFREN` -> `REFREN 2`). The job is one
`requestCopyElements`: it fails with nothing changed, copies made taken off the
card and re-pointed elements written back (it refuses more than one fork marked
to restore). **Drafts after a power cut**: an element that still holds a
snapshot with a changed edit hash is a draft the next entry and the next list
find; Arrange asks again, and a leave past a level holding a draft is refused ("keep
or undo the unfinished edit of this element first"). **Delete** of an element (`requestDeleteElement`) is refused,
with a fresh reading of who uses it, while any song or element does (the failure
names them) and while it is the open one, and while a song plays.

**Baking while playing, and never waiting.** All of it is the worker's
background work and goes on with the song playing: a piece of work only while the
ring holds at least half of itself (`pace()`), a mix in chunks between top-ups
(`kMixSpanChunks`), at most three card descriptors held (two readers beside the
writer, `kBakeReaders`), and any request the loop takes comes first. A **leave
does not wait** for its bake: the parent opens at once, the block shows Baking
and plays its old bake until the new one lands. **Idle baking**: Arrange tells
the engine the player is away -- the song stopped and no pad played for
`kIdleBakeMs` (5 s) -- and the background goes on to *other songs* that play an
element changed since: each one's blocks of it are relinked in its `song.json`
on the card, a bake mixed first where it is missing, with no song opened and no
kit loaded; a play or a pad stops it where it is.

**The cache is bounded.** `.bake/` (bakes and context stems) is brought under
`min(1 GiB, 10 % of the card's free room)` (`kBakeCapBytes`, `kBakeCapShare`) after
the first pass and after every bake, least recently used first. A file's last use
is a counter in `.bake/usage.json`, bumped by the relink pass and the stem and
written only once the song stops. **Never removed**: a file the open song's tree
plays, the stem in use, any element's **newest bake** (kept in its own
`song.json`, `AudioSong::newestBake`; an evicted bake is only baked again), and a
`.part` a mix is writing (the first pass removes the ones an earlier run left).
**An element that cannot be read removes nothing**: not knowing what plays, the
pass logs "bake cache: an element could not be read, nothing removed" and waits
for the next bake. On the board `freeStorageBytes()`
(`SongEnginePort.cpp`, `esp_vfs_fat_info` on `/sdcard`, worker only) gives the
free room; a platform that cannot tell leaves the 1 GiB cap alone.

**A kit saved under its own name** makes the freezes that play it stale. A
freeze is stamped with what it was rendered through: `freezeKit` (`kit` in the
freeze's object in `song.json`, written only when set) is the kit's or the
instrument's identity, `sound_identity()` -- `kit.json` whole and every file
beside it by size and time, as the element's content hash hears it. That is read
from the card, so a save in any boot is told, in any song, open or not: the
resolver does not bake from such a freeze (the block stays Stale, "Track n: its
kit changed since it was frozen", playing its old bake), opening the song or
element unfreezes that track (it plays live, and the next freeze renders anew),
and an engine bake that meets one unfreezes it and fails, so a leave freezes it
again before the bake (once). A freeze stamped before 2.25.0 has no identity
and is taken as it is. Inside the song open, a save SongSounds hears also bumps
the track's `freezeSalt` (`fsalt`), which enters the freeze key, the renders'
names and the element's content hash, so `freezeState()` turns Stale at once
without reading the card. **Varispeed of stale bakes**: a block remembers the
tempo its bake was made at (`bakeBpm`), and while the bake is stale after a tempo
change it is played at the song's tempo by linear resampling
(`varispeed_frames()`, pitch moves with it) instead of at the old speed. The
new tempo gives a new key, but a new bake only where one can be mixed from the
card: an element with a seq track frozen at the old tempo, or a recording not
fitted to the new one yet, stays Stale (varispeed, "frozen at another tempo",
"not fitted to the tempo yet") until it is baked on the engine. Arrange does
that by itself (0.26): with the song stopped and nothing touched for 3 s, the
first block whose element a rebake mends (`ISongEngine::elementClipRebakes()`,
`ResolvedElement::rebakes`: its own tracks, not a cycle, an undecided edit or a
child) is entered and left with the processing box up, a note saying which and
why; one that fails waits on a `RetryBackoff`. **Rebake now** in EDIT does the
same at once. Measured in beat_to_song: both blocks of a four-bar element of
kit tracks baked again 12 s after a BPM turn.

**An element of kit tracks** keeps the song's scenes (`sequences.json` written
with it, core 2.26): its tracks name them; given the card's old shared scenes
instead, every freeze would fail ("no such scene") and it would never bake.
`SongScenes` never adopts the shared file into an element; a render of a track
whose blocks hold their notes plays its clock on scene 0 when the scene is not
there (elements made by 2.25.0). It keeps ARM on the track it was on.

**An edit not decided yet is no other song's.** While an element holds its
snapshot (`song.orig.json`, `AudioSong::snapshotFileName()`), every song but the
one it was just left to resolves, bakes and links it from the snapshot
(`ResolvedElement::kept`, `ElementResolver`'s `heardDraft`): a power cut in the
middle of an edit leaves other songs playing the element as it was, and a song
opened before the question is answered never bakes the draft. Idle baking never
bakes or links the element open in the engine from its `song.json` (`editing`),
so neither is any element holding it; the cache keeps the snapshot's newest bake
as it keeps the element's. An element's newest bake found while the song
streams is written into its `song.json` once the song stops (the open element's
into the song in memory, with its next save), as the usage book is; a book the
card will not take is given up until the next use rather than tried again at
once.

**The edit hash counts each track's stretch mode**: a stretch the player
changes inside an element is an edit and asks the question; the enter's own fit
(`Off` to `KeepPitch` at another tempo) is made before the hash it is compared
with, so an element nobody touched asks nothing.

**A tempo turned inside an element** leaves the song around it unheard: the stem
was mixed at the song's tempo, and `contextState()` is Incomplete, with
`contextWhy()` naming that tempo, until the tempo is put back (the leave puts it
back too).

**Entering an element freezes the parent's live seq tracks first**, so the
parent can be baked from files while the user is inside: entering is slower when
the parent has unfrozen seq tracks.

**Known limitations.**
- Entering an element writes the song's tempo and the stretch change into the
  element itself, so another song that uses it finds it at that tempo, still
  playing from its own bake.
- A failed Copy at a copy-on-leave ancestor leaves its original pointing at the
  copy, with only a draft on the card, until the user resolves it.
- The list of copies made for the song (`elementsCopied()`) is in memory and is
  lost after a reset; the drafts on the card are what is found again.

**Left for later:** effects (spec section 8). A chain in an element's
`song.json` would enter its content hash and the disk mix-down; their place is
kept, nothing is built.

## Verbs (CDC)

| Verb | Does |
|---|---|
| `ASONG` | report: `ok open project bpm len sync play pos under openfail bounce … clips= chunks= chunk_avg_us= chunk_max_us= drain_kbps= io_fb=` |
| `ASONG OPEN name` / `PLAY [bar]` / `STOP` / `BOUNCE scene track` / `CANCEL` / `LEN bars` / `SYNC 0\|1` / `RENAME name` / `DELETE` | requests; poll `ASONG` |
| `ASONG REC bar [beats]` / `DUB bar [beats]` / `PUNCH R\|D\|N` | a take onto the seq track (0-based bar, count-in beats); STOP ends it |
| `ASONG BOUNCESEQ track` / `VOL track pct` / `MUTE track 0\|1` | Arrange's BOUNCE of a seq track; a track's fader and M as MIX sets them |
| `ASONG_SEQ [track]` | a seq track's blocks (the first seq track's by default), `ASEQ: bar= bars= from= notes= own=` each, then `track= take= punchable= edit= route= armed=` |
| `ASONG ARM track` / `SEQTRACK [kitjson]` | ARM a seq track; a kit track on the lowest free track, playing that kit (or the song's), armed |
| `ASONG_TRK` | every track in use, `ATRK: t= kind=a\|s route= live= fz= blocks= lvl=` each (`live=-1`: a kit track the kits' engine does not play now; `fz` 0 none, 1 frozen, 2 stale, 3 freezing; `lvl` the lane meter now, 0..1, the louder side: what the ring plays for the track -- a freeze too -- and its engine's peak, which the ask takes), then `used= armed= live_kit=` |
| `ASONG DELTRACK t` / `DUP t b` / `DELBLOCK t b` / `MOVE t b bar` / `PASTE a b at` / `TRACKKIT t kitjson` | DEL on a track; a block's DUP, DEL and MOVE; EDIT's copy of bars [a, b) pasted at a bar; a kit track's kit picked again |
| `SONG [ADD s r\|SET i s r\|DEL i\|CLEAR\|PLAY [i]\|LOOP 0\|1]` | the scene arrangement |
| `SEQ … DOUBLE\|HALF\|ERASE 0\|1\|COUNTIN 0\|1` | scene edits; `SEQ` reports `erase= countin= file=` |
| `MEM_BLOCKS PSRAM` | the PSRAM heap's block histogram |

Scenario: `crosspad-hil run song_bounce`.

## Traps

- **Count-in is on by default** (`SequencerController::init` sets it). A HIL
  scenario that records and hits at once needs `SEQ COUNTIN 0` first.
- A bounce that never closed (reset, card pulled) leaves a WAV with zero
  sizes; the clip list skips files with no frames.
- The sequencer's playback presses pads as `EventSource::SequencerCallback`;
  pad logic that must tell a finger from a take reads
  `PadManager::pressSource(pad)`.
- **A take keeps what each hit sounded as** (`PadPerformance`, core 1.55):
  `noteIn()` asks the active pad logic (`PadManager::performanceFor()`, set
  as the sequencer's performance source) and keeps its answer with the hit --
  under the Sampler's 16 levels, the source pad and the mode, with the
  velocity Full level or the ramp made of it. The event's `pad` stays the pad
  struck, so a 16 levels part keeps a lane per level. Playback hands it to
  `handlePadPress()`, and the Sampler plays a take's press as recorded
  (`pressPerformance()`): a 16 levels part sounds the same with the mode off,
  a plain part plays its own pads with the mode on. Stored as a fifth element
  of the event, `[ticks, pad, vel, len, perf]`, only where not plain
  (`PadPerformance::pack()`, levels << 4 | source) -- older firmware reads the
  first four. The Sequencer's scene window turns 16 LEVELS on through
  `PlatformServices::padPerformance`, while SRC is the kit.

## Loop region, region edits, merging tracks

- **Loop region** (`AudioSong::loopStart/loopEnd`, bars, `loop_start`/`loop_end`
  in song.json): with LOOP on, playback goes round it instead of the whole
  song, and Arrange's PLAY starts at it. RT and the worker wrap with one rule,
  `wrap_position()` in core's `SongEngine.cpp`, so what is heard and what is read
  next never disagree. Segments are cut at the song's length or the region's
  end, whichever reaches further.
- **Copy / paste / duplicate x2** are edits of the song model in Arrange
  (EDIT menu), by bars: what *sounds* inside the region on every track
  (`AudioSong::copyRange()`) -- a block reaching in from before is copied from
  the region's start, one reaching past it up to its end. Paste and duplicate
  insert time (`insertBars()`): a block sounding across the point is cut
  there, blocks at or after it move on, an explicit song length grows with
  them -- so nothing is written over. Duplicate doubles the region (4, 8, 16
  bars).
- **Cut** (EDIT → *Cut track N at bar B*, `AudioSong::cutClip()`): the picked
  track's block under the playhead in two. A block is `file`, `bar`, `bars`
  plus `offset` (frames into the file it starts at) and `frames` (what it
  plays from there, 0 = to the end), both in song.json only where set; the
  second half plays on from the cut, the first stops there and keeps that
  length through a tempo change (`doTempo` re-measures from `offset`/`frames`).
  `Segment::offset` carries it to playback and merge; the Live export sets the
  clip's start marker from it. Host tests: `[cut]`.
- **Seq blocks** (`SongTrack::seqBlocks`, `"blocks": true` in song.json): a seq
  track from a song saved before blocks had notes plays its scene over the
  whole song until the first cut or region edit gives it one block over the
  song (`seqBlocksExplicit()`); a track made now has blocks from the start. From then on
  its blocks (no file, `from` = the scene bar they start on) are moved, cut,
  copied, duplicated and deleted like audio ones, and the scene sounds only
  under them. The song engine hands the sequencer the blocks as sample spans
  (`PatternSequencer::setSongMap()`, re-sent after every commit while playing,
  cleared at stop); the sequencer counts the song's position itself on the
  same clock from `playbackAt()` and wraps it where the song wraps. The click,
  a take and the scene's loop bookkeeping stay on the scene's own loop, so a
  take records where the clock is, as before. A seq track's bounce renders
  only the bars its blocks play, first block to last (the same map, shifted to
  the first block's bar), and lands on that bar: the take runs in real time,
  and a one-bar block in a 64-bar song would otherwise be 64 bars of waiting. Merge leaves a
  seq track out: bounce it first (its header's menu). Host tests: `[songmap]`.
- **Merge** (`ISongEngine::requestMerge(mask, from, to)`): the unmuted audio tracks in
  the mask mixed down at their volumes into `clips/merge_NN.wav`, on the song
  engine's worker, reading a copy of the song; the range runs on to the end of
  any block that starts inside it. The clip goes on the lowest merged track at
  `from` (whose volume becomes 1.0, the gains being in the clip now), the
  replaced blocks go, and a block reaching into the range from before it is
  cut at `from`. Checked on hardware: the merged file is the sample-exact sum
  of its two sources. `ASONG REGION a b`, `ASONG MERGE mask from to`.
- **Import** (`ISongEngine::requestImport(file, start, end, track, bar, lead)`):
  a region of any 16-bit WAV at the engine's rate into `clips/`, after `lead`
  frames of silence, on a track at a bar -- how the Recorder's ARRANGE puts a
  take in (see [sample-tools.md](sample-tools.md)). Blocks starting inside it
  go, one reaching in is cut, as for a merge. `ASONG IMPORT`.
- **`framesPlayed()`**: frames the running playback has sounded, 0 while
  stopped, priming, armed or counting in; lock-free, asked by the render every
  block to line a take up with the song's first frame.
- **Count-in** (`playCountedIn(bar, take, beats)`, `countInBeatsLeft()`): in
  sync the metronome counts (`PatternSequencer::playbackAt(pos, beats)`,
  `MetronomeNode::startCountIn(beats, pos)`) and the song starts where the
  clock's loop does -- mid-block, from how far the clock moved in the block it
  started in; alone the song plays the metronome's click itself (`Counting`)
  and starts on the sample after the last. Host tests check both to the
  sample.
- Arrange's MIX sets each track's `volume` (a fader in dB, as the Mixer app's)
  and `muted`; `commit()` hands them to playback at once and saves the song once
  nothing has been committed for 400 ms (`kSaveDelayMs`) -- a fader turned is
  dozens of commits, and a card write each under playback was the alternative.
  Not while the song streams, though: a song.json write takes the worker off
  the ring for longer than the ring lasts (21 underruns in three seconds of
  moving round the UI), so the save waits for the stop.
  `ASONG_DBG` reports the stream's margins since the last read: `ringmin`
  (frames), `gap_max_us` (worker off the ring), `read_max_us` (one card
  access) and `save_max_us`.
- Rename, delete, tempo, merge and import are edit requests to the worker
  (`editState()`, `editProgress()`); core's host tests cover each, and the
  loop region's wrap.

## One tempo, and the playhead while stopped

- **Tempo** lives on the sequencer's clock; core's `transportTempo()`,
  `transportSetTempo()` (live, except under a playing song, whose audio cannot
  follow) and `transportCommitTempo()` (the open song re-measured and saved via
  `requestTempo()`, which stops it if it plays) are the one way to change it.
  The top bar's BPM (click, turn, click), the Sequencer's BPM field and
  Arrange's tempo dial all go through it, so Arrange, Song and the Sequencer
  show one number after an edit. Playing a song still puts its own tempo on the
  clock. `main/midi_clock.cpp` reports a host clock through
  `transportSetTempoFromHost()`, and the bar's BPM then refuses an edit.
- **Metronome**: the top bar's metronome key (beside STOP) and the Sequencer's
  M are one switch, `transportClick()`/`transportSetClick()` over the
  controller's click. REC does not turn it on and STOP does not turn it off,
  so the player's key keeps its state. A count-in is heard either way.
- **Locate**: the top bar's position is a value too, for a claim holder with
  `kTransportCanLocate` -- Arrange's `SongTransport` while stopped:
  `transportLocate(±1)` moves where PLAY starts a bar a detent. Arrange shows
  the playhead there while stopped and scrolls the grid to it when it moves;
  selecting a block still puts it on the block. The cut works at that bar. The
  held bar is the open song's (`ISongEngine::openGeneration()`, core 2.26):
  another song -- NEW included, even under a deleted song's name -- starts on
  its bar one.
- **STOP**: a STOP that ends a take -- recording, overdubbing, armed -- goes
  back to bar one; a STOP while only playing stops in place, and a second one
  rewinds.
- **The scene the clock runs** when the song starts is the one the seq view
  edits while it is open (`openSeqEdit()`), else the pads' track's, so a part
  recorded with SYNC in the seq view of another kit track's block reaches
  that block.

## Arrange as a performer uses it (28.10.2026, core 2.33-2.36, Arrange 0.29-0.31)

- **Mute, solo, fader and pan in ~0.3 s** (core 2.36, `remixRing()`): the ring
  is mixed 1.5 s ahead, so a level change on a frozen or audio track would be
  heard a ring later. `rebuild()` flags a change to a track the ring holds
  audio of; the worker cuts the ring back to the first chunk 300 ms
  (`kRemixLeadMs`) after what RT is playing (a `ChunkMark` per chunk: ring
  head and song frame) and mixes the rest again. Measured: every ring track
  muted, silent 0.10-0.25 s after the last `ASONG MUTE`, 0 underruns
  (`ASONG_DBG remix=` counts them). A mute or solo re-mixes at once, a fader
  or pan once its drag has rested 250 ms (`kRemixRestUs`), so a drag does not
  cut the ring back at every step. Not in the launcher, and not while a
  track's notes change over to its freeze (`songMapLaterIn()`).
- **A stale freeze stands in** (core 2.36, `flatten()`): a kit track the kits'
  engine does not play plays its last freeze, stale or not, under the blocks it
  has now (a deleted block's audio is not played), instead of falling silent
  until the next stop freezes it again. Its header shows the faint snowflake,
  `PARKED` only with no freeze at all.
- **Export mix to WAV** (core 2.35, `requestExportWav()`, the song's settings):
  every track heard -- mute, solo, fader, pan -- from bar one to the song's end
  into `<song>/<name>.wav` through a `.tmp`. Seq tracks play their freezes, the
  armed one's too; Arrange freezes what is not current first (`s_exportWav` in
  `auto_freeze()`, `live_kit()` handing the kits' engine to each kit track
  still to freeze in turn), then writes. Refused, naming them, while a seq track that
  plays something has no current freeze. "amen flip": 30 s in ~10 s.
- **The timeline by finger**: a tap on the ruler puts the playhead there (and,
  playing, the song goes on from there); the ruler held and dragged draws a
  loop region, looping on the release; a block held and dragged follows the
  finger, snapped to bars and to lanes of its kind (one undo step); EDIT held
  is one undo, and undo makes a running freeze give way (core 2.37). Checked
  by `tools/hil_arrange_touch.py`.
- **No note editor, the take again**: EDIT on a selected seq block -> Record
  this block again: the region round it, its track armed, REC from its first
  bar, counted in; the first pass replaces it, later ones are dubbed in.
- **Tracks duplicate**: the track menu's Duplicate copies an audio track, or a
  kit track's own-note blocks onto a new kit track with the same kit.
- **Tracks move**: the track menu's Move up / Move down (`AudioSong::swapTracks()`:
  ARM, the live kit track and the launcher's columns follow; refused while a
  freeze or take names the track by number).
- **Bars in and out**: EDIT inserts the region's length of empty bars at it,
  deletes the region and closes the gap (`AudioSong::deleteBars()`), or with
  no region inserts 4 bars at the playhead. A drag over the lanes scrolls them and never reaches the
  screen-wide back swipe while they can scroll (touch_gesture's veto).
- **What things are called**: a seq track's blocks are lettered by part --
  DUPs share their notes (and `load()` makes equal ones share again), so a lane
  reads A A B A; headers and MIX strips carry the track's name, its kit's or
  its instrument's (`track_title()`), set smaller when long; a Pitch track's
  menu opens its instrument in Pitch Play. A tap on a MIX strip's dB reading
  puts it at 0 dB. A track header held mutes or unmutes it at once (Arrange
  0.34; MIX is the key beside the song's name).
- **Freeze progress** is the box over the lanes, as a time stretch and an
  element bake are (`FREEZING TRACK n`, render x of y; Arrange 0.42).
- **The metronome is kept** across a restart (core 2.33, NVS `cfg_click`:
  on, level, division, count-in; `transportRestoreClick()`).
- **What the screen says is what plays** (core 2.38.2-2.40, Arrange 0.37-0.39):
  - the lane and MIX meters read a frozen seq track (its freeze in the ring),
    and a live one after its fader and pan; the ring's meter slots cover twice
    the largest ring, and a re-mix keeps those RT still plays;
  - VOICES counts the kits' engine and the Pitch instrument, amber
    when any one is at its own ceiling (gui `VoiceMeter.full`), and the song's
    tracks played from files -- audio and frozen seq tracks -- one each, heard
    now over all of them (`ISongEngine::ringVoices()`, core 2.41);
  - CLIP EDIT on a seq track arms it, so the pads in its seq view play its kit;
  - stopped with a region looping, the playhead is where PLAY starts: the held
    bar inside the region, else the region's start; a header tap picks the
    track without moving it;
  - undo of a deleted, added or moved track puts back ARM, the live kit track,
    each track's input and freeze (`Snapshot` side, not compared: ARM is no
    step), and a freeze landing keeps renders an undo step still names;
  - with Arrange up the armed track's fader and pan are on its engine stopped
    too (`ISongEngine::setSongScreen()`); mute stays the song's;
  - a REC that cannot start says why (`ArrangeAudioTake::refusals()`); ARM,
    Delete track and Record this block again wait for a running take; pasted
    bars only go into the song they were copied in;
  - a live track's header says `LOADING` while its kit or instrument loads and
    `!` in red when it would not (`track_sound()`).
- **Colour, peaks, monitoring** (core 2.41, Arrange 0.40): a track can take one
  of eight colours (track menu -> Colour, `SongTrack::colour`, song.json
  `colour`, an undo step) for its number, blocks and MIX chip; MIX meters hold
  their peak 1.5 s and turn it red once the track reached full scale, until the
  meter is tapped; an audio track whose input is monitored shows a speaker
  before its name.

## Note lengths

- `PatternSequencer::noteOut(pad)` stamps a pad's release into the same input
  ring as `noteIn()` (velocity 0 marks it); every input that feeds `noteIn()` --
  the STM pads, BLE MIDI, HIL injection -- feeds it too. A recorded press stays
  open until its release, and the event gets `len`, the samples it was held
  (from where the finger went down, quantised start or not; a release before
  beat one leaves a hit). A take that stops with a pad still down ends the note
  where the clock is.
- Playback holds a note with a length down for that many samples, counted on
  the clock poll by poll (2 ms of jitter, like the press), and lets go before
  the pad is struck again. `len` 0 is a hit, released on the next poll as
  everything recorded before lengths was -- right for one-shots, wrong for a
  sample on HOLD or a chord, which is why lengths exist.
- `sequences.json` writes the length as a fourth element of an event, in ticks,
  only where there is one; firmware from before reads the triple and plays a
  hit. A tempo change rescales lengths with positions; double copies them.
- STOP clears the "recorded this pass" marks, so a take stopped mid-loop plays
  in full from the next start, not only after its first wrap.
- The Sequencer's grid and Arrange's seq lane draw a held note as a bar as long
  as it is held. Host tests: `[length]`.
