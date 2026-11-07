# Hardware-in-the-loop Testing

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

There are no unit tests here. The test suite is
[**crosspad-hil**](https://github.com/CrossPad/crosspad-hil) — a pip-installable
package (from git; not on PyPI yet; library + CLI + NDJSON daemon) that drives a real board — and a
scenario run against hardware is what "tested" means in this repo.

```bash
pip install -r tools/requirements-hil.txt   # git+https form, not on PyPI yet; or: pip install -e "../crosspad-hil[all]"
crosspad-hil devices                        # what is attached, and on which ports
crosspad-hil run <scenario> [--json] [-d dev_xxxx] [flags]
crosspad-hil run <scenario> --help          # what that one takes
```

Setting up the venv, the board lease and reading reports, step by step:
[Test on hardware (HIL)](https://crosspad.app/docs/how-to/hil-testing/).

**Exit code 0** passed, **1** the firmware failed, **2** the test setup is wrong (no
board, port busy, missing dependency) — so "your change broke it" and "plug the
cable in" are distinguishable without reading the output. `--json` prints the
same object the daemon hands crosspad-mcp.

The fifteen scripts that used to live in `tools/` are that package now — one
implementation behind the CLI, the scenarios and the crosspad-mcp daemon. Each
`tools/hil_*.py` left behind is a shim (`tools/_hil_shim.py`): it prints a
deprecation line naming the `crosspad-hil` command and runs the CLI, or exits 2
with the install command when crosspad-hil is missing. The shims are
there for one release; write new invocations against `crosspad-hil`.

The core scenarios (the package's `crosspad_hil/scenarios/` has more, e.g.
`kit_swap_audio`, `pitched_kit`, `song_bounce`, `daw_control`, `fw_update`,
`wifi`, `latency`, `perf`, `power_state`; the full list with every flag is the
[crosspad-hil README](https://github.com/CrossPad/crosspad-hil#scenarios)):

| `crosspad-hil run …` | What it does |
|---|---|
| `app_churn` | open/close every app repeatedly; per-app internal-heap slope + crash watch |
| `back_churn` | go `--depth` levels deep in every app and click the power button back out; crash, launcher and heap watch |
| `audio_loopback` | play a multitone through the UAC2 endpoints and judge the return signal |
| `ble_midi` | drive BLE MIDI both ways from this PC's radio, in both of the board's roles |
| `fs_transfer` | round-trip a file over CDC (CRC + framing), `--assets <img>` reflashes the assets partition |
| `kit_churn` | swap kits while the pads keep firing; `--rapid` spins the selector |
| `led_state` | dump the LED controller model (colours, brightness, anim, power state) |
| `midi_bench` | echo-frame throughput and latency on the ESP port, the STM bridge, and both |
| `midi_stress` | burst SysEx queries at the ESP MIDI port and count the replies (RX loss) |
| `release_gate` | the whole chain, one report: smoke through stability (`--quick` for a CI-length dry run, `--keep-going`; `--help` lists the steps) |
| `rt_glitch` | phased UAC2 capture: clicks under pad/LED load that a held pad does not have |
| `sampler_record` | record the sampler through the board's own UAC2 endpoint and score it |
| `smoke` | reset, capture the boot log, check markers / E-lines / fatals / boot loops |
| `speaker_acoustic` | record the built-in mics while the sampler plays; did the speaker sound? |
| `speedtest` | ramp the pad hit rate until the device drops hits, per pad set |
| `stability` | soak: watch the console for resets, fatals, stalls, bad boots and heap drift |
| `usb_mode_cycle` | cycle CDC+MIDI <-> MIDI+UAC2 and watch enumeration, heap and faults |
| `velocity` | sweep pad velocity and measure loudness, monotonicity and trigger jitter |
| `waveform_cache` | load kits and watch `WAVE_STATUS`: does every pad layer get its waveform cached? |

Audio, BLE and raw-ALSA scenarios need the optional extras
(`crosspad-hil[all]`, which `tools/requirements-hil.txt` installs); without them
they exit 2 naming what is missing instead of raising an `ImportError` halfway
through a run.

The ones you will reach for while developing:

```bash
crosspad-hil run smoke                    # after every flash: did it boot?
crosspad-hil run beat [--depth medium|full]   # THE main test: make a beat, see below
crosspad-hil run app_churn --rounds 10    # app lifecycle leaks and crashes
crosspad-hil run back_churn               # go three levels deep, click back out
                                          # (plays two pads per Sampler visit; --pads 0 to hush)
crosspad-hil run kit_churn --rapid 0.4    # kit swaps while the pads keep firing
crosspad-hil run kit_swap_audio           # the same swaps, recorded and scored per tone
crosspad-hil run usb_mode_cycle           # CDC <-> UAC2 re-enumeration
crosspad-hil run stability --duration-hours 8 --stim-midi   # liveness = MEM over CDC every 30 s
crosspad-hil run sampler_record --kit-name DRUMS   # record what it plays
crosspad-hil run ble_midi --kit 8         # BLE MIDI both ways, from this PC
crosspad-hil run sampler_record --stim ble --kit-name DRUMS  # BLE + sampler + UAC2
crosspad-hil run play_perf                # MIDI -> sound: latency, jitter, load, two-kit song
crosspad-hil run fs_transfer --assets build_v2/assets.bin   # CDC transfer + assets reflash
crosspad-hil run release_gate --keep-going              # overnight, before a tag
```

**`beat` is the main end-to-end test** — the user's core workflow as one
file, `crosspad_hil/workflows/beat.yaml`, steps tagged `quick` (default, ~3
min), `medium`, `full`. No microphone: the run writes a loop of eight known
hits at eight pitches, the Slicer cuts it (onsets checked to the millisecond),
the cuts become a kit, a beat is sequenced on it (SEQ_DUMP against the
pattern), overdubbed, doubled, pad-edited in the pad editor through the
encoder, mixed, bounced — and the bounced WAV is pulled and every hit is found
at its time and its pad's pitch (an octave up for the +12 pad, a swell for
the reversed one). A theme colour is changed through Settings and put back.
It runs in a song and kit of its own (`HIL_BEAT_<tag>`, `BEAT_<TAG>`, deleted
at the end) and puts back the song, kit, app, desk and theme it found; it
refuses (exit 2) a board someone is playing. Verbs it added: `KIT_DELETE id
name`, `ASONG CUT track bar`, `ASONG DUPREGION`. The same file drives the
PC simulator for Mucha (`perform/perform_beat.py`), where the fruit fly plays
it on video. `release_gate` runs it after `perf`.

**Pad volumes from the host.** `PAD_MIX` lists the current kit's sixteen
volumes (`PADMIX: kit= max= loading= name=`, a row per pad, `PADMIX: end
count=16`); `PAD_MIX pad vol` sets one live, in the kit's units, the way the
Pad Mixer app's fader does -- parked per pad on the CDC task (newest wins) and
applied on the LVGL thread, only to the kit it was asked for. `KIT_SAVE` writes
the current kit into its kit.json there (the Sampler's save; refused while a
kit loads, and never for an unparsed kit), `KIT_SAVE_STATUS` says when it is
done. `crosspad-hil padmix` is the TUI over them.

`sampler_record` is the one scenario that needs only the CrossPad's own cable:
CDC to load a kit and start the Sampler, then the USB-audio profile with pads
driven over SysEx and the UAC2 capture endpoint recorded to a WAV. The
capture path has to be the DAC→ADC loop (`--preset headphone`, codec1 LINE1),
not the mics. The RT mixer runs in the USB-audio profile too: it is the only
I2S writer, host playback enters it as the "USB" channel and the capture is a
tap in its render, so `audio_loopback` and `sampler_record` both need it
running. Only `AUDIO_TASKS 0` / SysEx `1D 06 00` parks it, and the next USB
mode switch resumes it.

```bash
crosspad-hil kit list                                        # kits on the card
crosspad-hil run sampler_record --mode live --duration 300   # play it yourself
```

More of what `sampler_record` does and why:

- **Control and audio on one connector.** It loads a kit and starts the Sampler
  over CDC, switches to the USB-audio profile, then drives the pads over SysEx
  while recording the UAC2 capture endpoint — the whole point when the
  alternative is an interface and a second cable on stage. `--preset headphone`
  (codec1 LINE1, the default) is near unity and measured ~12 dB quieter in
  noise than the codec0 loop.
- **WASAPI exclusive mode where it exists** (`crosspad_hil/capture.py`): the
  shared path puts the Windows capture mixer's endpoint volume between the
  device and the file, which silently clipped otherwise clean takes.
- **One rate, the device's.** The endpoint reports the single rate the audio
  engine runs at (see [audio-engine-rate.md](audio-engine-rate.md)), so the
  recorder follows the device rather than accepting a rate that would only fail
  when the stream opens.
- **Level.** Left at the board's own volume the DAC→ADC loop clips, and a
  clipped transient is one the onset analysis cannot find. `--volume` sets the
  capture codec for the take (default 20, ~−21 dBFS peak) and the board's
  volume is put back afterwards.
- **Reading a speedtest.** Per rate step it reports how many injected hits were
  *audible* — the flux curve is aligned against the known injection schedule
  rather than peak-picked, because a drum sample's decay retriggers any blind
  onset detector. Read `captured N s of M s` first: a capture that stalls still
  produces a valid file with a respectable peak, and only the missing seconds
  give it away.

`ble_midi` is the only scenario whose second half runs on the host's own
Bluetooth radio: it scans for the board, connects to the BLE MIDI service,
subscribes, then plays notes at it while the CDC port answers what the device
saw. Each leg is separately falsifiable — `BLE_STATUS` counts messages parsed
off the radio, `PAD_STATS` counts what reached the sample engine, so "the note
never arrived" and "the note arrived and made no sound" are different failures.
`--host-mode` adds the other role: the PC advertises and the board scans,
connects and subscribes. It needs `bleak` (`crosspad-hil[ble]`) and the host
radio on; the board must be in **Server** mode, which the scenario sets and
then puts the old role back. `--stim ble` on `sampler_record` is the combined
case — pads over the radio while the same USB cable streams the audio back, the
only test with BLE, the sampler and the UAC2 endpoint all running at once.

Recordings can also be taken and judged by hand: `crosspad-hil capture` writes a
WAV, `crosspad-hil analyze <kind> <wav>` returns one verdict (`sine`, `click`,
`silence`, `psd`, `multitone`, `onset`, `velocity`):

```bash
crosspad-hil capture --seconds 20 --preset headphone --out /tmp/take.wav
crosspad-hil analyze sine /tmp/take.wav --opt freq=997     # slips, drift, glitch rate
crosspad-hil analyze silence /tmp/take.wav                 # the capture path with nothing playing
crosspad-hil analyze psd /tmp/take.wav --opt freq=997      # sidebands, harmonics, hum
```

`analyze sine` is the deep one — it reads the phase of a steady tone, so a stream that drops or repeats
samples shows up even though its level and spectrum look perfect, and
`--opt compare='[…]'` scores whether the glitches land at the same sample offset
on every take. `tools/glitch_capture.py` is deliberately not a scenario: it
plays references *into* the UAC2 playback endpoint with `aplay`, which is a
research rig for one audio defect, not a test with a verdict (`arecord` takes
the return; analyse what it leaves in its workdir with the commands above).

Control lands on the CrossPad's own CDC (`0x303A`); the console comes off the
STM32 bridge VCP (`0x0483:0x5740`), which survives ESP resets and USB mode
switches. Anything long-running listens on the latter.

`main/hil_control.cpp` implements the device side of the command set (`APP_*`,
`KIT_*`, `PAD_*`, `ENC_*`, `LED_STATE`, `SMPL_*`) — that file is still where a
new verb is added, and `crosspad_hil/verbs.py` is where the host learns to speak
it. A verb the simulator's control port answers too reads its state through
crosspad-core's `crosspad/control/` (KIT_STATUS/LIST/LOAD, LED_STATE, MIX_LVL,
PITCHED_STATUS, WAVE_STATUS, AUDIO_LEVEL, PAD_*, APP_START): core gathers it
once, `main/hil_text.cpp` renders the CDC line (no ESP-IDF call, so
`main/tests/` pins the exact text on the host:
`cmake -S main/tests -B main/tests/build && cmake --build main/tests/build &&
ctest --test-dir main/tests/build`), and crosspad-pc renders the JSON. A field
added to one of those verbs goes into the core struct first.

**`app_churn` does not exercise the kit-selector gate** — it
seeds a kit itself and skips it on purpose, so a gate regression needs a
manual repro on a fresh boot (`KIT_STATUS` → `current=-1`): `APP_START Sampler`
→ `APP_STOP` → `APP_START Sampler` → `ENC_GROUP`. A `release_gate` scenario
exists in `crosspad-hil` (`--quick`, `--keep-going`, RAM read before and after
each step; `crosspad-hil run release_gate --help` lists its steps). The periodic PerfMon dump is off — a ~50-line
blocking-UART write from priority 1 every 10 s stretches CDC replies past
`kit_churn`'s 2 s timeout on its own; `TASKS`/`MEM` give the same numbers
on demand.

### How the board plays: `play_perf`

What a player and a DAW feel, measured in one take (~1.5 min): note-ons sent
to the **STM bridge's** MIDI port -- the UART path a struck pad's note takes; a
note-on on the ESP's own port only lights a pad -- and the mixer's **capture
bus** recorded over UAC2 (`--preset sampler`, digital: codec 1's DAC->ADC loop,
`--preset headphone`, carries -35 dBFS RMS of broadband noise whatever the
volume and buries a click). Every send is stamped with the capture stream's
clock; PortAudio's ADC times on macOS come a buffer at a time (single marks off
by up to ~6 ms), so a send is mapped through the median of the marks around it
(`TimeBase`), which also steps over frames the host loses mid-take -- reported
as `capture_gaps`, and a hit inside one is not counted as lost. Latency is then
absolute *as the host sees it*: the USB capture path is in it, the I2S block
out and the DAC are not.

Phases: isolated hits (the baseline), host sixteenths at the song's tempo,
rising rates (8..32 hits/s), the two-kit test song alone (`HIL PERF`: kit A on
the eighths, the second kit a sixteenth later -- played from its freeze since
core 2.42 --, notes at velocity 40 so a live
hit's click -- velocity 120 -- is told from a song note by its height) and hits
played over it. Around each phase `SMPL_ENGINE`, `SMPL_PEAK`,
`PAD_STATS` and `UAC_STATS` are read over SysEx (and `ASONG PLAY`/`STOP` sent),
which is why the companion allowlist has them. It builds `HIL CLICK A`/`B`
(12 ms bursts at 1.2 and 4.4 kHz, told apart by a band-pass) and the song,
mutes codec 0 for the run, and opens the board's own song again at the end.

```bash
crosspad-hil run play_perf                         # everything, defaults
crosspad-hil run play_perf --rates 8,16,32,48 --bpm 140
```

Baseline, v2, 15.10.2026 (two kits up, 5 voices each): MIDI ->
sound 17.0 ms median, p95 17.8, max 18.0, spread 0.6 ms; every hit sounds up
to 32/s and the firmware counts no drop, steal or underrun, the worst render
block 0.11-0.16 ms of 1.45; the song's two kits keep their grids to 0.8-0.9 ms
(the sequencer fires on its 2 ms poll), kit B 0.3-1.1 ms early against kit A
(its notes go straight to its engine, kit A's through the pads' logic and the
event bus), and as steady with hits played over the song.

### Kit swaps you can hear

`kit_churn` proves a swap does not crash or wedge the loader; `SMPL_PEAK` only
says "something rendered". `kit_swap_audio` asks the harder question — was the
swap *seamless* — by recording the board through its own UAC2 endpoint while the
pads fire and the kit changes under them, then scoring the recording per hit.

```bash
crosspad-hil run kit_swap_audio                # 3 swaps, 4 hits/s, ~24 s + tail
crosspad-hil run kit_swap_audio --swaps 6 --hit-rate 8 --dwell 8
crosspad-hil run kit_swap_audio --no-provision # refuse to write to the card
python3 tools/make_sine_kits.py                 # push the fixtures by hand instead
```

**The scenario provisions its own fixtures.** A card without `HIL SINE A`/`HIL
SINE B` is something the host can fix rather than a run that dies on `no kit
named`: the generator lives in `crosspad_hil/fixtures.py`, the push is `FS_PUT`,
and the board is then reset through the STM bridge because the kit scan runs
once, after the mount. It waits for the banner, waits out `KIT_LIST`'s
`ERR scanning`, and only then continues; `data["provisioned"]` names what it
pushed. Anything that fails along the way is exit 2, not a firmware verdict.
`tools/make_sine_kits.py` writes the same kits byte for byte (it prefers the
crosspad-hil copy when it is importable, and crosspad-hil's `test_fixtures.py`
compares the two implementations file by file).

The two fixture kits give every pad a different pure sine — kit A `300 + 222*p`
Hz, kit B `411 + 222*p` Hz, interleaved 111 Hz apart with no harmonic collision
inside the ±15 Hz measurement band — so "which kit is pad p loaded with" is a
question about two named frequency bands, answered per 20 ms frame. Three
things then FAIL the run: a hit that made no sound, a hit whose tone has a hole
longer than `--max-gap-ms` (default 5 ms; one 64-frame block at 44.1 kHz is
1.45 ms), and a hit whose tone changes frequency mid-window — that last one is
a pad reloaded while it was still sounding, which is exactly what the loader
must not do. A pad still playing the *old* kit after a swap was requested is
fine and expected; a pad going *back* to an older kit later is not.

Two refinements. A switch has to hold **three frames
(60 ms)** to count: clean tones flip a single frame to the other candidate on a
harmonic, and a pad cannot be reloaded for 20 ms and back again — the raw count
is still reported beside the run-filtered one. And a hole at the *start* of a
hit is a late start, not a dropout: the voice waited for its first refill (a pad
hit at t=5.50 whose tone began at ~5.72). That leading run is reported as
`late_start_ms`, excluded from `max_gap_ms`, and gated by `--max-late-ms`
(default 20 ms) — folded into the gap rule, a late start would read as a
dropout in a tone that then plays perfectly.

The kit-load request travels as SysEx `F0 7D 1D 0B <kit>`, not CDC: the
USB-audio profile has no CDC endpoint and the recording cannot be paused to go
get one. `KIT_SWAP_LOG` is read afterwards, over CDC, and lands in the report
verbatim — per swap the `done_ms`, the render time the swap gate cost
(`gated_blocks`, `gate_max_us`) and the `deferred`/`forced` pad masks. It is
evidence, never a gate: firmware that predates the verb still gets a verdict
from the audio.

### Pad storms: the STM plays, the ESP is measured

"Notes queue up when I play fast" cannot be reproduced with `PAD_PRESS` —
that enters at `handlePadPress()` and skips the STM, the UART link and the
parser, which is most of the chain. The STM carries a virtual pad
stimulus (`vpad.c`, proto 0x000D): hits leave through `buttons_inject_pad()`,
the same note math and the same two sinks (`cbEspTx`, `cbMidiUsbTx`) a
physical hit uses, so what the ESP sees is what a player produces: note-on,
then while the pad is held a pressure stream through the pad's own analog
mode at the real streamer's cadence and change threshold (`REG_CTL_ANALOG_*`),
then note-off. `hold_ms` (default 100) is how long the finger stays down.

```
STM_VPAD <rate_hz> <count> [hold_ms] [mode]   # start; mode bits 1:0 pad rr/single/random, 3:2 vel sweep/random/fixed
STM_VPAD                                      # STM counters: sent, at_sent (pressure samples), remaining, bytes short on each sink,
                                              # DMA/USB busy iterations, ring high-water marks
STM_VPAD STOP
MIDI_TRS [TYPE A|B|SKEW n|FLIP seg N|RESET|ROUTE r0 r1|ROUTE SAVED]
            # TRS MIDI pair (STM 1.14): type, skew, injector, routing, per-segment
            # counters (docs/subsystems/trs-midi.md); scenario midi_trs_loop
MIDI_LOOP [CAPTURE 0|1|SEND count rate|STOP]
            # the ESP's end of the loop test: numbered note-ons sent / counted in order
PAD_STATS   # + stm= (presses parsed) at= (pressure samples) rxmsg= uart_ovf= sbuf_drop= usb_tx= usb_drop=
SMPL_PEAK   # + rt_max_us= (longest render call) noblock= underrun= (engine error reports)
            #   rt_busy_max_us= (whole pass, I2S waits excluded; read-and-clear, its own copy) overruns=
SMPL_LAST   # + opens/reads/closes per task, defer= (RT hand-offs), evict=, lat_us pad→engine last/avg/max
            #   stream_staged= stream_fb= stream_kbps= stream_read_max_us= late_max_ms= late_lost=
LED_STATE [COALESCE 0|1]  # LEDS: … txfail= refreshes= (frames sent; still at rest)
TASKS       # FreeRTOS run-time stats to the console; read twice, diff
            # (the USB-audio profile's 'SPK #' line carries rt_busy_max_us= idle1_pct=, and
            #  comes on an under/refused change at most once a second; the engine's error
            #  line, printed off audio_rt at most once a second, rt_busy_max=. Each of
            #  SPK, the error line, SMPL_PEAK and PITCHED_STATUS clears only its own peak)
MEM         # + dma_free= dma_largest= dma_min= (DMA-capable internal RAM; int_* includes
            #   ~8 kB of RTC fast memory no DMA can use -- the 63 kB floor is in dma_*)
SMPL_POLY n # prebuilt engine's voice cap (SetMaxPolyphony), test knob; holds over
            #   the port's 16 across loads until SMPL_POLY 0
SMPL_POOL, SMPL_BKG  # ERR only_native in an image without the prebuilt engine
```

Every stage has a counter, so a loss shows where it happens and not only at
the last place the counters reach. Measured: the STM sinks never go short
below 800 hits/s, the UART link overflows only at 400/s, and USB MIDI out of
the ESP trails the STM's own port by 2–3 ms flat.

**The prebuilt engine's polyphony cap must be set.**
`SampleStreamPlayer_SetMaxPolyphony()` (32 channels) is called right after
`SampleStreamPlayer_Init()`. Without a cap, sixteen pads in flight get
"no free block" at the first retrigger, the render call grows past the
1.33 ms I2S period, `audio_rt` holds core 1 and the task watchdog fires five
seconds later with `SampleStreamPly_Process` on top — a sixteen-pad
round-robin at 40 hits/s reboots the board inside a second. The library closes
files from the render task (`SampleStreamPlayer_FileClose`), its handles are
shared with `audio_bg` (a handle closed twice asserts in `_fclose_r`), and an
`fclose()` on the render task is a FAT directory sync, so
`sample_player_port.cpp` moves every hand-off under a spinlock and parks what
the render closes; the next open closes it.

What is left is not a cliff: at 25 random hits/s for four minutes the
pad→engine latency stays at ~1.3 ms average, 5 ms worst, with no drift, and
the engine reports about one underrun a second while `taskLVGL` takes half
of core 0 redrawing per hit. A reboot zeroes every counter, so poll *during*
the burst; a CDC reply that comes back empty under load is a main loop that
answered late, not a dead board.

### Reset black box

A spontaneous reboot leaves a trace, readable later over the CrossPad's
own CDC — no log has to be attached when it happens (`main/reset_log.cpp`):

- `RESET_LOG` — ring of the last 8 boots (NVS): ESP reset reason, what the
  STM counted on the ROM banner (`stm_last`/`stm_total`) and whether **its
  link watchdog** forced the reset (`stm_wd`, from `REG_ESP_RST_WD_*`, proto
  0x000C), previous uptime (5 min granularity) and the app that was up.
  `RESET_LOG STM` reads the STM counters live; `RESET_LOG CLEAR` wipes ring
  and core dump.
- `COREDUMP_INFO` — panic and task-WDT write an ELF core dump to the
  `coredump` partition (`ESP_COREDUMP_ENABLE_TO_FLASH`, `ESP_TASK_WDT_PANIC`);
  this prints task, PC, cause and the backtrace, ready for
  `xtensa-esp32s3-elf-addr2line -e build_<rev>/CrossPad.elf`. The dump belongs to
  the ring entry marked `COREDUMP`.
- `RESET_TEST panic|wdt|stmwd` kills the device each way on purpose; the
  three signatures are `PANIC`+dump, `TASK_WDT`+dump, and
  `POWERON stm_last=0x01 stm_wd+1` — the last one is the STM resetting a
  silent ESP, which no ESP-side log would ever show.
- `STM_REG <hexaddr> [len]` — raw register-map read; `0x1000` is the STM's
  live LPUART2/DMA/banner-parser debug block for when no SWD probe is attached.
- `STM_REGW <hexaddr> <hexval>` — raw register-map write beside it, for
  experiments; not saved, the next settings push restores the stored value.

Two host-side facts: Linux cdc-acm programs 9600 8N1 into the STM VCP at
enumeration, which the STM ignores (an unsupportable baud would detune the
bridge from the ESP console); and the ESP's boot banner opens the STM's boot
window, so a panic reboot is not followed by an STM watchdog reset. **Use
`ENC_GROUP` before driving the UI** (`crosspad-hil ui snapshot`) — it
dumps the encoder group with labels, so you can tell a row that was never built
from one the encoder has not reached. Counting distinct object pointers while
rotating cannot distinguish those two.

**A scenario that tests what a player does drives the screen, not the state verbs.**
`UI_TREE` (crosspad-gui `ui_tree_probe`) answers one `UITREE:` row per visible widget
of the screen and the top layer: class, rectangle in LCD pixels, `int` (clickable or
in the encoder group), `hit` (an `lv_indev_search_obj` from its middle lands on it or
inside it -- a map button under an app, or a row under a dialog, is drawn but not
`hit`), checked/disabled/focused, then its text. crosspad-hil's `ui.Ui` taps a control
by what it says (`TOUCH_TAP` on its middle, or on the label when the words are a row
of a list), scrolls a list with `TOUCH_DRAG` until a row shows, turns the knob to a
control no finger reaches (the Sequencer's top row), and fails with what the screen
offered instead. State verbs then only read the outcome. Traps when driving
the screen (`studio_session` does): a list keeps gliding after a drag (wait for the row to stop
before tapping), the first drag on a list only gives it the focus, a desktop mounts
the USB drive by itself, and USB transfer mode keeps CDC but boots no app registry.

The CDC command path has limits that shape how hard anything may drive it,
and none of them may fail silently — that is worse than failing, because a
lost command is indistinguishable from a handler that ignored it.

- **One read is not one command.** USB does not preserve the host's write
  boundaries; send faster than the prio-1 main loop drains `app_queue` and two
  commands arrive in one read. Every handler matches on a prefix, so
  `cdc_dispatch_lines()` in `main/main.cpp` frames on newlines and carries a partial tail across
  reads, dispatching each command separately. OTA is exempt: once `OTA_BEGIN`
  is accepted the stream is firmware bytes, where `0x0A` is data.
- **An OTA over CDC can wait for a person.** Unless the Firmware app's
  "Auto-accept updates over USB" is on, `OTA_BEGIN` answers `OTA_WAIT` and the
  board shows an Allow prompt nobody at an unattended board will press.
  `OTA_PREFS` reads it without changing anything (`OTA_PREFS: auto_accept=0|1`,
  NVS `ota_prefs/auto_accept`), so a job can tell before it flashes.
- **`app_queue` is 64 deep and drops on overflow.** Those drops are counted and
  reported (`CDC: N commands dropped`) from the idle tick — from the idle tick
  specifically, because a line per drop would pile console traffic onto the
  moment the console is why the queue overflowed. Anything that cares whether
  its command arrived should read that counter rather than assume.

- **Nothing that can block belongs on the drain loop.** It runs at priority 1
  and it is what answers every off-device query, so anything it waits for
  delays every later reply. The SD hotplug probe runs in `sd_hotplug_task`
  (`sdmmc_get_status()` takes the SDMMC transaction lock, so behind a kit load
  streaming samples it can wait up to **3.1 s**), and the LVGL lock is waited
  on by `lv_async_fwd` (`main/lv_async_safe.cpp`) with the caller only
  queueing (the LVGL port lock is held for a frame, and a kit load ends with
  pad reloads doing SD reads on the LVGL thread — **691 ms** measured from
  `KIT_LOAD`). `CDC_STATS` reports
  `maxcmd_us` (longest single handler) and `maxgap_us` (longest wait for the
  loop with commands already queued), both read-and-cleared, so bracketing a
  window says which of the two a late reply was. `LVGL_STATS` does the same
  for the UI thread: refresh cycles (max/avg), render time, flush wait, flushed
  and invalidated pixels since the last read — two calls around an encoder
  burst price the redraw (kit selector, 2026-09-02: ~95 ms of software
  rendering per detent for ~30 k pixels, ~11 ms of flush wait).
- **Every verb shares the main task's stack** (5 kB of internal RAM, under
  `hil_control_handle_cdc()`'s 1.2 kB frame, and a `snprintf()` is ~1 kB more).
  Over the edge is a panic, `task=main cause=65` (the stack-end watchpoint).
  `CDC_STATS` says how close: `stack_min` is the least any command since boot
  left free and `stack_verb` the verb, `stack_last` what the CDC command before
  it left -- the stack is filled again before each command, so that is the
  verb's own. 2026-10-13: `MIX_PRESET` (preset files + JSON) 3584 B,
  `FW_DOWNLOAD` (NVS) 3440, `WAVE_STATUS` 2960, a plain reply ~2.5 kB. Work
  deeper than that goes to a task of its own; `main/CMakeLists.txt` makes a
  frame over 1280 B in `hil_control.cpp` a build error.
- **A quiet console is not a hung board.** With the PerfMon heartbeat off, an
  idle firmware prints nothing for minutes; the
  stability soak therefore judges liveness by a `MEM` probe over CDC every
  `--probe-interval` seconds (default 30) and counts an unanswered probe as the
  stall. Console silence is only the rule when there is no CDC port.

Related: the per-command hex dump is `ESP_LOGD`, not `ESP_LOGI`. At INFO it
costs ~200 bytes and ~17 ms of blocking UART per command from the main loop,
which caps the command path well below the rate a pad stimulus reaches. Raise
the log level when you actually want to see the bytes.

A reply prefix is not an acknowledgement of *your* command: `PAD_PRESS` answers
`OK` exactly like `KIT_LOAD` does. Under pad traffic the first `OK` off the
wire is almost never the one you are waiting for — ask a state command
(`KIT_STATUS`, `APP_LIST`) instead. The `OK` means *queued*: injected hits play
from the `hil_pad` task (same priority as the STM link that plays physical
pads), never from the drain loop — a hit waits on the card while a kit loads,
and in the loop that would hold every reply behind it. `ERR busy` is a full queue
(32 deep; `PAD_STATS inj_drop` counts them).

Opening the STM32 bridge VCP with DTR or RTS asserted **reboots the ESP** — the
bridge emulates esptool's auto-reset — and the CDC control port then takes
several seconds to re-enumerate. Deassert both when you only want to read the
console. There is no CDC at all in the USB-audio profile — which is what an
unprovisioned board boots into, so the first thing to do with a fresh
one is `usb-mode set default`; control moves to SysEx
`0x1D` there.

These traps are encoded once inside the package — port opening, mode switching
and reply matching all live there, and nothing above it opens a serial port — so
they bite when you talk to a board *outside* it, which includes any throwaway
script.

Do not measure stability with an ST-Link attached: pyOCD sessions perturb the
STM32's USB bridge.

## The render loop has no clock of its own

`audio_rt` runs at priority 19 on core 1, and `taskYIELD()` at the end of its
loop yields to *equal or higher* priority only — there is nothing above it.
The loop's only brake is the blocking I2S write, so
`IdfI2sStream::pushSamples()` checks `i2s_channel_write`'s return: a channel
whose handle exists but is not enabled (a profile switch, a codec re-init, a
rate change — all of which happen in the USB-audio profile) answers
`ESP_ERR_INVALID_STATE` at once, and `isOpen()` only tests the handle. A
render that finishes early and loses its brake owns core 1. What that looks
like on the STM console:

```
E task_wdt: Task watchdog got triggered: IDLE1 (CPU 1)
   CPU 1: audio_rt
usb_audio_bridge: cap_ring=128 cap_drop=574318
CLOCK: max_late_us=49543 restarts=2
```

`IDLE1` never running means one task owns core 1; a `cap_drop` growing faster
than the render can legitimately produce blocks means the render runs ahead
of real time; an OTA over CDC at ~5 KB/s instead of ~137 KB/s is the same
starvation measured from the host. `SMPL_PEAK`'s `unpaced=` counts it, and is
0 on a healthy board.

`TASKS` says how much of a core `audio_rt` takes, not which part of the pass.
Two profilers do:

- `RT_PROF` times six spans of a render pass: begin, mixer, tap, push,
  capture, meter.
- `MIXPROF` (inside AudioMixerEngine) times every channel's node and the
  four stages around them.

Two rules they hold the render to:

- **An input that is not live does not wait for a block.**
  `IdfI2sInput::read()` consults `live_` (false at boot, when both codecs sit
  on the DAC loop) before its read, the gain and the hand-off to the sample
  tools, and bounds its wait to a tick. Asked after a 3 ms read, an unrouted
  "Mic" channel cost 704 µs of the 1450 µs block, against 16 µs for "Line"
  beside it: a 44× split between two instances of one node is waiting, not
  computation.
- **A channel whose own peak is exactly zero is skipped** rather than panned
  and summed into each of its sends: nine mixer channels exist, two or three
  sound. That is the idle mixer render 396 → 241 µs and the worst pass under
  pads 1893 → 1473 µs, inside the block.

At the launcher with no kit, both together take `mix_us` from 1354 µs to
241 µs. With a scene playing the render sleeps over half of every block in the
I2S write (`push_us`), which is where a paced loop belongs.

**Method:**

1. **A capability gate is a silent fallback.** With two engines,
   `if (!engine_ready()) return;` means *do nothing* for the engine that is
   present.
2. **A discarded error code is a missing clock.** The only brake on the one
   task that must never spin is a return value.
3. **Measure whose time it is before optimising anything.** `mix_us` can look
   like work and be mostly *waiting*; `TASKS` puts audio_rt's CPU at the same
   42 % whether the Mic channel waits or not.
4. **A reported cause is a hypothesis, and the cheapest instrument that can
   refute it comes first.** For a glitch reported under kit loads,
   `LVGL_STATS` across a whole swap (`render_us=3343`, two flushes) rules out
   the graphics, the same pads with and without a load underneath (1893
   against 1996 µs worst, `KIT_SWAP_LOG` `gated_blocks=0 underruns=0`) rule
   out the loader, and `MIXPROF` finds the time in the routing stage.

Verbs: `RT_PROF 1` to start, `RT_PROF` to read and clear, `RT_PROF 0` to stop;
`SMPL_PEAK` reports `unpaced=`. Both are on the companion SysEx allowlist,
because the profile where the render is under the most pressure is the one
with no CDC.

## Card and USB-audio probes

- `SD_TRACE` -- every card transaction since the last call, by task
  (`audio_bg`, `song_io`, `kit_load`, `lvgl`, `other`) and direction: calls,
  sectors, how many went to or from PSRAM (`ext`) and how many were one
  sector (`single`), busy/avg/max us. One line; reads and clears. Also over
  the companion SysEx, for the profile with no CDC.
- `SD_BOUNCE 0|1` -- the card's sector bounce (`sd_sector_io.cpp`) off and
  on, for an A/B; `SD_BENCH` measures the card itself.
- `UAC_STATS` ends with `asrc_ppm asrc_cyc_max asrc_read asrc_loop`: the
  playback resampler's ratio and cost.
- `INPUT_MON MIC|LINE|MICLINE|USB [0|1]` -- an input heard live or not, as
  an audio track's MONITOR in Arrange sets it (`IInputMonitor`: the Mixer's
  MONITOR at all or off for that input's channels); bare form reports
  `INPUT_MON: <input> 0|1`. CDC only: it changes the desk.
- `USB_HOST [AUTO|MAC|LINUX]` -- the host computer USB audio is tuned for
  (feedback shape, resampler range; audio-routing.md), applied at once and
  persisted; `USB_HOST: mode=auto|macOS|Linux host=<in force>
  detected=none|macOS|Linux`. Also over the companion SysEx, the only control
  plane of the USB-audio profile.
- `USB_ENUM [first]` -- the setup packets since the last bus reset (32
  kept), eight a reply as 16 hex digits each (bmRequestType bRequest wValue
  wIndex wLength, little-endian as sent), and `n=` how many came in all: the
  evidence Auto decided on.
- `UAC_PKT` -- `pk sh skip dbl late_us sof_gap_us core t`: speaker packets
  received and those short of the rate's whole frames a millisecond, frames
  skipped between two packets and frames with two, the latest a packet's
  interrupt ran into its frame and the longest gap between two SOFs (both us,
  reset on read), and the core the USB interrupt runs on (1). `skip` above 0
  is the board losing packets, not the host sending fewer.
- `UAC_LATE` -- the tasks a SOF more than 1.3 ms late interrupted, with
  counts: whoever held that core with interrupts off (e.g. taskLVGL walking
  the PSRAM heap).
- `UAC_FLOW` -- `fb host taken mic lost rs txu zh zp t`: the feedback sent,
  the playback and capture streams' frame counts since boot, the pacer's
  resyncs and TX underruns, and runs of silence in the host's stream as
  received and as played -- short enough for one companion SysEx frame; read
  twice for the rates (audio-routing.md).
