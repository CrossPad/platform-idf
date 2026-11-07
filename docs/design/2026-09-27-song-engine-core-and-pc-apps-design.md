# Song engine in core; every app on the PC simulator

## Goal

The PC simulator runs the five apps the board has and PC lacks — arrange,
dawcontrol, pad-mixer, sequencer, song — with their features working, not
only their screens. The one large missing piece, the audio song engine
(project list, clips, bounce, playback), moves from the board's
`SongEnginePort.cpp` into crosspad-core so the board and PC share one
implementation ("platform repos thin, shared repos thick").

## Decisions

- Full scope in one go, song engine included.
- Shared engine in core with a platform hook interface, not a PC copy.
- PC work is based on the branch `feat/mini-daw-merge`.
- The board's behaviour does not change: same files (`/sdcard/SONGS`), same
  NVS key (`song`/`current`), same CDC verbs (`ASONG`/`SONG`), same timings.

## 1. crosspad-core: `crosspad::SongEngine` (core 1.24.0)

`include/crosspad/song/SongEngine.hpp`, `src/song/SongEngine.cpp`: the
current `SongEngine` class (ISongEngine + IAudioNode) with its FrameRing,
ClipReader, Arrangement/rebuild, rtGen handoff, `mix`/`rtCapture`, worker
(lists, open, save, fill, bounce) — moved, not rewritten. File I/O stays
POSIX/stdio as today (`O_BINARY` where defined).

Everything board-specific goes behind `ISongEnginePlatform`:

| Hook | Board | PC |
|---|---|---|
| `startWorker(fn)` | FreeRTOS task, PSRAM stack, prio 5, core 0 | `std::thread` |
| `wake()` / `waitWake(ms)` | binary semaphore | condition variable |
| `sleepMs(ms)` | `vTaskDelay` | `this_thread::sleep_for` |
| `allocRing(bytes)` / `freeRing(p, bytes)` | arena slack while the prebuilt streamer runs, else PSRAM above the 128 kB UI reserve | `malloc` |
| `storageReady()` | SD handle + internal-RAM check | root folder exists |
| `yieldIo()` | `sample_player_port_yield_to_streamer()` | no-op |
| `songsRoot()` | `/sdcard/SONGS` | `<sdcard_path>/SONGS` |
| `loadCurrent()` / `saveCurrent(name)` | NVS `song`/`current` | `<songsRoot>/.current` |
| `beginPadsCapture()` / `endPadsCapture()` | capture bus 2 volume 1, unmuted; restored | PC pads bus, same |

Time comes from `IClock`; metronome and sequencer from `PlatformServices`
(`sequencer`, `sequencerEngine`) plus a `MetronomeNode&` passed in. The
platform calls `engine.rtCapture(bus, frames)` right after its mixer render
and adds the engine as a mixer channel routed to its stream outputs.

Tests (`tests/test_SongEngine.cpp`, fake platform, worker driven inline,
temp folder): project list/open/save round trip, clip list, arrangement
rebuild and gains, play → mix output matches the clip samples, bounce of a
generated signal → WAV on disk equals it, cancel, rings released only after
RT leaves (`rtGen`).

## 2. platform-idf: the board port shrinks

`SongEnginePort.cpp` keeps `song_engine_init` / `song_engine_rt_capture` and
implements the hooks (~120 lines). `crosspad_deps.h` requires core 1.24.0.
Verification on hardware: build v1 + v2, flash, `smoke`, `song_bounce`,
`seq_kit_change`, `ASONG`/`SONG` answers unchanged.

## 3. crosspad-pc (on `feat/mini-daw-merge`)

- core → 1.24.0 (submodule + `crosspad_deps.hpp`).
- Five app submodules under `src/apps/` at the SHAs platform-idf pins.
- `PlatformServices::stm32MessageHandler = &stm32Handler` (DAW Control).
- `MetronomeNode`, `PatternSequencer`, `SequencerController` created and
  published as on the board (`CrosspadPlatform.cpp`), with `getMetronome()` /
  `getSequencer()`.
- Mixer gets a third output, the pads-only bus, which nothing plays; the
  Song channel is routed to the audible outputs only; `PcAudioModule`
  calls `rtCapture` after `render`.
- PC song engine port: the hooks above; `songsRoot` from `sdcard_path`.
- `padMix`: the PC sampler node implements `IPadMix` (per-pad volume/pan),
  published in `PlatformServices`.

Verification: build, core tests, PC tests; run the simulator and open each
of the five apps (screenshots); sequencer plays a pattern; bounce a scene →
a WAV appears under `<sdcard_path>/SONGS/<project>/`, arrange plays it back;
pad-mixer changes a pad's level in the sampler; DAW Control shows "no host".

## Out of scope

The hub's launch buttons on PC (no hub there); Arduino.
