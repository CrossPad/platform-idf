# Driving a board and testing on hardware

Contributor notes: bench.py, the trap table, crosspad-hil scenarios, the CDC verbs. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Driving Your Board (start here before any device work)

`python3 tools/bench.py` is the entry point: it checks and prepares the board
connected over USB, so the traps below are handled for you.

```bash
python3 tools/bench.py check              # what is wrong + the command that fixes it
python3 tools/bench.py ready              # CDC profile, MICS route, a kit loaded
python3 tools/bench.py flash [--assets]   # flash the NEWEST build, prove it landed
python3 tools/bench.py test smoke ...     # scenarios, with the flags each one needs
python3 tools/bench.py recover            # UART-reflash via the STM bridge (no CDC left)
python3 tools/bench.py hil <args>         # raw crosspad-hil, venv resolved for you
```

Exit codes match crosspad-hil: **0** fine, **1** the firmware is at fault, **2**
the test setup is. `test` re-prepares the board before *each* scenario, because a
scenario that resets the board (smoke does) leaves it with no kit loaded and the
next one then fails as a setup problem instead of telling you anything.

| Trap | What you see | Why |
|---|---|---|
| `python` is not installed | `python: no such file or directory` | many Linux systems ship only `python3` — every doc here says `python3` |
| `crosspad-hil` is not on PATH | `command not found` | it lives in its repo's venv; use `bench.py hil` |
| Board in the MIDI+UAC2 profile | every verb "no reply", board looks dead | that profile is the **default** and has **no CDC endpoint**; `bench.py ready` switches back over SysEx, and the switch persists |
| Flashed, nothing changed | smoke passes, the change is absent | the image came from another build dir than `build_<rev>/`, or it rolled back because the board was reset inside its ~10 s trial window (an OTA over CDC is a trial boot — `bench.py flash` waits for the confirm and compares `APP_SHA` against `sha256sum build_<rev>/CrossPad.elf`, which `LIBVER` alone cannot do). Always confirm `LIBVER:` after OTA and `FS_LIST /assets` after assets |
| Icon/asset unchanged after OTA | old file still listed | OTA never touches the assets partition — `bench.py flash --assets` |
| Flashed a v1 image on a v2 board | reboots every ~19.5 s, CDC enumerates and never answers | hardware and image revision differ; `bench.py check` says MISMATCH, `bench.py recover --build build_v2` fixes it |
| OTA hangs: `expected OTA_READY, got: ''` and every verb unanswered after | the board logs `OTA started`, the host never sees the reply | the crosspad-hil MIDI hub keeps the CDC port open 5 s after each verb it carries and its reader takes `OTA_READY`; `ota_flash.py` takes the hub's exclusive op (run it with crosspad-hil's python, as `bench.py flash` does). The board is then in a half-open OTA session: `crosspad-hil console --reset` (no other console reader on the STM VCP, or the reset silently fails) |
| Scenario exits 2 with "seed kit" | setup problem, not firmware | no kit loaded; `bench.py ready` |
| `KIT_LIST` times out right after boot | "device is talking but never answered" | the boot scan holds the kit vector; retry, it is not a hang |
| Every audio test fails on good hardware | e.g. `audio_loopback` 227/227 | a DAC→ADC preset (`SPEAKER_BUS`/`HEADPHONE_BUS`) rails on codec 1. It lasts only until the next boot — every boot applies `SAMPLER` — so a reboot clears it; without one, `bench.py ready` puts MICS back |
| `TASKS` answers only `OK` | no table on CDC | it prints CPU-time stats to the **STM console**, and never stack sizes |
| `ble_midi` reports ENV | "bleak is not installed" | host-side package missing — that is not "BLE off" |
| Capture says "recorded through PipeWire" | audio scores drift late in a long take | another client (e.g. a DAW) holds the sound card, so the take runs on PipeWire's clock (~0.1 s per 16 s); it is healthy, but measure silence at the start of the take, not the end |
| Board has no CDC and no window to OTA into | every verb "no reply", `usb_mode=unknown` | firmware panicking before USB is up; `bench.py recover` UART-reflashes through the STM bridge (DTR/RTS there reach EN and GPIO0) and rewrites `ota_data_initial` so boot stops picking the bad slot |
| A line comes back as an echo | the board repeats what it was sent | on purpose: the dispatcher waits for LF (a host may split one write into two USB packets; Web Serial does) and echoes every line it does not handle, ending the echo with CRLF so it never glues onto the next reply, so a mistyped or truncated command shows itself |
| `usb-mode set default` (or `bench.py ready`, `ota_flash.py`) times out at 45 s and the board shows "USB serial mode?" | the USB serial guard is on (the default): a host's SysEx request for the CDC profile waits for Allow on the encoder | `USB_GUARD 0` over CDC once (persisted) on a development board; a user's board keeps it on. The prompt cannot be pressed from the host — `ENC_*` over BLE/SysEx answers `ERR guard_prompt_active` while it is up |
| `udisksctl power-off` on the transfer-mode drive takes the whole board off USB | every verb "no reply", `usb_mode=unknown`, the STM bridge VCP still there | that call removes the USB *device*, and the CDC port goes with it. `crosspad-hil console --reset` reboots the ESP and it re-enumerates; eject with the UDisks2 `Drive.Eject` method instead (a SCSI START STOP UNIT — the board stays on USB). See [docs/subsystems/usb-transfer-mode.md](../subsystems/usb-transfer-mode.md) |
| Browser companion (Web Serial) says "The device has been lost" on connect | Chrome/Chromium opens the port, then the first read fails; hil talks to the same port fine | a tty keeps its termios across reopen; pyserial-style `VMIN=0` left behind makes `poll()` always readable and `read()` return 0 bytes, which Chromium maps to device-lost. `stty -F /dev/ttyACMx min 1` or replug; hil's `RawTty.close()` restores `VMIN=1`, `bench.py check` reports it. Same family: Chromium leaves `PARMRK` set, which doubles every 0xFF byte for a reader that does not clear it (an `fs_transfer` pull then comes back too long, with a garbage trailer); hil's `RawTty` clears it |

Two test-setup facts that are **not** firmware bugs: kit loads under play
are slow — `kit_churn` needs `--load-timeout 30`, which `bench.py test` adds
for you — and `back_churn` takes 13 min for six rounds, 27 for twelve, against
the release gate's 17 min budget. A card with a bad DAT3 contact runs on the
1-bit bus; the boot's mount line says which width it got:
`SD Card mounted successfully at /sdcard (4-bit, 40000 kHz)` (`USB_MSC` has
`width= freq=` too). A failed 4-bit mount is retried 4-bit at 20 MHz at boot,
then 1-bit -- a second 4-bit attempt more than a cure: the usual failure is the
SSR read at 400 kHz, before the clock is raised, so 20 MHz helps only a card
marginal at 40 (that attempt logs `sdmmc_sd` at W, so the failing step shows).
The hotplug probe makes the two quick attempts only, and once on 1-bit the
card stays there until a restart; `USB_MSC` says which.

## Hardware-in-the-loop Testing

Host tests cover only parts of the firmware (`main/tests/`, `tools/tests/`, the
submodules' own suites). The test suite for the firmware as a whole is
[**crosspad-hil**](https://github.com/CrossPad/crosspad-hil) (pip package: library
+ CLI + daemon) run against a real board; exit **0** passed, **1** firmware
failed, **2** setup problem (host, cable, board state). Install: `pip install -r tools/requirements-hil.txt`
(the `git+https` form; not on PyPI yet) or `pip install -e "../crosspad-hil[all]"` from
a checkout. User walkthrough: [Test on hardware](https://crosspad.app/docs/how-to/hil-testing/).

```bash
python3 tools/bench.py test smoke         # preferred: prepares the board first
crosspad-hil run smoke                    # after every flash
crosspad-hil run app_churn --rounds 10    # app lifecycle leaks/crashes
crosspad-hil run back_churn               # multi-level back gesture, pads played in the Sampler
crosspad-hil run kit_churn --rapid 0.4    # kit swaps while pads fire
crosspad-hil run fs_transfer              # CDC file transfer + framing
crosspad-hil run release_gate             # the whole chain, one report
python3 tools/hil_perf_song.py            # A Drums+Slice+audio, B Pitch+audio, C all four at once: built, played, render judged
python3 tools/hil_stress_song.py --rounds 10   # PERF_ALL played under kit swaps and song switches: heap, resets, leaks (--listen: speakers on)
python3 tools/hil_torture_song.py --actions 250 # every Arrange action at random (--seed): tracks, takes, chords, storms, blocks, FX, freeze/bounce, kit swaps, reopen
python3 tools/hil_track_capacity.py            # a song built track by track to 8, played with chords at each count: what is safe
python3 tools/hil_track_transitions.py --tracks 5   # kit loads, ARM, TRACKKIT, edits, PUNCH under a playing song: underruns, card time by task
```

Device side of the command set: one function per verb in `main/hil_verbs_<domain>.cpp`
(ui, twin, audio, kit, song, system, fw, midi), offered a line in the order of
`k_verbs` in `main/hil_control.cpp` -- the order is part of the protocol, verbs match
by prefix. A new verb is a function in its domain's file, a line in `main/hil_verbs.h`
and an entry in `k_verbs`; the
state of the verbs the simulator answers too comes from crosspad-core
`crosspad/control/`, their CDC text from `main/hil_text.cpp` (host goldens in
`main/tests/`); host side: `crosspad_hil/verbs.py` + `knowledge/cdc.yaml`. No finger needed:
`TOUCH_DRAG x1 y1 x2 y2 ms [rest_ms] [hold_ms]` / `TOUCH_TAP x y [hold_ms]` play a synthetic
contact through the panel's read path (`main/touch_trace.cpp`; the indev is in
LVGL's event mode, so a timer reads it while one plays; `hold_ms` is a long press
before the drag), `TOAST text` shows a toast (wrap, time on screen), and `TOPBAR` says what
the top row, the drawer, the encoder and the last swipe did. `UI_TREE` lists every
visible widget with its text, rectangle and `hit` (a finger on its middle reaches
it, not something drawn over it): crosspad-hil `ui.py` finds a control by what it
says and taps it, so a scenario presses the buttons a player presses
(`studio_session`). Multi-line replies carry a
`NAME:` prefix per row — the reader drops unprefixed rows.

Traps: control is the CrossPad CDC (`0x303A`), console
is the STM bridge VCP (`0x0483:5740`) and **opening it with DTR/RTS asserted
reboots the ESP**; `PAD_PRESS` answers `OK` like `KIT_LOAD` does, so ask a state
verb instead; no CDC at all in the USB-audio profile; `ENC_GROUP` before driving
the UI; nothing that blocks belongs on the prio-1 drain loop (`CDC_STATS
maxcmd_us/maxgap_us` tells which wait made a reply late — and when both are
small yet nothing answers, the loop is starved of CPU by something at a higher
priority on core 0: `LVGL_STATS render_us` first); `app_queue` drops are
counted, not logged per drop. Pad storms come from the STM (`STM_VPAD`), not
`PAD_PRESS`; a reboot leaves a trace in `RESET_LOG` / `COREDUMP_INFO`. The
RT mixer runs in every USB profile (host playback and UAC capture go through
it); only `AUDIO_TASKS 0` / SysEx `1D 06 00` parks it -- silence both ways --
and the next USB mode switch resumes it.
All of it, with the measurements: [docs/subsystems/hil-testing.md](../subsystems/hil-testing.md).
