# Sample tools: Recorder and Slicer

Two apps on one platform service, `crosspad::ISampleTools`
(`components/crosspad-core/include/crosspad/sampletools/`), implemented in
core by `SampleToolsEngine` over an `ISampleToolsPlatform`, the way the song
engine is: takes, auditions and file jobs are portable, and the board's side
(`components/crosspad-platform-idf/SampleToolsPort.cpp`) is the worker task,
where ring memory comes from, the card, the ADC claims and the render's taps.
crosspad-pc runs the same engine (`src/sampletools/PcSampleToolsPort.cpp`:
IN1/IN2 as MIC/LINE, the capture bus as PADS, no USB). Host tests:
`tests/test_SampleToolsEngine.cpp` in core.

- **Recorder** (`components/crosspad-recorder`): a take from the two inputs the
  Mixer's PAIR connects (MIC, LINE 1/2, LINE 3/4), PADS, USB or PITCH,
  trimmed front and back to a loop (START/END turned coarse, then fine with the
  picture zoomed), auditioned in a loop, saved as `loop_NNN.wav`, or handed to
  the Slicer.
- **Slicer** (`components/crosspad-slicer`): up to 16 slices by the loop's hits
  (sensitivity), in equal parts, or cut live (FREE: the first pad plays the
  loop, every next pad cuts where it is heard); SEL/MOVE/+/DEL; ATK, every
  slice's attack; PITCH, the loop transposed; FX, a copy of the loop with
  effects the slices move onto; KIT saves them as a Slice kit (one WAV, a
  region a pad, one voice).

## Threads

| Thread | Does |
|---|---|
| `audio_rt` | meters the selected input, pushes a take into a 256 kB ring, plays the preview ring out as the mixer's "Preview" channel |
| `tools_io` (prio 5, PSRAM stack) | drains a take to the card, keeps the preview ring filled, runs one file job at a time and services the first two between a job's chunks |
| LVGL / CDC | ask and poll: nothing waits for the card |

## Inputs

- MIC (`RecInput::Mic`) = codec 0, LINE (`RecInput::Line`) = codec 1, each on
  the input the Mixer's PAIR connects there: the button names it from core
  2.44's `IInputPairs::inputOn()` -- MIC and LINE 1/2 in `Mic + 1/2`, MIC and
  LINE 3/4 in `Mic + 3/4`, LINE 3/4 (codec 0, LINE1) and LINE 1/2 in
  `1/2 + 3/4` -- and follows a PAIR changed while it is open (crosspad-recorder
  0.9.0). The take is the codec's block after the input gain
  (`input_gain_apply()` in `read_codec_block()`). PADS = the pads tap
  (Sampler and Pitched as mixed, the same a song bounce records -- never the
  inputs or the song, whatever the Mixer sends to USB rec), USB = what the computer plays
  into the board over USB audio, tapped where the mixer's USB channel reads it
  (`UsbPlaybackInput::read`), before its fader, an underrun's shortfall as
  silence so a take keeps the host's time. MIC+LINE (`RecInput::MicLine`, an
  Arrange audio track's input; the Recorder does not cycle to it) is both
  codecs' blocks of one render summed, clipped to int16: the mics' block is
  held until the line-in's comes, and one whose line-in is never read goes in
  alone at the next block, so the take keeps the song's time. USB exists only in the USB audio
  profile: `inputAvailable()` says so and a take is refused outside it. Selecting MIC or LINE claims
  that codec's ADC for its input (MIC+LINE both) (`audio_route_claim_adc`, the same claim a
  monitored IN channel makes -- see audio-routing.md); `releaseInput()` (the
  Recorder closing) lets go, and the capture preset's choice comes back once
  nothing else holds it. Not persisted.
- The ADC block is taken where this render already read it: the IN1/IN2
  mixer channels (`IdfI2sInput::read`) in the CDC profile, the USB bridge's tap
  in the USB audio profile, or a read of its own there when USB captures the
  pads bus (`IdfAudioModule::readCodecsForRecorder`). Never a second read of
  one codec in one block.

## With the song (SYNC) and into it (ARRANGE)

A verse over the beat: with SYNC on, REC plays the open song (Arrange's
project, the sequencer with it when the song's SYNC is on) from bar FROM, and
the take lines up with it -- its frame 0 is that bar. FROM follows the song's
loop region until it is turned. The time reads the song's bar.beat.

- `ISampleTools::startRecordingWithSong(bar)` arms the take on `tools_io`,
  then asks `ISongEngine::play(bar)`: armed first, so the take is listening
  before the song can sound. `stopRecording()` stops the song with it.
- The render marks where each block began in the take
  (`sample_tools_rt_block_begin()`, first thing in `processMixer()`); after the
  mixer, `rtSync()` asks `ISongEngine::framesPlayed()`. The first block it is
  not 0 in is the block the song started in, so that block's first input
  frame is what came in with the song's first frame -- whichever side of the
  mixer the ADC was read on (IN channels in the CDC profile, after the mixer in
  the USB one). `framesPlayed()` must be seen at 0 once after arming, so a
  playback from before the take is never taken for its start.
- Until then `tools_io` throws the lead-in away and keeps the newest
  `kSyncKeep` (16384) frames in the ring. Frame 0 is that frame plus
  `kCodecDelayFrames` for MIC and LINE -- DMA out, the codecs' filters, DMA
  in -- and plus nothing for PADS, which never leaves the mixer. The delay is
  **not measured yet** and is 0, so a sung take sits late by it (estimated
  5-12 ms from the 4 x 64-frame DMA each way). To measure: a song of one click
  per beat, `STOOL IN LINE`, `AUDIO_ADC 1 LINE1` (codec 1 then hears the
  phones' DAC loop), `AUDIO_MUTE 0 1`, `STOOL SYNC 0`, and the click's lag in
  the take; check the phones' loop level with `STOOL` peaks first, it can be
  silent. Needs the board to itself.
- A song that cannot play (no blocks and no length, a bounce running) fails
  the take at once; one not playing 3 s after the request, or not sounding
  after 12 s, fails it then.
- **Into a song already playing** (PLAY on the transport bar): it is not
  started again -- that would restart it from FROM under the player. The
  take goes in at the next bar line the song reaches: `rtPunch()` takes the
  song frame the block began on from `ISongEngine::positionFrames()` and the
  bar length the worker looked up when it armed, and frame 0 is that many
  input frames into the block (to the loop's end and its wrap if the line is
  past it; a song that ends first fails the take). `RecStatus::fromBar` is
  then the bar it went in at, not FROM. No count-in.
- Sent by Arrange to fill an audio track, the Recorder opens with SYNC on and
  FROM where Arrange's playhead stood (`ArrangeFocus::bar`).

**PRE** counts one or two bars in before a take with the song
(`startRecordingWithSong(bar, beats)` -> `ISongEngine::playCountedIn`): with
the song's SYNC on the metronome counts (the scene waits in PreBeat, as before
a sequencer take) and the song starts when the clock moves, on the sample --
mid-block, from how far the clock moved in it; a song alone plays the same
click itself (state `Counting`). Either way frame 0 of the take is still the
song's first frame, and the time on screen counts the beats down
(`countInBeatsLeft()`).

ARRANGE puts the loop between the markers into the song through
`ISongEngine::requestImport(file, start, end, track, bar, lead)`, which the
song engine's worker copies into `<project>/clips/` (named after the take,
numbered when the name is taken) and lays on the track. A synced take goes
where it was sung: the bar under START, with the part of that bar before
START written as `lead` frames of silence, so a START trimmed anywhere keeps
the words on the beat. Any other take goes after the last block of the first
empty track. Track and bar can be turned before PUT; the window says which
blocks would go (those starting inside, and the one reaching in is cut, as a
merge does).

## Files

- Takes: `/sdcard/crosspad/recordings/rec_NNN.wav`, loops `loop_NNN.wav` --
  always there, whichever folder the source was in -- 16-bit stereo at the
  engine's rate, at most 10 minutes.
- **NORM** in the Recorder (crosspad-recorder 0.6.0) brings the
  take on screen up to -0.3 dB of full scale through the same
  `requestNormalized()` job as the Sampler pad editor's NORM: a copy beside
  it, `<name>_norm.wav`, written to a `.tmp` and swapped in, which is then on
  screen with START and END where they were (the length is the same). The take
  itself is never written over. A file already within 0.1 dB of the peak is
  its own answer: nothing is written and the Recorder says so. There is no cap
  on the gain: a near-silent take comes up with its noise (a mic take at
  -42.2 dBFS came to -0.30, x125).
  On an Arrange block opened by CLIP EDIT (crosspad-recorder 0.7.0) the copy
  lands on the track at once: the block plays `<name>_norm.wav` from then on,
  same bar and same part of the file, as one commit Arrange's undo takes back.
- **FX** in the Recorder (crosspad-recorder 0.8.0, core 2.28),
  beside FROM, opens a window of effects -- the first two, a row each for the
  next: PITCH -12..+12 st, either **KEEP LENGTH** (the default: core
  `SampleFx::keepLength`, `TimeStretch::run` to the pitch's ratio into
  `<out>.str`, then `repitch` back to the take's own frames, so a clip on a
  song's bars stays on them) or **TAPE** (varispeed, as a pad pitches: up is
  shorter); LOW PASS and HIGH PASS, the third-octave steps of `kFxCutoffs`.
  LISTEN plays the marked loop through `previewAs()` -- the pitch always as
  TAPE, the audition cannot stretch. APPLY is `requestRendered()` into a copy
  beside the take, `<name>_p+3k_h125_l8000.wav` (what was set, `k` for the
  length kept), which is then on screen with the markers on the same sound
  (moved by the ratio after TAPE); the FX go back to neutral. On an Arrange
  block it lands on the track as NORM's does, and after TAPE the block's
  `offset`/`frames` follow the ratio and its `bars` what it now lasts, up to
  the next block. KEEP reads the card twice and writes it twice (~30 kB of
  working memory from `allocRingSegment()` for the run).
- **FX** in the Slicer (crosspad-slicer 0.5.0), beside ATK: PITCH
  with the length kept, LOW and HIGH PASS (the Recorder's steps), LISTEN (the
  loop round through `previewAs()`, the pitch on top of PITCH's, as
  varispeed) and APPLY: `requestRendered()` into a copy beside the file,
  `<name>_p+3k_h125_l8000.wav`, which the Slicer then works on -- every cut
  where it was (the length is kept, so no cut moves), PITCH and ATK as they
  were, the loop's end bounded by the copy's length again -- so the pads and
  KIT play the processed loop. TAPE is left out on purpose: PITCH on the
  screen is the varispeed, played in real time. NORMALIZE runs last, on the
  render (`requestNormalized()` to -0.3 dB, `<name>_norm.wav`, as NORM; a file
  already there is its own answer), so the copy peaks at -0.3 dB with the
  filters on. APPLY and KIT show the Sampler's PROCESSING box -- the pad
  editor's, on top of the picture: "PROCESSING 38%..." (NORMALIZING, SAVING
  KIT), what is being made, a thin bar -- while the card works; the info line
  says what came of it. The pads stay off the card meanwhile.
- **KIT** in the Slicer asks first (`ui_modal_choice`, the Sampler's
  dialog): **User kit** -- a PERF kit of the user's own
  (`/crosspad/kits/<name>`, category `user`, `own`; `SliceKit::perf`, core
  2.44), every slice its own WAV (`SAMPLES/01.wav`..) with PITCH and ATK and
  the slice's fades written in, so it plays as any sampler kit and the pad
  editor edits it -- or **Slice kit** (SLICE, one WAV, a region a pad, as
  before); then the name.
- **SNAP** in the Slicer (on by default): every cut onto the zero crossing of
  L+R before it (`requestSnapped()`, core 2.44: a 20 ms window before each
  cut, 1 ms after when there is none, one 4 kB read a cut;
  `SliceMath::zeroCrossing()`), so a slice does not start mid-wave and click
  and the hit it was cut for keeps its attack. Asked after every change of the
  cuts -- HITS, EQUAL, +, a FREE cut, MOVE once let go -- and taken only if the
  cuts are still the ones asked about; snapping a snapped cut moves nothing. A
  first cut snapped before the loop's start takes the start with it. FREE
  plays from a cut with ATK (`slice_sound()`), as the kit's pad does, so a cut
  played there neither clicks nor hides ATK.
- FILES in both apps browses the whole card (`requestFolder(dir)` ->
  `CardFolder`): folders, then WAVs, by name, 40 rows a page, never above
  `/sdcard`, at most 2000 entries. The names are one block of text with an
  offsets table, not a string each: allocations up to 64 B come from internal
  RAM (`SPIRAM_MALLOC_ALWAYSINTERNAL`), and a folder of a few hundred samples
  would have emptied it. A WAV the reader cannot take (24-bit, compressed)
  says so when it is opened.
- **Slice kits** (core 2.5, `SampleToolsEngine::doExport()`): KIT writes
  `/sdcard/crosspad/slicer/<name>/kit.json` (the kit browser's SLICE list is that
  folder) and **one** WAV,
  `SAMPLES/loop.wav` -- the loop from its first slice to its end, as it is --
  with `"engine": "slice"`, `"category": "slice"`, and each pad a region of
  that file (`{"sample": "loop.wav", "start", "end"}`, frames into it),
  colours round the wheel.
  The native engine plays a Slice kit as any sampler kit, with three things of
  its own: every pad in choke group 1 (one voice: a hit cuts the one sounding;
  `buildTakeSpec()`), the kit's `"pitch"` (-24..+24, PITCH in the
  Slicer, varispeed: a slice an octave up is half as long, and a known tempo
  moves with it) added to every pad's in real time, and each pad's
  `"attack"` (ms, the Slicer's ATK) as the voice's fade-in
  (`KitPad::attackMs`). A slice's end fades over `SamplerEngine::kTrimFadeMs`
  (5 ms) -- any one-shot whose trim ends inside its file does. Each region is
  a head of its own (`SampleStore` keys by path and start).
  The Slicer still plays everything -- PLAY, the pads, FREE -- through
  `previewAs()` with `SliceMath`; `previewFrame()` stays in the file's frames,
  so the playhead and FREE's cuts land on the file whatever the pitch.
- The kit browser lists Slice kits under SLICE (`KitEngine::Slice`), between
  DRUMS and PITCH; older Slicer kits (`"category": "user"`, author "CrossPad
  Slicer") stay under USER (`KitInfo::isUser()`).
- Kits from scratch: PERF's **+ NEW KIT** and DRUMS' **+ NEW DRUMS** write an
  empty kit (sixteen coloured pads, no samples) to `/crosspad/kits/NEW KIT[ n]/`
  or `/crosspad/drums/NEW DRUMS[ n]/` and highlight it on that list; the pad
  editor in the Sampler fills it. Both carry `"own": true` (`KitInfo::own`,
  core 2.32), which `isUser()` reads: the card renames and deletes them, and the
  drum kit keeps `"category": "drums"` for its 12 voices. A kit of one's own
  from before (category `user`) stays under USER.
- A new kit joins the kit list at once through `PortableKitLoader::addKit()`,
  into the `kSpareKits` (8) slots a scan leaves: growing the vector would move
  every `KitInfo` the GUI holds. With no slot left it appears after a rescan.

## Sampler pads: reverse, pitch and filters

The Sampler's pad editor (`crosspad-sampler/src/ui/pad_editor.cpp`, laid out as
the Recorder) sets REV (the sample backwards), PITCH (-12..+12 semitones,
varispeed), HP and LP (12 dB/octave Butterworth, stepped in thirds of an octave,
OFF at the open end) per pad; the kit keeps them as `reverse`, `pitch`, `hp`,
`lp` (written only when set). The prebuilt sample player has no transpose,
filter or reverse, so a pad with any of them plays a copy of its sample
rendered once: `crosspad::renderedSamplePath()` names it,
`<sample dir>/.fx/<stem>_p+3_h160_l4000_r.wav`, one file per setting, and
`requestRendered()` makes it on `tools_io` (read backwards when reversed --
`readPitched` with a negative ratio -- pitched, then `SampleFilters`, at the
file's own rate; written
as `.tmp` and renamed, never over a file that exists). The platform's pad setup
(`pad_file()` in `components/crosspad-platform-idf/SamplerPlatformPort.cpp`) plays the copy when the card has it, the
trim mapped onto it -- mirrored when reversed, the END marker being where the
copy starts, and scaled by the pitch ratio -- and the sample as it is until
then. A reversed pad is drawn as it is heard, in the editor and in the Sampler
panel's small picture (`waveform_view_set_reversed()`): mirrored, START on the
left over the sample's end, END on the right, the playhead left to right, and
START/END turned the way it is heard -- the kit still stores the trim in the
sample's own frames.

Who renders: the editor, once PITCH/HP/LP have been still for 400 ms (PLAY and
LOOP apply them live through `previewAs()` meanwhile), then reloads the pad; the
kit load task (`render_fx()`), for every pad it reloads whose copy is missing
-- a first load, a kit edited on a computer -- before any pad is swapped; and
closing the editor mid-render hands the pad to the load task. Traps: copies
accumulate, one per setting tried; and a copy the player has open (its handle
cache keeps files after a reload) cannot be deleted until it lets go -- a
restart does. Folders starting with a dot are not listed by the card browser.

## Hits

`SliceMath::onsets()` works on a level envelope (one value per 256 frames,
0.35 dB a step) that comes with every overview, so a sensitivity turned on the
encoder answers without reading the card. A hit is a rise of 30 dB (sensitivity
0) down to 3 dB (100) within 50 dB of the loop's loudest part; the cut goes one
hop before the hop that shows it, so it lands up to ~10 ms early and never late.
On a synthetic file of 8 known hits all 8 were found at every sensitivity.

## HIL

`STOOL` over CDC: `IN MIC|LINE|PADS`, `REC`, `SYNC bar [beats]` (a take with
the song, after that many beats of count-in; `song=`, `wait=`, `from=` in the
report), `DIR [path]` (`dir=`, `n=`, `folders=`, `names=` a|b/|...), `STOP`,
`RELEASE`, `LIST`,
`OV path [a b]`, `ONSETS sens`, `EQ n`, `PLAY path a b loop`, `SLICE path a b`
(a slice's audition, with ATK and PITCH), `ATK ms`, `PITCH st` (for SLICE and
EXPORT), `PSTOP`,
`SAVE path a b`, `EXPORT name` (the slices from the last ONSETS/EQ),
`HANDOFF path a b`. Bare `STOOL` reports; names come last.
`ASONG IMPORT path a b track bar lead` puts a region into the open song.
