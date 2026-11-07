# Changing Kits While Playing

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

A kit swap is the one operation that restructures the sample engine underneath
a performance that has not stopped. Four things keep it working, and each is
load-bearing.

**The loader is one long-lived worker, not a task per load.**
`kit_load_task_start()` signals a worker parked on a semaphore
(`components/crosspad-sampler/src/logic/kit_load_task.cpp`).
`xTaskCreatePinnedToCoreWithCaps()` builds a **static** task: `vTaskDelete()`
frees nothing, only `vTaskDeleteWithCaps()` releases its buffers. A task per
load ended with `vTaskDelete()` leaks 8196 B of PSRAM (the stack) and 372 B of
internal RAM (the TCB, from `pvPortMalloc`) a swap; ten swaps take internal RAM
from 18.7 kB to 5.7 kB free, `sdmmc_cmd` can no longer allocate a DMA buffer,
every SD read fails, and the load in flight never finishes. **Any task created
`WithCaps` must be deleted `WithCaps`.**

**A kit that is not current does not keep its parsed pads.**
`PortableKitLoader` caches every kit it has ever parsed. The pad payload is
hundreds of small `std::string`s, and under `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`
(128) an allocation that small comes from *internal* RAM — ~1.4 kB per distinct
kit, permanently. With 17 kits on the card that is more internal RAM than the
board has once BLE is up. `releaseParsedPads()` drops the payload when the kit
stops being current, keeping metadata (the launcher lists it) and skipping any
kit with unsaved pad edits. Re-parsing costs one JSON read on the load task.

**A request made mid-load is queued, not dropped.** A dropped request would
make picking a kit while the previous one is still loading do nothing at all —
no sound change, no error, a status bar still showing the older load — and two
taps inside the ~3 s a load takes are enough for that. The newest request is
held and started when the running one reports; only the newest, so spinning the selector lands on the kit you stopped
at rather than replaying every kit you passed. `kit_load_task_pending_kit()`
exposes what is waiting, and `KIT_STATUS` reports it over CDC.

**Three threads share one prebuilt player, so access is arbitrated.**
See `components/crosspad-platform-idf/include/SamplePlayerAccess.hpp` — the
mechanisms and the panics they prevent are documented there. In short:
`audio_rt` renders, `audio_bg` refills from SD, and the kit loader and LVGL
thread restructure voices, all through the prebuilt library, whose files are
handles of the port (`SampleStreamPlayer_File*` in `sample_player_port.cpp`).
Two mechanisms, deliberately not one lock:

- a **file mutex** held for *one library call* — long enough to make an
  open→seek→read→close sequence atomic, short enough that the streamer is not
  blocked for the length of an SD-backed reload;
- a **render gate**, which is a flag and not a lock, because the RT task must
  never block on a thread doing SD I/O. While a mutator holds it,
  `MarcelSampleNode` contributes silence instead of rendering voices that are
  being freed.

Note the third piece: `samplePlayerStreamBegin()` makes `audio_bg` **stand
aside** when a mutator wants the slot. A plain mutex is not enough — FreeRTOS
wakes the highest-priority waiter and has no anti-starvation, so `audio_bg` at
prio 18, looping with a 1 ms delay, re-takes it before the loader is ever
scheduled. Priority inheritance does not help; it applies once you *hold* the
mutex, and the problem is acquiring it.

**The voice cap is set by the port.** `SampleStreamPlayer_SetMaxPolyphony(32)`
(channels: sixteen stereo voices) after every `Init()` (`sample_player_port.cpp`);
a sample change (`SetupSample()`, `WipeOut()`) holds the streamer out for the
call, because `BkgProc()` opens, reads and closes the files of the samples it
changes. `SMPL_POLY`
overrides the cap until
`SMPL_POLY 0`. `audio_bg` refills through the library's own
`SampleStreamPlayer_BkgProc()`.

The loader also runs above priority 1: at 1 it sits level with the main CDC
loop, which is enough to stretch a 3 s load past 20 s under a heavy pad stream.
It runs on **core 1** (priority 4, under TinyUSB and `audio_rt`), not core 0:
a pitched load is ~2 s of FFT, and at priority 5 on core 0 that starves LVGL
and the CDC drain loop for the whole build — no reply to `KIT_STATUS` or
`SMPL_PEAK` within 2 s, which `kit_churn` reports as the loader never
finishing. `CDC_STATS` cannot show that starvation: `maxgap_us` is measured
only while the queue was already backed, and a loop that was idle when the
command arrived simply never ran.

**The audio tasks themselves fail loud, not silently.** Under memory pressure
(NimBLE up — see [ble-midi.md](ble-midi.md), *What it costs*) an unchecked
create can leave `audio_rt` missing, with no I2S, no codec, and no log line
saying why. Both tasks in `main/audio/audio_task.cpp` run on static
stacks/TCBs (`.bss`, laid out at link time, so they can't lose a race for a
contiguous block) and a failed create is an `ESP_LOGE("AudioTasks", …)` the
smoke scenario catches. `audio_bg`'s stack is 4 kB against a measured 2.1 kB
high-water mark.

## A swap you cannot hear

The four facts above make a swap survive a performance. Five more make it
inaudible, each measured by the `KIT_SWAP_LOG` row the load task keeps per
swap. The test is `crosspad-hil run kit_swap_audio` — two kits of pure sines,
one frequency per pad, recorded through the board's own UAC2 endpoint while
the pads fire and the kit changes under them; without these five it hears
every swap as 1.2–2.0 s of silence on every other pad, plus lost hits.

**The render gate is for a sounding pad, not for every pad.**
`SampleStreamPlayer_SetupSample()` reads the sample off the card — 30..200 ms
on a 1-bit card — and under the gate the renderer would skip 650..1350 blocks
per swap (`gated_blocks`, 1.45 ms each). The
renderer only has to be kept out of a voice it is inside, and the port knows
which pads have a voice (`sample_player_pad_sounding()`, one bit per
pad/layer, set and cleared by the engine's own NoteOn/NoteOff callbacks).
`PadMutationGuard` in `SamplerPlatformPort.cpp` takes the gate only for a pad that is sounding;
an idle pad is restructured with the file slot alone. `gated_blocks` is 0.

**A sounding pad keeps sounding; its reload waits.** The orchestrator does
not post NoteOff to every pad. `kit_load_task.cpp` reloads the idle pads
first, polls the sounding ones every 5 ms and reloads each as its voice ends.
Only when nothing but sounding pads is left does it force one — the last one,
after a second's grace, or all of them past a 4 s deadline (a looped pad never
ends). `deferred=`/`forced=` in the row say which pads took which path. A
sampler kit following a sampler kit is swapped this way; a pitched kit
replaces the whole engine and stops its own voices on the platform side.

**A hit on the pad being rebuilt is held, not lost.** `platform_reloadPad()`
marks its pad; the SamplePlayback bridge parks a hit on that pad (velocity,
one per pad) and the marker's destructor plays it once the pad is back —
late by what was left of the reload (`late_hits`), instead of a half-built
voice (a crash) or silence. While a hit waits the
loader stops yielding the card to the streamer, so the wait is the raw read.
The files are opened *before* the pad is marked, and a hit that lands during
that warm-up plays the old sample, which is still intact — to its end: the
loader asks through `reloadPadIfIdle()`, gets "not now", and comes back for
the pad (a wipe under a live voice would let it carry on into the new file:
old tone, a hole, new tone). NoteOn marks the pad as sounding before the engine
confirms it, so the loader never looks into that one-block gap.

**The streamer has its own file slot.** For a reader that addresses files by
path (the native engine, `sample_port_pread()`) the port's "current file" is
per task: `audio_bg` refills through one slot, the mutators (loader, LVGL,
engine teardown) share another under the file mutex, the RT task — which
only ever closes — gets one that stays empty. The streamer therefore does
not wait behind the loader's 30..200 ms library calls; what it still
yields to is the engine teardown (`SamplePlayerEngineGuard`, the only thing
that takes the stream mutex besides the streamer). A handle live in another
slot is never shared from the cache: two tasks seeking one FILE interleave
their positions. The slot is assigned inside the same critical section that
inserts the handle — a handle in the cache but in no slot yet can be evicted
by the streamer between the two, and closed twice (`multi_heap_free` assert
on the loader). The prebuilt library opens and closes its files itself, a
handle each (`SampleStreamPlayer_FileOpen`), from `audio_bg`, the mutators and
the render; a seek or read holds its handle for the call, so a close from
another task lands after it.

**Everything else on the card yields to the streamer.** The card is the
budget: eight voices streaming is ~1.4 MB/s; a loader reading at ~2 MB/s and
a waveform loader walking every file of the new kit flat out stretch the
refill pass to 100..230 ms, and every voice near empty has a hole (or, for a
fresh voice, a late start). The loader's `readBytes()` goes in 4 kB pieces;
after each, `sample_player_port_yield_to_streamer()` sleeps by the length of
the streamer's last pass (`g_bg_pass_last_us`: >40 ms → 3 ms, >20 ms → 1 ms,
else a yield), bounded so a kit still lands under heavy play. The waveform
loader waits for the load to finish and yields three times harder. The
handle cache is 16 (a whole kit), unbuffered stdio (a buffered FILE was
~1.4 kB of internal RAM; 24 of them left 9 kB free), the streamer's handles
are evicted last, and the cache is not flushed at the end of a load —
closing eight sounding voices' handles would make the next refill pass reopen
all eight, a hole in all of them at once.

**A preload goes around stdio.** This newlib's `fread()` has no large-read
bypass, so a 96 kB layer would come through the handle's 1 kB PSRAM buffer,
and the S3's SDMMC cannot DMA into PSRAM: a CMD17 + CMD13 through a 512 B
bounce per sector, ~400 transactions a layer, each a wake of the loader under
`audio_rt` on core 1 (measured 2026-10-05: ~166 ms a pad in CDC, ~244 in
UAC). A mutator's read of 8 kB or more (`read_staged()` in
`sample_player_port.cpp`) reads the descriptor into a 4 kB internal DMA
buffer, in pieces that end on 4 kB of the file -- one CMD18 each -- and keeps
the 4 kB yield. `KIT_SWAP_LOG` ends with `KITSWAP: io staged=N fallback=M`,
both since boot: `fallback` > 0 means some preloads went through stdio.

The streamer reads the same way: the library refills a voice with one call
of up to 96 kB, which through the FILE's 1 kB buffer is ~190 CMD17+CMD13 pairs
a refill, and leaves a bounce beside it no card time. A
streamer read of 8 kB or more now goes through `read_staged()` too, unpaced.
The buffer is one stage for the board (`sample_player_port_stage_init()`, 4 kB
of DMA-capable internal RAM taken once before BLE), shared a piece at a time
under a mutex by the streamer, the mutators and a bounce's writes; without it
every path is stdio, and the boot logs an E line. A FreeRTOS mutex is not
handed to whoever waits -- the streamer (18) gives it and takes it again before
a waiter on its core runs -- so: the streamer never waits for the stage (a
piece it finds taken is read straight into the voice's buffer, the old
per-sector cost, counted in `stream_fb`; waiting, it would wait at the pace of
a kit load starved on core 1, and the inheritance would stop at the stage's
holder instead of reaching whoever holds the FAT lock); and a bounce piece
waiting for it is urgent -- the streamer sleeps a tick after its piece and the
bounce gets the next one (at most a tick per bounce piece). A kit load is not
urgent: it paces itself on the streamer. `SMPLLAST` reports
`stream_staged`, `stream_fb` (refills, or 4 kB pieces of one, read without the
stage), `stream_kbps` and `stream_read_max_us` (the read, its waits for the
FAT lock included, no fopen), and `late_max_ms` / `late_lost`: how late the
latest held hit sounded and the held hits a newer one on the same pad
replaced, all since the previous `SMPLLAST`. `late_*` measure; nothing here
shortens the window a held hit waits (the Marker's WipeOut + SetupSample).

What a release asks of a swap under play (the same in the CDC and the USB
audio profile -- nothing is relaxed for UAC): at most one late
hit per swap (`late_hits`), none later than 50 ms (`late_max_ms`) and none
replaced (`late_lost=0`); the longest gap between two render passes
(`rt_gap_max_us` in the swap row) at most two blocks, 2902 us, with 0
underruns -- the I2S DMA holds 4 x 64 frames, so two blocks late is still
heard as nothing.

Evidence for a swap: `KIT_SWAP_LOG` over CDC, `crosspad-hil run
kit_swap_audio --hit-rate 2`.

**Testing it:** `crosspad-hil run kit_churn` plays and swaps at the same time,
which is the whole point — a swap that only works from silence is not the
thing being tested — and `kit_swap_audio` listens to it. It reports how many pad hits landed
*inside* each swap window and fails if that is near zero, because a green run
with no stimulus in the window is a false negative.

```bash
crosspad-hil run kit_churn                            # 20 swaps, pads at 8/s
crosspad-hil run kit_churn --rapid 0.4 --rounds 40    # spin the selector
crosspad-hil run kit_churn --no-play                  # the control case
```

`--rapid` fires requests faster than a load can finish, so every one after the
first meets a busy loader — the coalescing path. It asserts the burst settles
on the *last* kit asked for and that the kit still sounds.

Two harness traps worth knowing. Opening the STM VCP console with DTR or RTS
asserted **reboots the ESP** (the bridge emulates esptool's auto-reset), so the
console is always opened with both deasserted. And `PAD_PRESS` replies `OK` just like
`KIT_LOAD` does, so waiting for `"OK"` after a kit request proves nothing while
the pads are running — `KIT_STATUS` is the honest answer.

## Polyphony: how many stereo WAVs at once (measured 2026-10-07)

The budget is one I2S block: **64 frames at 44.1 kHz = 1451 µs**. A render
pass costing more than that is a late block. The DMA (6 × 64 frames = 8.7 ms)
absorbs the odd one; a run of them is a glitch.

Fixture: `tools/make_poly_kit.py` — sixteen *distinct* stereo notes (a
chromatic run from A3), one per pad, same length, same level. Distinct
because identical files would phase-cancel and would also share one
`SampleStore` slot, which is not sixteen samples being read. `--loop` makes a
pad sustain so the voice count can be *verified* rather than assumed.

One v2 board, SD mounted **4-bit at 40 MHz**, native engine, voice cap raised
with `SMPL_POLY 28`.

### Samples held whole in RAM (`complete=16`, no card access)

| voices | worst pass | of the block | faults |
|---|---|---|---|
| 1 | 133 µs | 9 % | none |
| 4 | 470 µs | 32 % | none |
| 8 | 777 µs | 54 % | none |
| 12 | 1177 µs | 81 % | none |
| **16** | **1232 µs** | **85 %** | **none** |

**Sixteen stereo voices, every one verified sounding, with 15 % of the block
left over.** ~73 µs a voice. Sixteen is the pad count, so this is the whole
instrument playing at once.

### Every voice streaming from the card (`complete=0`, 2 s samples)

| asked | actually sounding | worst pass | of the block | faults |
|---|---|---|---|---|
| 8 | 8 | 763 µs | 53 % | none |
| **10** | **10** | **1131 µs** | **78 %** | **none** |
| 11 | 8 | 1249 µs | 86 % | none |
| 12 | 5 | 1367 µs | 94 % | underrun |
| 16 | 7 | 1787 µs | 123 % | underrun |

**Ten clean.** From eleven the engine can no longer keep every voice fed and
they start ending early; from twelve the underrun counter moves. ~110 µs a
voice, half again what a resident voice costs, plus the card's own ceiling.

### Which regime a kit is in

`HeadPlan` gives each sample up to **256 kB** resident, having first set aside
1.5 MB for the stream pool and 512 kB of margin out of the 4605 kB arena. So
a sample up to ~256 kB — **1.45 s of 44.1 kHz stereo**, which is most drum
hits — is held whole and costs the resident price. Anything longer streams.
`SMPL_ENGINE` says which: `complete=` of `slots=`.

A mixed kit sits between the two figures in proportion to how many of its
sounding voices are streaming.

### What would raise the streaming number

Not the render — at ten streaming voices it is at 78 % and the faults are
underruns, i.e. delivery, not arithmetic. The levers are more head per sample
(fewer voices streaming at all) and a deeper ring per voice; both are arena
budget, which the heads and the pool currently split 2048/512 kB.

### Sixteen, and where the time went (2026-10-23)

Measured on a v2 board with the POLYS/POLYO fixtures (`make_poly_kit.py
--seconds 10 [--loop]`), `SD_TRACE` counting every card transaction by
task, `RT_PROF`/`MIXPROF` for the render:

- **The card is not the wall.** Raw (`sdmmc_read_sectors()` timed on its own
  task): 4 kB in 359 us sequential, 721 us random -- ~150/400 us a command
  plus ~50 us a kB on a 4-bit 40 MHz bus. Sixteen stereo voices need 2.8 MB/s and kept it ~46 %
  busy. Fifteen loops streamed with no underrun in the CDC profile, sixteen
  one-shots through a kit change under them likewise.
- **What a 16 kB stream block cost:** 6.6 transactions, one of them a single
  sector into PSRAM (the WAV's 44 B header leaves every block unaligned, and
  the FATFS buffers live in PSRAM). Into PSRAM, IDF's sdmmc does one CMD17 a
  sector through a buffer it mallocs every time: 4 kB in 1513 us against
  359, and "not enough mem" once WiFi has taken the DMA heap at boot.
  **The sector bounce** (`sd_sector_io.cpp`, `-Wl,--wrap=sdmmc_read_sectors`/
  `_write_sectors`) carries such a transfer through one buffer taken at boot,
  sixteen sectors a command (eight when measured): 4 kB into PSRAM 1513 -> 366 us, a 16 kB read into
  PSRAM 9955 -> 2701 us. **It keeps single sectors too**: FATFS
  reads a directory and the FAT a sector a command through one window per
  volume, and every open of a kit's sample walks `/sdcard/crosspad/kits`
  (~190 entries, dozens of sectors) again -- ~700 such reads in a kit's load.
  32 sets of 2 ways in PSRAM (32 kB); a write takes out the sectors it
  covers, every card init (`--wrap=sdmmc_card_init`) everything, a read that
  raced a write is not kept. A kit's heads under an eight-track song
  0.91-1.01 s -> 0.53-0.65 s (68 % hits); `SD_CACHE 0|1`, `hits= misses=` in
  `SD_TRACE`. With it the board's 4 kB stage has nothing left to
  do and is not allocated: the streamer reads a block in one `read()` into
  its ring, the loaders, the kit converter and the bounce write their own
  buffers (`sample_player_port_direct_io()`).
- **The render was the wall in the USB-audio profile.** Sixteen voices
  there: 984 us average, 1378 worst of the 1451 us block. Code runs from
  PSRAM through a 16 kB I-cache both cores share, and the mixer's buffers
  were in PSRAM too. 32 kB of I-cache and the render's own buffers in
  internal RAM (`AlignedVector`): 613-667 average, 1036-1064 worst; the
  internal RAM is paid back by moving the waveform cache table, the OTA
  write buffer, `smpl_pad`'s and `audio_bg`'s stacks and two verb buffers to
  PSRAM -- boot with WiFi up leaves `int_min` within ~1 kB of before.
- `Params::maxStreamingVoices` is sixteen, and a song's two kits play
  eight each.

The kit loader is where the card's time still goes in small pieces: one kit
of 16 samples was 493-1911 transactions of ~1 kB, most of them directory
sectors -- `resolveKitSamplePath()` stats the flat path, then `SAMPLES/`,
for every sample, each a walk of `kits/` -- and `fseek` refilling the 1 kB
stdio buffer for the header parse.

## The native engine under a kit change (2026-10-17)

A kit change while the native engine plays needs four things of its own, each
measured on a v2 board with an STM_VPAD storm (random pads and velocities):

- **Readers give way to the native streamer too.** The native pass stamps
  `g_bg_pass_*` and `sample_player_any_sounding()` counts the native engine's
  *streaming* voices, so every reader that paces itself
  (`sample_player_port_yield_to_streamer()`: the kit loader, the waveform
  loader, the store) waits for a running pass to end before its next piece or
  its next fopen (bounded, `SMPL_PACE` ms, 20 by default; 0 turns the pacing
  off). 10 swaps at 15 hits/s: 16 underruns without, 6 with.
- **Six-block rings, for the card's other users.** Right as a kit lands the
  streamer's opens can wait 180-200 ms on the volume lock behind the loader
  and the LVGL thread (traced with a `--wrap` of `ff_mutex_take`: 31 takes by
  `taskLVGL` while `audio_bg` waited 164 ms -- a FreeRTOS mutex is not handed
  to its waiter across cores). Six 16 kB blocks a ring (465 ms of lookahead,
  a 96 kB page each, core's `Params::streamBlocks`): at 20 hits/s 10 underruns
  in 18 swaps with four, 1 in 30 with six, 0 in 12 with six and a 20 ms pace.
  A "streamer goes first" rule at the FAT lock measures worse (18 against 10).
- **The waveform cache takes the largest block there is**, down to 16 kB: a
  fixed 256 kB buffer is never read back once PSRAM has no such block left
  (66-122 kB the largest after a few swaps), and every swap then computes all
  sixteen waveforms again off the card (WAVE `ok=` +16 a swap -> +1).
- **A pad is loaded when it plays.** A kit can name no sample on any pad
  (`"samples": []`, e.g. from a broken import). The loader asks `padPlayable` per pad: empty
  pads go dark, a sample that did not load dim red, a kit of which nothing
  plays is a failed load with the reason on the loader bar. `KIT_SWAP_LOG`
  `empty= failed= why=`, `SMPL_ENGINE pads=` (pads with a sample in the
  engine) is the other side of `LED_STATE`.

Swap duration under play is the price: at 20 hits/s a 16-pad kit took
5.1-5.5 s without giving way and 6.9-8.9 s with it; with nothing streaming
the pace never engages. `SMPL_STALL ON` records the slowest note-on with the
caller's, `audio_bg`'s and the core's idle time over it.
