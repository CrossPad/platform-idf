# Pitched instrument: one sample, sixteen notes

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

The kit loader lists a pitched kit like any other.

## What it is

A second sample engine beside the streaming sampler. A kit whose
`kit.json` says `"engine": "pitched"` carries one to four recorded notes
("zones"), held **whole in PSRAM**, and every pad plays the nearest zone
transposed to its own MIDI note. Nothing streams from the SD card while
playing, which is where the polyphony comes from: a voice is an interpolated
read from RAM, not a file refill.

Design and the reasoning behind each choice:
`docs/design/2026-09-03-pitched-instrument-design.md`.

## Where the pieces live

| Piece | Path |
|---|---|
| Engine (voices, Hermite varispeed, envelopes, stealing) | core `include/crosspad/instrument/PitchedInstrument.hpp`, `src/instrument/PitchedInstrument.cpp` |
| Zone storage with allocator callbacks | core `instrument/SampleBank.hpp` |
| WAV reader (stdio, 16-bit PCM, mono/stereo→mono) | core `instrument/WavPcm.hpp` |
| FFT seam + portable radix-2 | core `dsp/IFft.hpp`, `dsp/PortableFft.hpp` |
| Root-pitch detector (Hann, HPS, log-parabolic + DTFT refine) | core `dsp/PitchDetector.hpp` |
| Band-limited rate conversion (OLA 4096, spectral 2× oversample, Hermite read-out) | core `dsp/FftResampler.hpp` |
| Kit JSON (`engine`, `instrument`) | core `kit/KitInfo.hpp`, `src/kit/KitJsonParser.cpp` |
| esp-dsp FFT adapter + factory | core `dsp/EspDspFft.hpp`, `dsp/FftFactory.hpp` (`createFft`/`currentFft`); the board's aligned-PSRAM `FftMemory` is a local in `PitchedKitPort.cpp` |
| Kit → bank (WAV read, resample, root detect) | core `instrument/PitchedKitLoader.hpp` (`PitchedLoadEnv`, `pitchedBuildBank`) |
| Bank swap: render gate, mutation window, old-bank verdict | core `instrument/PitchedInstrument.hpp` (`gateEnter`/`gateExit`, `beginMutation`/`endMutation`, `installBank`) |
| Mixer channel "Pitched" — rt-max-µs timing wrapper only | `components/crosspad-platform-idf/include/PitchedSampleNode.hpp` (header-only), wired in `CrosspadPlatform.cpp` |
| Board's half of the load: arena memory, `/sdcard` prefix, I2S rate, ESP_LOG, status for the CDC verbs | `components/crosspad-platform-idf/PitchedKitPort.{hpp,cpp}` |
| Shared engine PSRAM arena (`multi_heap` over one block) | `components/crosspad-platform-idf/engine_arena.{hpp,cpp}` |
| Prebuilt-streamer start/stop handoff | `main/include/engine_handoff.h`, `main/audio/engine_handoff.cpp` |
| Engine dispatch (which engine a pad hit and a kit load go to) | `components/crosspad-platform-idf/SamplerPlatformPort.cpp` (`platform_reloadPad`, `platform_reloadPads`, `onSamplePlaybackEvent`) |
| CDC verbs `PITCHED_STATUS`, `PITCHED_BENCH`, `ARENA` | `main/hil_control.cpp`; host `crosspad-hil` `knowledge/cdc.yaml`, `verbs.py`, `parsers.py` |
| Fixture kit | `tools/make_pitched_kit.py` |
| HIL scenario | `crosspad-hil run pitched_kit` |

Core tests: `components/crosspad-core/tests` (`test_PortableFft`,
`test_FftFactory`, `test_PitchDetector`, `test_FftResampler`, `test_WavPcm`,
`test_SampleBank`, `test_PitchedInstrument`, `test_PitchedKitLoader`,
`test_kit_json_instrument`); `wav_fixture.hpp` is the shared RIFF/sine writer
the file-reading ones share.

## Kit JSON

```json
{
  "name": "HIL PITCHED", "author": "hil", "engine": "pitched", "type": 0,
  "instrument": {
    "zones": [
      { "sample": "SAMPLES/z57.wav", "root": 57 },
      { "sample": "SAMPLES/z69.wav", "root": "auto" }
    ],
    "pad_notes": [60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75],
    "base_note": 60, "release_ms": 100, "attack_ms": 2, "max_voices": 24
  },
  "pads": [ { "note": 0, "playbackMode": 1, "volume": 100, "pan": 0, "samples": [] }, … ]
}
```

- `engine` absent → the sampler path, byte-for-byte as before. `KitInfo::engine`
  and `KitInfo::instrument` are parsed at list time and load time alike.
- `root` is a MIDI note (int or float) or `"auto"`: the detector runs on the
  loudest 4096-sample window and logs `root <midi> (<hz>, conf <n>) detected`.
  A detected root is never written back into the JSON.
- `pad_notes` must have exactly 16 entries in 0..127 or it is ignored and the
  map is chromatic from `base_note`. `pads[].note` stays the pad index — LED
  and choke code read it. `playbackMode: 1` (hold) is what makes the pad
  release reach the engine; without it the note only ends when the sample
  does.
- `max_voices` is capped by the board at 10 (`PitchedSampleNode::kVoiceCap`,
  set in `CrosspadPlatform_Init`; `PitchedInstrument::setVoiceCap()`, which
  every `setParams()` meets -- a load and the kit editor's Attack, Release and
  Voices alike). Measured in the 16-note chord: ~65-69 us a voice beside a
  pass of ~550 us (CDC) to ~680 us (phone streaming over USB audio); ten
  voices are 84 % / 93 % of the 1451 us block. At 15-16 the render fills the
  block, `crosspad_events` (which plays the notes) starves and a chord loses
  its last note and hangs its release. Above the cap a voice is stolen (see
  below). `PITCHED_CAP n` sets the cap from the next `setParams()`, until a
  restart; the editor's Voices goes up to it.
- **The render holds the polyphony over budget** (`main/audio/audio_task.cpp`):
  `audio_rt` keeps a running average of its pass, I2S waits excluded, each
  pass weighing a quarter; over 85 % of the block it raises
  `PitchedInstrument::setOverloaded()` and a new note takes a stolen voice's
  place instead of adding one, until the average is back under 75 %. The cap
  is a guess at the budget, this is the budget as measured, with USB audio,
  the song and the metronome in it. `PITCHED_STATUS shed=` counts those steals.
- Up to four zones (`kInstrumentMaxZones`). Zone selection is nearest root,
  ties to the lower one. `loop_start`/`loop_end` in samples of the file;
  they are scaled when the file is rate-converted, and so are the two below.
- `start`: where a note begins -- the attack, played once before the loop, so
  a loop placed after the attack keeps it; a file without `start` begins at
  `loop_start` (a one-shot at 0).
  `SampleBank::startFor()` keeps it inside the PCM and short of the loop end.
- `loop_xfade`: frames before `loop_end` crossfaded with the same stretch
  before `loop_start`, so the wrap has no seam and one sample holds for as
  long as the pad does. Cut to the loop's length and to what precedes its
  start (`SampleBank::loopXfadeFor()`); written only when set.
- Mono or stereo 16-bit WAV; stereo is averaged to mono. A file not at the
  engine rate is converted once at load (the sampler engines play a file at
  its own rate, see [audio-engine-rate.md](audio-engine-rate.md)).

## Real-time path

Each voice reads its zone at increment `2^((note − root)/12)` in 32.32 fixed
point through 4-point cubic Hermite, times envelope (linear attack, linear
release from wherever the envelope was), velocity/127 and pad volume; then
equal-power pan into the stereo bus, with `dsps_mulc_f32`/`dsps_add_f32` on
the board. Retrigger on the same note releases the old voice instead of
cutting it. Stealing at `max_voices` (or over budget): a voice already in its
release before a held one -- a held chord note taken out is the steal anyone
hears -- the oldest of those, which fades out over `kStealFadeMs` (5 ms; 2 ms
was a cut on a low note) in its own slot while the new note starts in a free
one — `kMaxVoices` (32) is the slot count, `max_voices` is capped at
`kMaxSounding` (28) so `kFadeSlots` (4) stay free for the fades (under the
board's cap of 10 there are 22); `PITCHED_STATUS steals=` counts them, the
hard cut when every fade slot is busy included. A 16-note chord sounds as 10
voices: the 6 oldest are stolen. Duration scales with pitch, as on every classic sampler.

Measured with `PITCHED_BENCH 20` on a v2 board (44.1 kHz, 64-frame
block = 1451 µs):

| Measurement | µs |
|---|---|
| `voice64_us` — one voice, one block | 49 (87 before the unchecked interior read) |
| `voice64x16_us` — sixteen voices, one block | 723 (1147 before) |
| `fft1024_fwd_us` / `inv` — esp-dsp, PSRAM buffer | 557 / 614 |
| `fft4096_fwd_us` / `inv` | 7128 / 8559 |
| `portable_fft4096_fwd_us` — scalar radix-2, same data | 14184 |
| `fft_maxdiff` — esp-dsp vs portable, unit-scale input | 1.06e-6 |

Sixteen voices cost 0.72 ms of the 1.45 ms block, beside the mixer; a 16-pad
chord through the whole RT path reports `rt_max_us` ≈ 860 (it was 1380 with
the bounds-checked taps). One 1024-point FFT alone costs 0.56 ms, so a phase
vocoder per voice (an FFT and an IFFT every hop, per voice) does not fit on
this core — that is why the spectral work is done once at load and the
per-sample work is an interpolated read. The render loop takes the four taps
unchecked for `1 <= i <= length-3` and falls back to the bounds-checked read
only at the edges; SIMD-ising that loop is the next lever past ~24 voices.

The loop crossfade is done in the render, not baked into the PCM, so a loop
edited while the bank plays (`editZone()`, the Sampler's pitched editor) needs
no rebuild. Inside the last `loop_xfade` frames a voice makes a second read one
loop span back and mixes the two on an equal-power curve (`1.5t - 0.5t^3`,
sin(t·π/2) to within 0.25 dB). Outside them the voice costs what it did; inside,
one more four-tap read. Host test: a ramp looped plain jumps by the whole span
at the wrap, crossfaded it never steps by more than a few LSB.

## The two engines share one PSRAM arena

The prebuilt streamer's memory is chosen by the caller:
`SampleStreamPlayer_Init(32 kB, 16 kB)` (`engine_handoff.cpp`) takes 6484 B
of state in 25 blocks (`SampleStreamPlayer_MallocFastMemory`), and its sample
and stream blocks come from the arena as a kit is set up
(`SampleStreamPlayer_MallocSlowMemory`; 1343 kB for HIL SINE A's 16 samples,
measured 2026-11-07 -- the hooks count them and `EngineHandoff` logs the sum on
every start). Only one of the two engines is ever current, so they take turns
owning that memory.

`components/crosspad-platform-idf/engine_arena.cpp` allocates **one** PSRAM
block of 4608 kB at boot (sized so the UI and WiFi keep their PSRAM beside it),
before `SampleStreamPlayer_Init()`, and registers an
IDF `multi_heap` over it. `SampleStreamPlayer_MallocSlowMemory/Free` serve from it, and
so does the pitched bank (`SampleBank`'s allocator *and* the resampler
scratch). An arena that runs dry falls back to `heap_caps_malloc(SPIRAM)` with
a WARN rather than failing — nothing outside the arena ever moves into it.
Blocks are handed out 16-byte aligned, which is what esp-dsp's `aes3` FFT
kernels need to read the bank PCM and the resampler scratch in place.

`main/audio/engine_handoff.cpp` is the switch:

| Direction | Sequence |
|---|---|
| → pitched kit (pad 0) | `marcel_engine_stop()` → `pitchedKitLoad()` builds the bank from the arena |
| → sampler kit (pad 0) | `pitchedKitUnload()` → `engine_arena_reset()` → `marcel_engine_start()` → the prebuilt streamer's per-pad path |

`marcel_engine_stop()` lowers `marcelEngineReady()` first, then runs all-notes-
off, `WipeOut()` and `SampleStreamPlayer_DeInit()` under
`SamplePlayerMutationGuard` — the gate waits out an in-flight
`SampleStreamPly_Process()`, the file mutex waits out an in-flight
`SampleStreamPlayer_BkgProc()`. `smpl_stream_ply_ready()` is that flag, and
every call into the prebuilt library is gated on it: `MarcelSampleNode::mix()`
returns silence without a library call, `AudioModule_BG_Process()` reads the
flag *inside* the file mutex (so a refill that sees it up is holding the lock
the teardown still needs), and the `platform_*` player callbacks in `SamplerPlatformPort.cpp`
plus `SMPL_POLY`/`SMPL_PREVIEW` refuse while it is down.

**`engine_arena_reset()` reclaims whatever `SampleStreamPlayer_DeInit()`
leaves**, in one step, so the handoff does not depend on what the library
hands back through the Free hook (with no kit set up it returned all 6484 B in
25 blocks); a fast block it left is freed by the port
(`sample_player_port_forget_blocks()`). `Init()` after `DeInit()` plays again
-- `kit_churn` through HIL PITCHED and back under pad hits, 2026-11-07.

Numbers on a v2 board (arena 4608 kB, usable 4605 kB; the pitched
line is from the 5424 kB arena, the zones' size does not depend on it):

```
ARENA: cap_kb=4605 used_kb=4252 owner=marcel      # launcher, sampler kit (2026-10-03)
ARENA: cap_kb=5421 used_kb=861   owner=pitched    # HIL PITCHED, 2 × 5 s zones
MEM: … psram_free=210796 psram_largest=167924     # launcher after app_churn, BLE on (2026-10-03)
```

`ARENA` is its own verb because `MEM` cannot see inside the block: to the
general heap the whole arena is one allocation whichever engine is up.

The arena is a whole number of TLSF size classes, less the 16 B the heap
adds in front: between 4 and 8 MiB a class is 128 kB, and TLSF keeps the
rounding, so 4720 kB and 4656 kB both cost 4736 kB (the same 326 kB left
outside on a v2 board). At 4608 kB (2026-10-03, v2) the prebuilt streamer holds 4252 kB, and
the ~350 kB left is what the song engine's and the sample tools' rings borrow
(Recorder take 256 kB, bounce 128 kB, song playback 32 kB, audition 32 kB,
measured with `ARENA`); a ring that does not fit goes to the general heap above
the UI's reserve. A pitched bank's budget is 4605 − 256 kB. PSRAM
*outside* it is what LVGL, the kit list and the waveform cache run on; an
arena of 5.5 MiB (233 kB more than the streamer asks for) leaves 306 kB
outside, too little: `lv_realloc` cannot grow and the board boot-loops on
core 1 before the kit scan finishes. One contiguous block also fragments the
pool differently from 56 separate ones: the launcher reports `psram_free`
334 kB / largest 200 kB, where 56 interleaved blocks left ~450 kB / 393 kB.

## Load path and memory

`platform_reloadPad(0)` on a pitched kit stops the prebuilt streamer and then calls
`pitchedKitLoad()` on the kit-load worker (8 kB PSRAM stack, core 1, priority 4 — the build is CPU-bound and must not starve LVGL or the CDC loop on core 0):
core's `pitchedBuildBank()` **streams** each zone (`WavPcmReader` →
`FftResamplerStream` when the rate differs → the zone's PCM), so a file longer
than the budget costs only block-sized scratch and is **cut to what fits**
rather than refused — `PitchedLoadReport` carries `truncatedZones`,
`droppedSeconds` and a one-line warning the platform shows in the status bar
after the load; roots marked `auto` are detected from the resident PCM; then `PitchedInstrument::installBank()`
runs `gateEnter()` → `beginMutation()` → `allNotesOff()` → `setBank(new)` →
`setParams` → per-pad volume/pan → `endMutation()` → `gateExit()` and hands the
old bank back for the port to free. Two doors, not one. The gate is a flag the RT
task checks on entry, and `gateEnter()` waits until the renderer is actually
out; the mutation flag covers the event task, which the gate does not stop and
which would otherwise publish a `noteOn` between `allNotesOff()` and
`setBank()` — a voice pointing into the bank about to be freed.
**`gateEnter()` returns whether the renderer was observed out.** On a timeout
it keeps the gate held and answers false, and the old bank is then deliberately
leaked with an `ESP_LOGE`: a leak beats a use-after-free under a live render.
That leak is not permanent — the next `engine_arena_reset()` reclaims it
with everything else. `pitchedKitUnload()` is the same call with a null bank.

A **failed** load unloads whatever was playing. Nothing was swapped in, but
the caller has already moved on to the new kit's `pad_notes`, so a surviving
previous bank would sound its zones under a note map belonging to neither
kit. For the same reason `onSamplePlaybackEvent` does nothing at all when the
kit is `Pitched` and no bank is installed — falling through to the prebuilt
streamer is not a fallback, `reloadPad()` wiped its slots for this kit and put
nothing back. A
pitched pad with no bank is silent, and `PITCHED_STATUS`'s `err=` says why.

Pads 1–15 only `WipeOut()` the prebuilt streamer's slot, and only while it is
running.

Budget: `pitchedMaxBankBytes()` is the arena's capacity minus **256 kB held
back for the resampler**, which needs the source, the converted copy and up to
~200 kB of overlap-add scratch live at once (8 kHz → 44.1 kHz, the widest ratio
a kit ships) *while the zones already built are resident*. On a v2 board
that is ~4.3 MB of resident PCM, roughly 50 s of mono 16-bit at 44.1 kHz across
all zones. Two 5 s zones load at `bank_kb=861`. The
per-zone failure message names the arena's free bytes when fragmentation is
the limit.

The event-bus task (`crosspad_events`, core 1, priority 4) has 4 kB of stack:
the SamplePlayback handler reaches `noteOn()` (libm `pow`, `cos`, `sin`) for
pitched kits, and at 2 kB the first `KIT_LOAD` of the 48 kHz fixture after
boot spills the register window past the stack end (PC=1, `crosspad_events`
in the core dump).

`sizeof(KitInfo)` is 1 kB with `InstrumentSpec`; with 76 kits on the card the
list's vector growth (one contiguous block of capacity × 1 kB) must not meet
the still-cached splash at boot — `std::bad_alloc` in
`discoverKitsRecursive`, a boot loop. The scan in `main.cpp` therefore waits
for the launcher (where `LoadMainScreen()` drops the image cache), bounded at
10 s and skipped headless. Because the scan starts that late and grows the vector in place,
`KIT_LIST` and `KIT_LOAD` answer `ERR scanning` while `g_kit_scan_done` is
false rather than iterate a buffer that is moving — a bare `ERR` line, which
is what the host's multi-line reader takes as a terminal answer, so a scenario
gets a `HilError` instead of idling out the read.

`CONFIG_DSP_MAX_FFT_SIZE` is 8192 (the resampler's inverse) in
`sdkconfig.defaults`, `sdkconfig.v1` and `sdkconfig.v2`; the 32 kB twiddle
table lives in PSRAM. Its build is **retried** on every call until it works,
never latched: the one runtime failure is 32 kB of PSRAM missing at the moment
a kit load asks first, and that moment passes — a remembered failure would
answer `no FFT` for the life of the process against a heap that has since
freed the room. Unaligned buffers take esp-dsp's scalar kernels rather
than a bounce copy — the copy would cost 64 kB of PSRAM exactly during a kit
load.

## Scope view

The Sampler panel shows the engine's real output instead of the file
waveform whenever the current kit is pitched: `PitchedInstrument` publishes
its own mono sum into a `ScopeTap` ring every block (heap, PSRAM on the S3)
and remembers the last note's frequency; crosspad-gui's `scope_view` pulls
the newest samples every 33 ms, sizes the window to three periods clamped
to 1–10 ms (`scopeWindowSamples`), locks the phase on the last rising
zero-crossing with hysteresis (`scopeTrigger`) like a scope's edge trigger,
auto-scales to the window's peak on a fast-attack/slow-release envelope
with a −40 dBFS floor, and draws it on a small canvas with a
`440Hz 6.8ms -20dB` label. Apps reach the engine through
`PlatformServices::pitchedInstrument`; the panel hook is one function in
`sampler_panel.cpp`, identical on both sampler branches.

Verified on the simulator (pads 0/5/9/15 → 262/349/440/622 Hz, windows
10/8.6/6.8/4.8 ms, three cycles each, silence a flat line) and on the
board: `pitched_kit` PASS with the Sampler open, `rt_max_us` 1006 (the tap
costs ~60 µs per block at sixteen voices), 32.6 kB internal RAM free.

## Off-device

```
PITCHED_STATUS   → PITCHED: active=1 zones=2 roots=57.00,69.00 voices=1/10 bank_kb=172 rt_max_us=107 steals=0 rt_busy_max_us=… overruns=0 cap=10 kit_max=24 shed=0
                 → PITCHED: active=0 zones=0 roots= voices=0/0 bank_kb=0 rt_max_us=0 steals=0 err=zone_2:_no_PSRAM_(86_kB) rt_busy_max_us=0 overruns=0 cap=10 kit_max=0 shed=0
PITCHED_CAP 10   → OK      (the board's voice cap, from the next setParams: a load or an edit; until a restart)
PITCHED_BENCH 20 → BENCH: <name>=<us> … OK      (async, own task with a PSRAM stack; --expect OK --timeout 120000)
ARENA            → ARENA: cap_kb=5421 used_kb=861 owner=pitched
```

`rt_max_us` resets on read; so does `rt_busy_max_us`, the whole render pass
less its I2S waits (room in TX; the codec reads of the USB audio profile),
this reply's own copy (`overruns` counts passes over a block, since boot).
`voices=` is the polyphony in force now. `err=` stays right after `steals=`
(the harness reads it there; later fields follow it); it is there only after a
failed load and goes on the next load that works; its spaces are already underscores so it stays
one field, and `active=0 err=…` is the shape of a pad that will make no sound.
`PITCHED_BENCH` yields a tick per iteration (its task sits one priority above
idle on core 0, and an unbroken run trips the 5 s idle watchdog), so a large
`iters` needs a matching host timeout. `crosspad-hil pitched_status` /
`pitched_bench` speak both.

## Testing it

```bash
python3 tools/make_pitched_kit.py --zones 2 --seconds 5.0   # HIL PITCHED; reboot to scan
crosspad-hil run pitched_kit --kit-name "HIL PITCHED" --cents 10
crosspad-hil run kit_churn --kits 0,2                     # the DeInit/Init cycle, under play
python3 tools/make_pitched_kit.py --zones 4 --seconds 1.0
python3 tools/make_pitched_kit.py --zones 1 --rate 48000 --seconds 0.5   # exercises the resampler
```

`SMPL_PEAK` reports the louder of the two engines' nodes, so a `kit_churn`
round on a pitched kit is not reported as silence. `free=-1` in the same row
is the stopped sampler's slot count, and is expected. `pitched_kit` is still
what proves the pitched engine sounds at the right pitch.

The scenario loads the kit over CDC, switches to the USB-audio profile with
the RT mixer resumed (DAC→ADC loop, `headphone` preset — the same path as
`sampler_record`), plays each pad and measures its dominant frequency
against `440·2^((60+pad−69)/12)` (±10 cents) plus `analyze sine`'s phase-slip
count, then a 16-pad chord (every expected peak present within tolerance and
≥ −40 dB of the strongest), then `PITCHED_STATUS` (`active=1`, `steals=0`,
`rt_max_us` under the block, every root within 0.05 semitone of an integer —
the auto-detected ones prove the detector against real hardware). Zones are
pure sines, so the numbers are exact: a wrong zone choice or a wrong ratio
shows as cents, not as taste.

Results on a v2 board, 2026-09-03 (`crosspad-hil run pitched_kit --cents 10`):

| Fixture | Pads | Chord | `rt_max_us` | Roots (`auto` marked) | Verdict |
|---|---|---|---|---|---|
| 2 zones, 5 s, 44.1 k (arena, prebuilt streamer stopped) | 16/16 | 16/16 | 935–945 | 57, 69* | PASS ×3, `bank_kb` 861, `ARENA owner=pitched` |
| 2 zones, 1 s, 44.1 k | 16/16, worst 0.07 cent | 16/16 | 853 | 57, 69* | PASS |
| 4 zones, 0.5 s, 44.1 k | 16/16 | 16/16 | 863 | 57, 69*, 81, 93* | PASS |
| 1 zone, 0.5 s, 48 k (resampled at load) | 16/16 | 16/16 | 838–859 (4 runs) | 69 | PASS ×4 (4 kB event-task stack) |
| 2 zones, **5 s**, 44.1 k (`bank_kb=861`, arena) | 16/16 | 16/16 | 935 | 57, 69* | PASS |

With the bank-swap safeguards (mutation flag, atomic voice publish, modulo
loop wrap, `gateEnter()` result, unload on a failed load, `ERR scanning`,
`cdc_reply` mutex) the 48 kHz row and a repeat of the first: both PASS,
`rt_max_us` 840/841, and `kit_churn` (20 swaps, 740 hits in the windows)
PASS on the same build. Three pitched↔sampler swap cycles leave internal RAM
flat (22 415 B free before and after) and PSRAM within ±5 kB.

With the arena and the handoff (2026-09-03): `smoke` PASS
with no boot loop, `pitched_kit` PASS on the 5 s fixture, `kit_churn --kits 0,2`
PASS (20 swaps, 482 hits inside the windows, 0 reboots, 0 CDC drops),
`app_churn --rounds 5` PASS (no per-app slope over 32 B/visit). Five
pitched↔sampler cycles: `int_free` 18 799 → 19 663 B, `psram_free` 228 556 →
258 720 B, `ARENA` back to `used_kb=5405 owner=marcel` — no drift in either
direction.

The fixture sets pad volume to 20 because the engine applies no 1/N headroom,
exactly like the sampler: sixteen −6 dBFS sines at full volume clip the chord
test (`peak_dbfs 0.0`, 4 of 16 peaks lost, measured 2026-09-03).

Sampler regressions to run beside it after touching this path: `smoke`,
`kit_churn` (swaps under playback, sampler kits), `app_churn`.
