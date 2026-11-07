# Pitched instrument engine (single-sample kit) — design

Date: 2026-09-03. Status: approved for implementation under stated assumptions
(engine first, GUI later).

## Goal

A second sample engine beside the prebuilt streaming sampler: one to four recorded
notes ("zones") held **whole in PSRAM**, every pad playing them transposed to
its own MIDI note in real time. It appears in the kit loader as an ordinary
saved kit with its own `kit.json`. Target: more simultaneous voices than the
multi-file sampler on the same board, because nothing streams from SD while
playing. First content: a sine "kit" measured through the board's own UAC2.

## Constraints that shape it

| Fact | Consequence |
|---|---|
| PSRAM free at the launcher ≈ 450 kB (the prebuilt streamer's pool takes 5.5 of 6.2 MB at boot, prebuilt, not reclaimable) | No pre-rendering of 16 variants. Zones live once, mono int16; budget ≈ 5 s of audio total (`kMaxBankBytes`, default 400 kB, checked before allocation, error surfaced in the status bar). |
| RT task: core 1, prio 19, block = 64 frames @ 44.1 kHz = 1.45 ms; mixer + the prebuilt streamer already run there | Per-voice render must be O(1)/sample. A phase vocoder per voice (FFT+IFFT per hop) is ~5–8 % of a core per voice by estimate — 16 voices do not fit. |
| esp-dsp is linked (`CROSSPAD_USE_ESP_DSP=1`), FFT unused so far | FFT/IFFT SIMD is used where it pays: at **load time** (root-pitch detection, band-limited rate conversion) and in a bench verb that measures the true per-voice spectral cost on the device, so the decision above is backed by numbers, not estimate. |
| The prebuilt library has no pitch API and one global `FILE*` | New engine is independent: its own `IAudioNode`, its own PSRAM bank, no shared file slot at play time. Loading reads WAVs with plain stdio on the kit-load worker (already off the LVGL thread). |
| Kits are 44.1 kHz; the engine rate is settings-driven (44 100 default) | Zone at another rate → converted once at load with the FFT resampler, so playback ratio is exact. |
| Core must stay platform-free | Engine, FFT interface + portable radix-2 FFT, pitch detector, resampler, WAV reader are in crosspad-core with an allocator callback. esp-dsp adapter and mixer wiring in `crosspad-platform-idf`. |

## Real-time pitch method

Varispeed: each voice reads its zone at increment `2^((note − zoneRoot)/12)`
with 4-point cubic Hermite interpolation, phase in 32.32 fixed point. Zone
selection = nearest root (ties → lower). Per voice: velocity gain (linear
`vel/127`), attack (2 ms) and release (`release_ms`, default 150) linear
envelopes, optional sustain loop (`loop_start`/`loop_end` in samples, crossfade
free — loop points are the kit author's job). Voice count `max_voices`
(default 24, cap 32). Stealing: a voice in release with the lowest level,
else the oldest. Mix: each voice accumulates into a mono scratch, then pan
(per pad, from `pads[].pan`) to the stereo interleaved bus. Gain-accumulate
uses `dsps_mulc_f32`/`dsps_add_f32` via the existing `CROSSPAD_USE_ESP_DSP`
pattern; the interpolation loop is scalar.

Duration changes with pitch, as on every classic sampler; acceptable for the
piano target (2–4 zones → ±2–8 semitones). Anti-alias mip levels (FFT-built
half-band copies) are a documented follow-up, not in scope.

## Where FFT/IFFT (SIMD) is used

1. **Root detection** (`PitchDetector`): Hann-windowed 4096-point real FFT
   over the loudest 4096 samples after the attack, harmonic product spectrum
   (3 harmonics), parabolic peak interpolation → Hz → fractional MIDI note.
   Used when a zone says `"root": "auto"` (or omits `root`); the detected value
   is logged and reported by `PITCHED_STATUS`. On the sine kit the result must
   be within 5 cents of the generator frequency.
2. **Rate conversion** (`FftResampler`): overlap-add blocks (N = 4096, 50 %
   Hann), forward FFT, spectrum re-binned/zero-padded to N·outRate/inRate,
   inverse FFT — band-limited, no design of a FIR per ratio. Used when
   `wavRate != engineRate`.
3. **`PITCHED_BENCH`** (CDC): times `IFft` 1024/2048/4096 forward+inverse on
   esp-dsp and one Hermite voice render of 64 frames, N times; prints µs. That
   is the evidence behind "spectral per-voice is too expensive here".

`IFft` is the seam: `PortableFft` (radix-2, precomputed twiddles, scalar) in
core for PC/tests; `EspDspFft` (`dsps_fft2r_fc32` + `dsps_bit_rev2r_fc32`,
inverse by conjugation) in platform-idf. Both produce identical results within
1e-4 on the test vectors.

## Kit JSON

```json
{
  "name": "HIL SINE PITCHED", "author": "hil", "engine": "pitched", "type": 0,
  "instrument": {
    "zones": [
      { "sample": "a3.wav", "root": 57 },
      { "sample": "a4.wav", "root": "auto", "loop_start": 0, "loop_end": 0 }
    ],
    "pad_notes": [60,61,62,63,64,65,66,67,68,69,70,71,72,73,74,75],
    "base_note": 60,
    "release_ms": 150, "attack_ms": 2, "max_voices": 24
  },
  "pads": [ { "note": 0, "color": …, "playbackMode": 1, "volume": 100, "pan": 0 }, … 16 ]
}
```

- `engine` absent or `"sampler"` → existing path, untouched. `"pitched"` →
  this engine. `KitInfo::engine` (`KitEngine::Sampler|Pitched`) is parsed and
  serialized; `KitInfo::instrument` (`InstrumentSpec`) holds the block.
- `pad_notes` (16) wins; else chromatic from `base_note`. `pads[].note` stays
  the pad index (LED/choke code reads it); `pads[].playbackMode` should be 1
  (hold) so release reaches the engine — the tool writes it so; the engine
  does not require it.
- `pads[].volume`/`pan` apply per pad. `pitch` (existing dead field) is left
  alone.
- Samples resolve through `resolveKitSamplePath()` like today (flat or
  `SAMPLES/`). Mono or stereo WAV, 16-bit PCM; stereo is averaged to mono.

## Runtime integration (platform-idf)

- `PitchedSampleNode : IAudioNode` (generator), registered once at
  `CrosspadPlatform_Init` as mixer channel `"Pitched"` routed to all outputs,
  beside `"Sampler"`. Renders silence when no bank or while the load gate is
  held (atomic flag, same idea as `samplePlayerRenderBegin`).
- `PitchedKitPort` (platform): `loadKit(const KitInfo&)` builds a new
  `SampleBank` (PSRAM via `heap_caps_malloc(MALLOC_CAP_SPIRAM)`), runs
  detection/resampling, then swaps it into the engine under the gate;
  `unload()` frees. Old bank freed after the swap, never while RT may read it
  (gate held across the swap; RT sees the pointer only through an atomic).
- Kit dispatch in `main/gui/gui.cpp`: the kit-load worker calls
  `reloadPad(i)` for 16 pads. For a pitched kit, pad 0 triggers
  `PitchedKitPort::loadKit()` and wipes the streamer's pads; pads 1–15 are no-ops
  for the streamer and only update pad volume/pan in the engine. For a sampler kit,
  pad 0 calls `PitchedKitPort::unload()` first. `onSamplePlaybackEvent`
  routes NoteOn/NoteOff to the engine when the current kit is pitched
  (pad → `pad_notes[pad]`), otherwise to the streamer. Both engines never hold
  voices at once, so mixer headroom is unchanged.
- CDC verbs (`main/hil_control.cpp`): `PITCHED_STATUS` →
  `PITCHED: active=<0|1> zones=<n> roots=<a,b,…> voices=<active>/<max>
  bank_kb=<n> rt_max_us=<n> steals=<n>`; `PITCHED_BENCH [iters]` → one line per
  measurement. Registered in crosspad-hil `knowledge/cdc.yaml`.

## Test plan (the deliverable is measured, not asserted)

- **Core unit tests** (Catch2 in `components/crosspad-core/tests`): FFT vs
  naive DFT; detector on synthetic sines (A4 440 → 69.00 ± 0.05); resampler
  48 k → 44.1 k tone stays at its Hz; engine: note 69 on a root-57 zone
  advances exactly 2.0 samples/frame, 16 simultaneous notes render without
  steals at `max_voices = 24`, steal order, release tail length, JSON
  round-trip of `engine`/`instrument`.
- **Fixture**: `tools/make_pitched_kit.py [--zones 1|2|4] [--seconds 1.0]`
  writes sine zones (A3 220 Hz, A4 440, A5 880, A6 1760 as roots 57/69/81/93)
  and the kit JSON, pushes `/sdcard/crosspad/kits/HIL_PITCHED` with
  `fs_transfer.py`. Zones use `"root": "auto"` for two of them and explicit
  roots for the rest, so both paths are exercised by one kit.
- **HIL** (`crosspad-hil run pitched_kit`, new scenario): load the kit over
  CDC, record each pad through UAC2 (`CaptureSession`, mixer resumed, DAC→ADC
  loop), measure the dominant frequency per hit against
  `440·2^((note−69)/12)` — pass within ±10 cents, held-frame count 0; then a
  16-pad chord, all 16 expected peaks present ≥ −40 dB of the strongest;
  then `PITCHED_STATUS` must show `steals=0` and `rt_max_us` under the block
  period. `smoke`, `kit_churn` (sampler kits) and `app_churn` must still pass
  — the streamer path is untouched but shares the RT task.
- Bench: `PITCHED_BENCH` numbers go into the subsystem doc.

## Out of scope now

GUI beyond "it shows in the kit loader" (badge, editor), MIDI-keyboard note
input (engine API already takes MIDI notes), anti-alias mip zones, Arduino
port, PC simulator wiring (core is portable; the PC repo has its own core
checkout and gets it on the next submodule bump).

## Outcome

Implemented as specified, with two departures worth naming: the FFT lives in
crosspad-core rather than the platform (esp-dsp ships with both ESP-IDF and
Arduino-ESP32, so a `CROSSPAD_USE_ESP_DSP` guard is enough), and the two sample
engines share one PSRAM arena instead of competing for the pool — which is what
took the bank budget from 256 kB to ~5.1 MB. As-built behaviour, paths and
bench numbers: [docs/subsystems/pitched-instrument.md](../../subsystems/pitched-instrument.md).
This spec is kept as written.
