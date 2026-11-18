# Kits, the sample engines and the pitched instrument

Contributor notes: kit types, the native engine, the arena, kit swaps under play. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Kit Types (Performance, Drums, Slice, Pitch)

The kit browser lists PERF, DRUMS, SLICE, PITCH (then SONGS, USER), filed by
the folder a kit is in (core `KitFolders`, all three scanned at boot):
`/crosspad/kits` PERF, `/crosspad/drums` DRUMS, `/crosspad/slicer` SLICE (where
the Slicer's KIT writes); a kit elsewhere by its kit.json. PERF starts with
**+ NEW KIT** and DRUMS with **+ NEW DRUMS**: an empty kit of one's own in that
folder, sixteen coloured pads, filled in the Sampler's pad editor; `"own": true`
(core 2.32 `KitInfo::own`) makes it the user's to rename and delete while a drum
kit's category stays `drums`. **Drums** kits carry
`category` drums (`KitInfo::isDrums()`): the native engine
plays them at most 12 voices (`voiceCap()`, `TakeSpec::voiceCap`, applied on a
take and on a seamless swap's first pad). **Slice** kits (`"engine": "slice"`,
what the Slicer's KIT writes) are one WAV in regions, a pad each
(`{sample, start, end}`), one voice (choke group 1 forced in
`buildTakeSpec()`), the kit's `"pitch"` and each pad's `"attack"` in real
time; a trim that ends inside its file fades over `kTrimFadeMs`.
**Pitch** plays at most 4 voices (`PitchedSampleNode::kVoiceCap`).
**One song holds Drums, Slice, Pitch and audio at once** (core 2.6): route 0
the kits' engine, 2 the Pitch instrument (`AudioSong::seqRoute()`); up to eight
tracks (`kTracks`), every seq track but the armed one frozen, kit tracks a kit
each taking turns on the one kits' engine (`AudioSong::liveKit`, whose kit is
the song's and the Sampler's; `liveRoute()`; no second kit engine since core
2.42) -- and the Pitch track's instrument is built **beside** the kits' engines
(`PitchedLoadOptions::besideKits`: general PSRAM heap, not the arena, so a
kit take's arena reset leaves it; `instrument_host_beside()`, INST_STATUS
`beside=1`); only a bank in the arena (`pitchedBankInArena()`) still makes the
kits give way. Arrange's ADD TRACK is AUDIO / KITS, and KITS is Perf, Drums,
Slice or Pitch, a kit picked from the browser's list of its kind
(`IKitPicker::open(..., KitList)`). With Pitch armed the fingers play the
instrument and the song's route-0 notes still reach the kit (`PitchFingers`).
Measured (`tools/hil_perf_song.py --only C`): ~760 us of work a 1451 us block
(~850 in the run [audio-song.md](../subsystems/audio-song.md) records),
no underruns, a few single passes over the block a minute. Slice filter and
sample position are next.
Details: [docs/subsystems/sample-tools.md](../subsystems/sample-tools.md).

## Sampler Engines (native by default)

The native engine (core `crosspad/sampler/*`, `components/crosspad-platform-idf/SamplerEnginePort.cpp`)
plays every sampler kit: a kit load ends in `native_sampler_take_kit()`, which
reads each sample's head into the arena on its own PSRAM-stack task. The asker
copies the kit into a plain-data `TakeSpec` (core `KitTake.hpp`; a pad edit a
`TakePadSpec`) -- the worker never reads the `KitInfo` the UI frees -- and a
take asked for during another runs right after it (`WorkerGate`); `SMPL_ENGINE`
`kit=` is the kit the engine holds. Every teardown (release, arena reclaim)
goes through `io_quiesce()`: one streaming pass of its own, then
`abandonVoices()` for voices a parked render never ends, then the audition. The
prebuilt streaming engine and the glue only it uses are compiled out unless
`CONFIG_CROSSPAD_SAMPLER_MARCEL=y` (`components/crosspad-platform-idf/Kconfig`,
off in `sdkconfig.v*`/`defaults`): then it is back with its own "Sampler"
mixer channel, the native one on "Sampler2", and `SMPL_ENGINE native|marcel`
switches; its memory is the caller's (`SampleStreamPlayer_Init(32 kB, 16 kB)` in
`engine_handoff.cpp`: a first block per sample and 16 kB stream blocks, from the
arena as a kit is set up -- 1343 kB for HIL SINE A's 16 samples -- and ~6 kB of
state from internal RAM while 48 kB stays free). Without it the native engine's channel is "Sampler" in the old slot,
`SMPL_ENGINE` reports `engines=native`, `SMPL_POOL`/`SMPL_BKG` answer
`ERR only_native`. A file outside the kit (the browser, `SMPL_PREVIEW`, the
start-up sound `crosspad/STARTUP.wav`) plays on the engine's **audition voice**
(core `SamplerEngine::previewStart`, its own ring of 4 pool blocks reserved
with every kit, streamed on audio_bg, heard on the "Preview" channel; the next
one, `SMPL_PREVIEW_STOP`, closing the browser or a kit load ends it). The
live scope is the native node's own `ScopeTap` (32 kB PSRAM, taken the first
time it is shown). While the native engine holds a kit the song and preview
rings borrow the arena's slack (`engine_arena_lend()`: never what its pool
may still grow by, plus one head), and give it back before every reset. Mixer desks are matched by channel name (crosspad-mixer desk
v4), so a desk from either layout loads onto the right channels. Design and
measurements: [docs/subsystems/sampler-engine.md](../subsystems/sampler-engine.md).

## Pitched Instrument (one sample, sixteen notes)

A kit with `"engine": "pitched"` holds 1–24 recorded zones whole in PSRAM (`kInstrumentMaxZones`); every
pad plays the nearest zone transposed to its `instrument.pad_notes` note —
varispeed Hermite, 32.32 phase, `max_voices` clamped to 28 (`kMaxSounding`)
and to the board's cap of 4 (`PitchedSampleNode::kVoiceCap`). **Core owns the engine**
(`crosspad/instrument/*`, `crosspad/dsp/*`): voices, render gate, `installBank()`,
`createFft()`, `pitchedBuildBank()`. FFT runs at load time only (root detection,
rate conversion) — one 1024-point FFT is 0.56 ms of a 1.45 ms block where 16
voices are 0.72 ms. Platform is glue: `PitchedKitPort.cpp` fills `PitchedLoadEnv`,
`PitchedSampleNode.hpp` adds rt-max/peak, dispatch in `components/crosspad-platform-idf/SamplerPlatformPort.cpp`. **The
engines take turns owning one arena** (`engine_arena.hpp`; no reserved
block: the general PSRAM heap with every block on one list, never
below a 512 kB floor kept for the UI), handed
over by `kit_engines_stop()` (`engine_handoff.h`: the native sampler's heads,
the prebuilt streamer when built, the song rings) — a bank can take most of it, not 256 kB.
**One owner at a time:** a native take/pad rebuild, a pitched build/unload and
the handoff all hold `EngineArenaOwner` (`engine_arena_owner_lock`, taken
*before* the native `LoaderLock`; `engine_arena_reset()` takes it too), and
`kit_engines_stop()` unloads the installed pitched bank *before* it resets.
Every reset bumps `engine_arena_generation()`; the bank is stamped with it
(core `SampleBank` `MemoryGenerationFn`) and a stale one forgets its blocks
instead of freeing them (freeing them after a reset corrupts the heap).
Size the arena's requests in whole 128 kB TLSF classes:
the heap rounds a 4–8 MiB request up to one and keeps the rest. Off-device
`PITCHED_STATUS`, `PITCHED_BENCH`, `ARENA`, `tools/make_pitched_kit.py`,
`crosspad-hil run pitched_kit`; numbers: [docs/subsystems/pitched-instrument.md](../subsystems/pitched-instrument.md).

## Changing Kits While Playing

Four load-bearing facts: the loader is one long-lived worker (a task created
`WithCaps` must be deleted `WithCaps`, or every swap leaks its stack); a kit that
is not current drops its parsed pads (`releaseParsedPads()`); a request made
mid-load is queued, newest wins; three threads share one prebuilt player through
`SamplePlayerAccess.hpp` (file mutex per library call + render gate + the streamer
held out per `SetupSample()`/`WipeOut()`; the library's files are the port's
handles in `sample_player_port.cpp` — see both); the port gives it a voice cap of
32 channels after every `Init()` (`SetMaxPolyphony()`, `SMPL_POLY` overrides it) and
`audio_bg` refills through the library's own `SampleStreamPlayer_BkgProc()`. The swap is
seamless: a sounding pad finishes its sample before it is reloaded (last one forced after 1 s), the
render gate is taken only for a sounding pad, a hit on the pad being rebuilt is
held and played after it, and every bulk reader of the card yields to the
streamer (`sample_player_port_yield_to_streamer()`). The streamer's refills,
the loader's preloads and a bounce's pieces all go around stdio straight into
or out of their PSRAM buffers: the S3's SDMMC cannot DMA to or from PSRAM, and
the card's sector layer (`sd_sector_io.cpp` -- it also caches single sectors, the directory walks every open repeats, `SD_CACHE`; `--wrap=sdmmc_read_sectors`/
`_write_sectors`) bounces such a transfer through one internal buffer taken
at boot, eight sectors a command -- IDF's own path is a CMD17 a sector
through a fresh malloc (4x slower, "not enough mem" under WiFi). The older 4 kB
stage is not allocated while the sector bounce is
(`sample_player_port_direct_io()`). `SD_TRACE` counts every transaction by
task. Pad LED frames go out from the `pad_anim`
worker only (coalesced refresh): `audio_rt` records a voice's state and the
worker paints it -- the render takes no strip lock -- and the engine's voice
reports are not posted on the event queue. `KIT_SWAP_LOG` over CDC is
the per-swap evidence. Test with `crosspad-hil run kit_churn [--rapid 0.4]` and
`kit_swap_audio` (sine kits, recorded through UAC2).
Why each of those holds: [docs/subsystems/kit-swap.md](../subsystems/kit-swap.md).
**The native engine never stops for a kit change**: the loader swaps it pad by
pad (`native_sampler_swap_pad()`), a sounding voice plays its old sample out,
and an old head goes back once `SamplerEngine::slotInUse()` is false; no whole
take after the load while it already holds the kit. See
[docs/subsystems/sampler-engine.md](../subsystems/sampler-engine.md#changing-kits-while-playing).
