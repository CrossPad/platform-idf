# Songs: bounce, arrange, play back

Contributor notes: the song engine, Arrange, freeze, elements. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Audio Song (bounce, arrange, play back)

Sequencer (`components/crosspad-sequencer`, repo `crosspad-pattern-seq`)
bounces a scene to a WAV in a project on the card; Arrange lays clips on eight
tracks; Song arranges scenes as blocks with repeats; Pad Mixer sets per-pad
levels through `PlatformServices::padMix`. Core owns the model and interface
(`AudioSong`, `ISongEngine`, `WavWriter`) and, since 1.24.0, the engine itself
(`crosspad/song/SongEngine`); `crosspad-platform-idf/SongEnginePort.cpp` only
implements `ISongEnginePlatform` for the board, crosspad-pc has its own port.
**Every card operation is a
request to the `song_io` worker — nothing on the LVGL thread or the CDC loop
waits for the card**; a bounce writes 4 kB pieces that end on 4 kB of the file
through the board's DMA stage (`writeIo`, one CMD25 each; `ASONG` reports
`chunk_avg_us`/`drain_kbps`/`io_fb`); the scenes are the song's, `<project>/sequences.json` (a song from before 11.10.2026 copies the old shared `/sdcard/crosspad/sequences.json`), not the kit's -- a kit change keeps the groove (`seq_kit_change`); only `song_io` frees the rings, after RT has left
(`rtGen_`); rings never take the last 128 kB of PSRAM. The sequencer's
playback presses pads as `EventSource::SequencerCallback` (read with
`PadManager::pressSource()`); count-in is on by default (`SEQ COUNTIN 0` in a
scenario that records at once). Home screen: pad map, grid or list, Settings → Display
(`CrosspadSettings::launcherStyle`). Arrange edits by bars: a cut keeps an
audio block's `offset`/`frames` into its file, and a seq track that has been
cut or region-copied plays its scene only under its own blocks
(`seqBlocks`, `PatternSequencer::setSongMap()`). **REC/DUB in Arrange record the
pads onto the seq track from the playhead** (`ISongEngine::recordSeq`): a REC is
blocks of its own notes (`SongClip::notes`, shared copy-on-write, in song.json as
one string per block, `version` 2), to the bar of its last note -- shorter
than the loop it was played in, one block going round to the loop's end --, at most 8
bars / 192 notes each -- what one scene holds, so CLIP EDIT opens every one
(`AudioSong::putSeqTakeNotes`); a DUB adds to
the blocks under it; LOOP over a region goes round it, and with LOOP on and no region a take inside the song goes round the whole song (core 2.26). **Kit tracks,
a kit each, on one kits' engine** (core 2.42; "Kit 2"/"Kit 3", more engines, are
gone), and a Pitch track beside them (route 2, core 2.6): every kit track keeps
its kit (`SongTrack::kit`), the live one (`AudioSong::liveKit`) is the one the
engine plays and its kit is the song's -- the pads', the Sampler's; the others
play their freeze. Arrange's `live_kit()` gives the engine to the armed kit
track only after freezing the one it leaves (a new kit track stops the song:
stop, freeze, then the new kit), CLIP EDIT arms the
track and hands it over at once, so EDIT in its seq view edits that track's
kit; a kit loaded while the song is open is the live track's
(`AudioSong::kitHeld()`). REC/DUB take onto the armed track (ARM,
`AudioSong::armed`); the sequencer routes each note
(`PatternSequencer::RoutedSink`). Arrange picks a track's kit in the kit
browser (core `IKitPicker`, crosspad-sampler `kit_gate`). The seq view edits one block at a time in the
track's scene (`openSeqEdit`/`syncSeqEdit`); a seq track's bounce takes only
the bars its blocks play. **Freeze** (core 2.9, `ISongEngine::freezeTrack()`): a seq track's engine
alone bounced to `clips/.freeze/FRZ%08X.wav` and played in its place while it is not
armed and its notes, sound and tempo are unchanged (`SongTrack::freeze`,
`freezeKey`) -- a looping block's two passes only, the second played as often
as the block goes round (core 2.11), each render found on the card by what it
plays so only changed blocks render (`clips/.freeze/`, core 2.12), rendered
off the clock ~5x faster than real time (`audio_rt` offline: the clock and the
captured channel only, the sequencer polled by the render; core 2.13); Arrange freezes every seq track ARM is not on that is not frozen as it is once the song stops (core 2.26 `autoFreezeNext()`), PLAY
cancels a freeze, and the transport bar shows the render's load as `CPU nn%`.
A mute, solo or fader on a ring track is heard in ~0.3 s (core 2.36, the ring
re-mixed from 300 ms ahead, `remixRing()`); a kit track the kits' engine does
not play plays its last freeze, stale or not, instead of silence; Export mix
to WAV writes `<song>/<name>.wav`; a song keeps its Pitch pad map
(`pitch_pads`, in the freeze key) -- [docs/subsystems/audio-song.md](../subsystems/audio-song.md#arrange-as-a-performer-uses-it-28102026-core-233-236-arrange-029-031).
`ASONG REC|DUB|PUNCH|ARM|SEQTRACK [kit]|TRACKKIT|EXPORTWAV`,
`ASONG_SEQ [track]`. **A song names its kit and its Pitch instrument**
(`AudioSong::kit`/`instrument`, `main/song_sounds.cpp`): opening it loads them --
no kit in the song frees the kit's memory -- and a kit or instrument picked
while it is open is written into it (an unsaved instrument is saved to
`<song>/pitch/` first). One tempo:
`transportSetTempo()`/`transportCommitTempo()` in core, turned on the top bar's
BPM (its position locates Arrange while stopped). **Elements** (core 2.25, from a region or a song, baked from the card, copy or replace, a bounded cache): a block can be another arrangement, baked to `.bake/` -- [docs/subsystems/audio-song.md](../subsystems/audio-song.md#elements-arrangements-inside-arrangements). `ASONG`/`SONG` over CDC;
`crosspad-hil run song_bounce`. Details:
[docs/subsystems/audio-song.md](../subsystems/audio-song.md).
