# The sampler engine

Sixteen pads, streamed from the card, pitched in real time. Lives in
`crosspad-core` (`crosspad/sampler/`), portable and host-testable; the board
supplies only bytes and memory through `crosspad-platform-idf/SamplerEnginePort.cpp`.

It is the default engine. The prebuilt streaming library, `libcrosspad_ply.a`
(`components/ml_sample_stream_player_lib/`), is an optional build
(`CONFIG_CROSSPAD_SAMPLER_MARCEL=y`); with it compiled in,
`SMPL_ENGINE native|marcel` hands the kit from one engine to the other. Its
memory is chosen by the caller: `SampleStreamPlayer_Init(32 kB, 16 kB)`
(`main/audio/engine_handoff.cpp`) is a first block per sample and 16 kB
stream blocks, taken from the same arena as a kit is set up
(`SampleStreamPlayer_MallocSlowMemory`; 1343 kB for HIL SINE A's 16
samples), and its small state (`MallocFastMemory`, ~6 kB) from internal RAM
while 48 kB stays free there.

## What it is built for

1. **Memory follows the kit.** Heads and stream blocks are taken as a kit
   needs them and given back (`BlockPool`, below). The board's whole PSRAM
   pool is 6.2 MB, and the UI, LVGL's cache, the kit list and the song rings
   live in what the engine does not hold.
2. **A note-on never reaches the card** — it reads the sample's resident head.
3. **Realtime pitch.** A pad's `pitch` is a phase increment, not a copy of the
   file rendered to the card.
4. **Every sample plays to its end**, whatever its length.
5. **It is source** in crosspad-core, with host tests.

## The one idea: a resident head

Every sample keeps its **first frames in RAM**, read once when the kit loads.
A note-on therefore never touches the card — it reads the head — and the
streamer has the whole length of that head to deliver what comes after.

A sample shorter than its head is **complete**: it never streams, and neither
the pool nor the card hears about it again. That is every short drum hit,
which is most of what a pad is struck for.

Measured over a card of 23 kits (1783 WAVs, 771 MB):

| head per sample | RAM for a whole kit | head start for the streamer | samples held whole |
|---|---|---|---|
| 64 kB | 0.74 – 1.56 MB (median 1.05) | 0.37 s | 17 % |
| 128 kB | 1.10 – 2.89 MB | 0.74 s | 36 % |

`HeadPlan` (`SampleStore.hpp`) resolves
the head against a per-kit budget by halving (0.37 s → 0.18 s → 0.09 s) rather
than solving for an exact fit, so the number a person reads is a time they can
reason about, and it never refuses a kit.

**The head begins where the layer does** (core 1.61): a slot is a
file *and its trim start* (`SampleStore::load(path, headFrames, start, end)`,
`trimStart()`), its head the frames from there on, and the stream window opens
after it (`headStart + headFrames`). A head taken from the file's first frames
would leave a START past it unplayable: the voice's first frame would be in
neither the head nor the window, which the streamer fills forwards from the
head's end, and the render would end the voice in its first block with an
underrun. Slices of one file are a head each; the same file from the same
start on several pads is one. `plan_heads()` sizes a head on what is left of the
file after the start.

## How it works

**`SampleStore`** — what of each sample is in RAM and what is still on the
card. `load()`/`unload()` are the loader's; everything the render thread
touches is `const` and reads only what `load()` published; `serviceIo()` is
the streaming thread's.

**`BlockPool`** — memory that is given back. Two sizes for two jobs: a **page**
is what the heap is asked for and given back (96 kB, one voice's whole ring, so
a ring stays one span and one card read refills several of its blocks), and a
**block** is what a voice reads through (16 kB = 4096 stereo frames = 93 ms at
44.1 kHz; `kBlockBytes` in `SamplerEnginePort.cpp`). A streaming voice takes a
ring of six blocks when it starts and returns them when it ends; an idle kit
holds none. At most 16 pages, 1.5 MB of streaming (`kMaxPages`). `trim()`
hands back every page with nothing in it — that is the "share the RAM between
operations" half, and it is what lets the Recorder, the Slicer and Pitch Play
have memory the sampler is not using.

16 kB blocks, not 4 kB, because the wall on streaming polyphony is the
overhead of a read, not the card's throughput: at sixteen voices a 4 kB read
averaged 1784 µs and peaked at 12704 µs (`SMPL_LAST` lat avg/max), so one
streamer task issuing them in series could not keep sixteen voices fed. Six
blocks a ring, not four, because the card is the kit loader's and the UI's
too: right as a kit lands under play, the streamer's opens waited 180–200 ms
behind theirs. Six blocks are 465 ms of lookahead at 44.1 kHz stereo against
279 ms with four; at 20 hits/s across kit swaps that was 1 underrun in 30
swaps against 10 in 18.

`acquire()`/`release()` touch one intrusive free list under a spinlock held for
a few instructions and never reach the heap, so they are safe on the render
thread. `reserve()`/`trim()` reach the allocator and belong on a worker.

**`StreamWindow`** — the part of a sample on its way from the card, for one
voice. A fixed ring of pool blocks over a sliding run of frames, built around
one rule that removes every pointer race:

> **Which block holds frame `f` is arithmetic, not state.** Block
> `n = (f - streamStart) / blockFrames`, in ring slot `n % blocks`. Slots are
> acquired when the voice starts and released when it ends; their addresses
> never move while it sounds. The streaming thread only ever writes *into* a
> slot, never re-points one.

So the two threads share exactly one monotonic number — `validUpTo()`, the
first frame not yet delivered — published with a release store and read with an
acquire load. The render thread reads below it and never waits; the streaming
thread fills above it and never asks permission. Overwriting is bounded by the
same arithmetic, and the daylight is 8192 frames against the 64 a render pass
consumes.

**`SampleCursor`** — a 32.32 fixed-point phase and 4-point cubic Hermite, the
same arithmetic `PitchedInstrument` already runs 32 voices of. A pad's `pitch`
is an increment, not a file, and the same accumulator plays a 48 kHz or 22 kHz
file at the right speed without resampling it.

**`SamplerEngine`** — a generator `IAudioNode`. `mix()` adds into the stereo
float block the mixer has just cleared, and allocates nothing, takes no lock,
opens no file and writes no log. The budget is **1.45 ms for 64 frames at
44.1 kHz**, and `MetronomeNode` counts the sequencer's beat in frames
*rendered*, so a render that overruns does not thin the sound — it takes the
pulse with it.

### Three threads, one writer per arrow

The ownership is in the voice states, and a voice moves one way only:

```
Free     --(note thread: claim, fill, publish)-->      Playing
Playing  --(render thread: it ended, or was stolen)--> Finished
Finished --(streaming thread: give the blocks back)--> Free
```

Each arrow has exactly one writer, so the only atomic operation is the claim,
where two note sources could race for a slot. In particular **the render
thread never gives a block back and never closes anything** — it marks the
voice Finished and moves on.

### A hit before its velocity: arm, fire

A v2 STM measures a hit's velocity over a press window (`pressDebounceMs`, 6 ms
on hardware) and sends the note-on at its end. From STM 1.11 it also sends `PAD_TOUCH` (`F0 7D 22 pad 1 F7`) when the window opens and
`... pad 0 F7` when it closes without a hit, to the ESP only. The pad logic's
`onPadTouch()` arms the voice the hit will play:

```
Free  --(note thread: claim, fill all but the level)--> Armed
Armed --(the velocity: level, cut, room, publish)-->    Playing
Armed --(touch left / other hit / mutation / 300 ms)--> Finished
```

An armed voice is never heard and cuts or steals nothing; `fire()` does the
velocity's part of a note-on (and picks the layer again where the pad splits
by velocity). Whoever ends an armed voice takes it with a compare-and-swap.
Every finger's hit -- armed or not, v1 edge or v2 note-on -- goes from the
pad logic straight into the engine on the UART task (`player.playPad`),
skipping the event queue; the sequencer's playback and MIDI keep the event.
What it buys, measured (pad-preload NOTES, 16.10.2026, Buffer 4.4 ms): UART
note -> voice in the engine 375/837 us p50/p95 -> 270/360 direct -> 213/303
armed; note -> DAC p50 ~5.45 -> ~5.3 ms quiet, ~5.94 -> ~5.58 ms (p95 12.5 ->
10.9 ms) under a 25/s storm. The press window itself is the velocity's
measurement and stays: it is the larger part of a finger's latency. STM 1.12
can end it once the press has peaked instead of at its full length (Settings ->
Analog / Velocity -> Hit window, Mode Adaptive; `REG_CTL_BTN_HIT_*`, live with
`STM_REGW`); the fixed window stays the default.
A v1 STM sends only digital edges, no velocity: the edge is the hit, at 127.
`SMPLENG armed= fired= disarmed= expired=`, `LAT_TRACE` arm/fire stamps.

### Stealing

Above the voice cap the oldest voice is asked to fade over `kStealFadeMs`, in
the slot it already occupies — its phase and envelope are in there. The note
that stole it therefore needs a different slot, and `Params::maxVoices` is held
`kFadeSlots` below `kMaxVoices` to guarantee one. A hard cut at an arbitrary
phase is a click at full scale, the one artefact a sampler cannot hide.

### Over the block: shedding

Over 85 % of the block on average a note takes a voice's place instead of
adding one (`setOverloaded()`). Over the *whole* block that is not enough:
the render no longer waits for its DMA, the event task that would release a
note gets no CPU, and a loop never ends on its own. So `audio_task.cpp` lets
one sounding voice go (`SamplerEngine::shedOne()`; faded as a steal,
counted in `shed=`) every
`kShedVoiceEvery` (8) passes while the average stays over the block. Measured
with an artificial 1000 us added to every pass: 16 loops came down to 5 in
~5 s and CDC answered throughout; without it, sixteen pitched loops hold
core 1 until the task watchdog fires on IDLE1.

### The render, frame by frame

Every run of a voice goes through one of three out-of-line kernels in
`SamplerEngine.cpp`, each counted once a run so nothing is tested a frame:
`walkLevel<Ch, Loud>` (a frame a frame), `walkPitched<Ch, Loud>`
(Catmull-Rom off four taps, coefficients in integers by `hermite4i()`, the
same number as the float form to the bit for int16 taps), and `playLevel<Ch>`
for the one run the quiet cut may end. A loop keeps no loudness at all
(nothing reads its quiet count or audible end). A pitched streamed loop keeps
its run walk past its first lap: `SampleCursor::runAt()` takes the lap offset
(read frame by frame it costs ~170 us a voice a block). Board, 2026-10-23, 16
stereo loops, mixer pass: pitched +7 680 us, unpitched 354 us. What that is
(Xtensa PMU, 16 pitched voices): ~60 % instructions, the rest waiting on PSRAM
for code (I-stall ~90 us) and samples (D-stall ~85 us). Not levers: the
cache's autoload (D hangs the board, I is slower), a D-cache preload of the
next voice (-10 us, a ROM call IDF says not to use), the kernels in IRAM (no
change), octal PSRAM at 120 MHz (experimental, temperature-bound).

### 16 levels of pitch: a note is a key, not a pad

`noteOn(pad, velocity, transpose, key)`: `transpose` is semitones on top of the
pad's own `semitones`, for that note only; `key` is which hit it is. The
per-pad retrigger (`monoPerPad`) and a Hold release (`noteOff(pad, key)`) go by
the key, which defaults to the pad -- so a plain kit behaves as before. The
Sampler's 16 levels in pitch mode strike the source pad from every pad with
`transpose` = the pad's index and `key` = 16 + the pad, so pads 1, 5 and 8 at
once are a chord of three voices, and each lets go on its own pad's release.
The two fields ride `SamplePlaybackData` (`transpose`, `key`); a pitched kit
moves the pad's note instead, and the prebuilt engine ignores both.

### Pan: balance, as the mixer

A pad's pan is a balance -- unity at the centre, turning takes level from the
other side only -- the law the mixer's channels use. Equal-power would put
every centred pad 3 dB under the same sample in the Slicer's audition and
through any mixer channel, so a kit made in the Slicer would sound quieter
than its slices. The pitched instrument pans equal-power.

### The quiet cut

A one-shot that has gone inaudible stops instead of streaming the rest of its
file. This mattered more than anything except the arithmetic: card traffic
40 MB → 8.3 MB, and the STM buffer drops and USB MIDI drops that came with that
traffic went to zero.

A rest inside a phrase can be longer than 200 ms (DREAMSCAPE pad 4 has a
221 ms rest at 1.05 s, with 1.8 s still to play after it), so silence is
judged on the file itself, not on what
the pad's gain makes of it, and where a voice found the sound ending is kept
per sample (`SampleStore::audibleEnd()`): every later hit is trimmed to it in
`noteOn()`, so it stops exactly there, at any velocity, and one whose sound
fits in the head streams nothing at all. The wait (`silenceMs`) is 1.5 s and
only matters until a sample's first hit has run past its sound -- to the end
of the file, or into 1.5 s of silence, which is then taken as the end. A rest
longer than that is still cut, the same way every time. Loops are never cut.

### Loop pads: launch and mute

A pad in Loop mode (`playbackMode` 2, "toggle" in the crosspad.app export) is
not a note but a clip launcher. Every hit -- a finger, the sequencer's playback, a host's
MIDI note -- goes through `noteOn()`/`fire()` and toggles; a release never
reaches a loop.

| The loop is | Clock (metronome) running | Clock stopped |
|---|---|---|
| not there | queued, starts **on the next bar line** | starts at once |
| waiting for its bar | the launch is cancelled, unheard | -- |
| sounding | **muted**: silent, still advancing and streaming | stopped (the steal fade) |
| muted | unmuted at once, where it is (no restart, no re-quantise) | unmuted at once |

The clock stopping ends every loop, muted and waiting ones too; a kit change
and `allNotesOff()` are as before. A mute is a per-voice flag the render ramps
over `Params::muteMs` (5 ms), so neither edge clicks; the muted voice keeps
its slot and its ring, and counts against the caps like any other.

The launch is placed to the frame in the render, not on a timer: the engine
asks a bar clock (`setBarClock()`) once a block for the frames to the next bar
line, and a queued voice is rendered from that offset in the block the line
falls in. On the board the clock is the metronome (`MetronomeNode::framesToBar()`,
the count-in's remainder while counting in), which the mixer renders after
the sampler -- so the position it gives is the one at the start of the
sampler's block, and the loop's first frame is the click's downbeat frame.

LEDs: a waiting or muted loop shows its playing colour dimmed
(`PadLedController::notifyNoteDimmed()`, `kDimmedLevel`); the launch lights
it fully. `SMPL_LOOPS` prints each pad's state (`.` idle, `q` waiting, `P`
playing, `m` muted), `queued=`/`launched=` and the clock's `to_bar=`.

Checked on a v2 (19.10.2026, a HIL_LOOP fixture kit with two Loop pads,
`PAD_PRESS` over CDC and notes 36/37 over USB MIDI, the clock from `SEQ PLAY`
on an empty scene): all of the table above, 0 underruns, the muted voice still
reading the card. The frame of the launch is held by the host test
(`SamplerEngine: a Loop pad hit while the clock runs starts on the next bar
line, to the frame`), not measured off the board's audio.

## Changing kits while playing

A kit change never stops this engine. A whole take on top of a load would
silence every voice and read every head off the card again, a gap in the
music as long as the kit's heads take to read. So the loader swaps the kit
pad by pad (`native_sampler_swap_pad()`, on the `kit_load` task), and a take
only runs when this engine does not hold the kit yet. The first kit after boot
is taken by the loader itself, before its first pad, and the prebuilt engine
never starts; a kit change that lands while a take is still reading waits for
it rather than taking the prebuilt engine's path. The way back from a pitched
kit and `SMPL_ENGINE native` take the kit after the load.

A swap:

1. The first pad of another kit resolves `HeadPlan` over all of its files,
   while the outgoing kit is still what plays.
2. A pad's new samples are read into the store **beside** the old ones; a hit
   meanwhile plays the old sample.
3. The pad is pointed at them in one copy of its `KitPad`; the next hit plays
   the new sample. No mutation window: hits on other pads are never dropped.
4. A voice already sounding keeps the head it started on and plays to its own
   end. An old sample no pad names is **retired**, and goes back
   (`SampleStore::unloadSlot()`) only once `SamplerEngine::slotInUse()` is
   false -- no pad names it, no voice holds it, and no note is inside
   `noteOn()`, which may have read the pad before the copy (counted, both
   sides sequentially consistent).
5. The last voices of the old kit are waited for on the `smpl_pad` worker
   (every 50 ms, up to 20 s -- a loop plays until stopped; whatever is left
   goes back at the next swap or pad edit). `SMPL_ENGINE`'s `retired=` is how
   many are still waiting.

Old and new heads share the arena for the length of the swap. When a head
does not fit, `build_pad()` halves it down to HeadPlan's floor rather than
leave the pad without its sample; a sample that got a short head streams
sooner until the kit is loaded again.

The loader's wait for a sounding pad (`padSounding`, the prebuilt engine's
mechanism) does not apply here: the native engine never takes a pad apart
under a voice, so the platform answers false for it.

## A song's kit tracks

A song's kit tracks all play through this one engine, in turn: the live kit
track's kit is the kit it holds ([audio-song.md](audio-song.md#kit-tracks-a-kit-each-on-one-kits-engine)).
There is one kits' engine since core 2.42: an engine per kit track would
halve every kit's voices and heads, and its kit could not be edited from the
track's seq view.

## Measured

One v2 board, kit CLASSIC (16 pads, 6.8 MB), speakers muted, 200 pad hits at
20/s through `STM_VPAD`. Budget is 1450 µs.

| | native |
|---|---|
| render, worst block | **1122–1290 µs** (77–89 %) |
| µs per voice | **117** |
| render under sequencer + pad storm | **1230 µs** (85 %) |
| arena held | **1437–2573 kB, following the kit** |
| realtime pitch | **a phase increment, 0 bytes read** |

`MetronomeNode` counts the beat in frames rendered, so a render that stays
under the block, sequencer and pad storm included, keeps the pulse.

**Realtime pitch, shown rather than asserted.** PITCHTEST, one 48 kB sample on
all sixteen pads: `SMPL_PITCH 0 12`, then `-12`, then pad 7 to `-5` — and
`read_kb=46` before and after all three. Not one byte off the card, and the kit
is never handed back.

**A 1303-note soak** (CLASSIC, a scene playing, 1200 pad hits at 20/s on top)
held every error counter at zero — drops, steals, refused rings, underruns —
with `rt_max` between 1140 and 1279 µs and no drift. Memory before and after,
to the byte: `int_free=67483 psram_free=337288` both times.

## Note latency

The note-latency metric (average 1905 µs, worst 5701 in the run above)
measures the system's scheduling, not the engine: the time is the note thread
waiting for its core. The event-bus task runs at priority 4 on core 1, under
the render thread at 19 on the same core. A `portENTER_CRITICAL` on the pool's
free list makes `noteOn()`'s worst case worse (50.7 → 101.6 ms); with the pool
off the note path entirely it stays at 98.6 ms, and a call that does nothing
but write fields cannot take 99 ms. The pool stays off the note path anyway —
the ring is opened by the streaming thread on its next pass, which the 0.74 s
head makes free: work the head makes unnecessary has no business on the one
thread a player is waiting on. Why the note thread waits as long as it does
needs an instrument that separates runnable from running, which this board
does not have.

## Driving it

```
SMPL_ENGINE                 # who holds the kit, and everything below
SMPL_ENGINE native|marcel   # hand the kit over; reloads it on the way in
SMPL_PITCH <pad> <semis>    # a realtime step, the pad editor's own door
ARENA                       # cap_kb / used_kb / owner
```

A live reading (v2 board, 2026-11-08), native engine holding BLACK TESLA with a
Pitch instrument loaded beside it:

```
SMPLENG: engine=native state=2 slots=16 complete=2 head_kb=64 heads_kb=996
         pool_kb=192 pool_blocks=0/12 voices=0/2 lim=0/16
         notes=5 drop=0 steal=0 norings=0 underrun=0 quiet=0 rt_max_us=149
ARENA:   cap_kb=3375 used_kb=1188 owner=pitched
```

`complete=2` — two of the sixteen samples are held whole and never stream.
`pool_kb=192` is two 96 kB pages (the audition's ring and one voice ring, see
below); `pool_blocks=0/12` — nothing is sounding, so the pool holds nothing out.
`ARENA`'s `cap_kb` is what the arena holds plus what it could still take
above the 512 kB floor, so it moves with the rest of PSRAM; `owner` is
`native` or `pitched` (`marcel` or `none` too when the prebuilt streamer is
built).

Fixtures: `crosspad-hil run sampler_poly`, and the host tests
(`BlockPool`, `SampleStore`, `StreamWindow`, `SampleCursor`, `SamplerEngine`).

## Traps

Most of these fail **silently** — no error, no counter, no failed build.

| rule | what breaking it shows |
|---|---|
| no card I/O on the CDC main task, the pad rebuild included | `task=main cause=65`, a crash |
| no polymorphic object in `.ext_ram.bss` | `cause=28 vaddr=0x8` — the start zeroes the vptr |
| **the pool's own tables never inside the arena it hands out** | nothing — rings silently refused, underruns |
| the header cache keeps the name it keys on | nothing — a kit refused as unreadable |
| no memory accumulating across kit changes | nothing — until the arena fills |

- **The memory a pool hands out is not where its own bookkeeping may live.**
  The arena exists so nobody has to trust anyone else's teardown; a tenant that
  keeps its state inside it has quietly opted back in. The pool's tables come
  from the ordinary PSRAM heap.
- **A capability gate is a silent fallback.** With two engines,
  `if (!engine_ready()) return;` means *do nothing* for the engine that is
  present, and a pad cannot be given a sample at all. Every such gate asks
  *which* engine holds the kit, not *whether* one does.

Tests that imitate what a player does — the heaviest kit, one sample on every
pad, turning the pitch knob, changing kits over and over — find these; a
benchmark finds only the ones that crash outright.

## What is still open

- PSRAM on demand. The arena is not a reserved block but the general heap, every block on one list (`engine_arena.cpp`, a 512 kB
  floor kept for the UI); heads are HeadPlan's 64 kB, not 256 kB; and the pool
  reserves the audition's ring and one voice ring at a take, grows a ring for
  every voice that waits for one (`SamplerEngine::ringsWaiting()`, core 2.2.0:
  a voice keeps asking while its head plays) and trims back after 4 s with
  nothing streaming (`pool_follow_voices()`). POP_ICONIC: 1268 → 914 kB held;
  160 hits at 16/s, 7 voices: pool 192 → 576 → 192 kB, norings 0, underrun 0.
- The MIDI+UAC2 profile is unmeasured; switching into it needs a SysEx request
  the board confirms on the encoder while `USB_GUARD` is 1.
- Why the note thread waits as long as it does (above).
