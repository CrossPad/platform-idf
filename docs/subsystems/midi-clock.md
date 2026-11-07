# MIDI Clock

CrossPad keeps one MIDI clock for everything plugged into it. Design:
[../design/2026-09-23-midi-clock-hub-design.md](../design/2026-09-23-midi-clock-hub-design.md).

## What it does

- **Follows the USB host.** 0xF8 at 24 PPQN from the DAW sets the tempo: the
  least-squares slope over the last four beats (a one-beat mean is up to 0.6 %
  off at 174 BPM under ±1 ms of USB jitter; the slope stays under 0.05 %). A gap
  over 250 ms between ticks starts the window over, so stop → new tempo → play
  locks at once.
- **Transport drives the click.** 0xFA starts the metronome in phase, 0xFB
  resumes it, 0xFC stops it.
- **Hands back after 1 s** without anything from the host (tick or transport),
  keeping the last host tempo; from then on the device generates the clock
  itself from the metronome's BPM. After a Start the host keeps the clock up to
  8 s before its first tick (count-in). A Start clears the tempo window, and the
  250 ms restart gap is measured between ticks only.
- **The device's own click** (Sampler/sequencer start and stop) reaches the clock
  through `MetronomeNode`'s transport hook: FA and tick 0 go out on the click's
  first beat and the generator counts from there.
- **Generator.** One `esp_timer`: tick *n* due at `t0 + n·period`, never
  relative to the previous tick, so timer latency never becomes drift; a stall
  longer than a period restarts the count instead of bursting. Continuous while
  internal; the device's own click start/stop sends 0xFA/0xFC.
- **Fan-out.** Every tick and transport byte, forwarded or generated, goes to
  BLE MIDI (only with a peer; every BLE message is stamped when queued, so
  timestamps never run backwards inside a packet) and to the STM over UART
  through `stm32_uart_send()`, which stays off the wire while the STM updater
  owns it. Never back to the USB host (both transports keep thru off).
- **UI.** The host tempo is written to the metronome directly, not through the
  sequencer, so it never dirties `sequences.json` or the kit. The Sampler's BPM
  field turns blue and snaps back from manual edits while the host drives;
  the clock repeats the host state once a second so a panel opened mid-song is
  current.

## Numbers (board r20, 23.09.2026)

| Measurement | Value |
|---|---|
| Host 90 / 120 / 174 BPM → board | 90.00 / 120.00–120.01 / 174.00–174.01 |
| Hand-back after the host's last byte | 1.00–1.40 s bracket (each status read is ~0.2 s) |
| Generator lateness, steady (launcher, Sampler open) | 17–27 µs |
| Generator lateness, launcher rebuild / app churn | 0.65 ms / 1.2 ms once |
| Audio | `rt_glitch` PASS, smoke PASS with the clock running |

## Interfaces

- Core: `crosspad/midi/MidiClock.hpp` (portable, Catch2 `[midiclock]`).
- ESP: `main/midi_clock.cpp` (`midi_clock_init()` after `bsp_stm32_init()` — earlier
  the generator wrote to a UART with no driver), handlers in `main/midiCallbacks.cpp`.
- CDC: `CLOCK_STATUS [RESET]` → `CLOCK: src=host|int bpm running ticks_in
  ticks_out last_gap_us max_jitter_us max_late_us`.
- HIL: `crosspad-hil run midi_clock [--bpms 90,120,174 --beats 16]`.

## Open

- The host tempo is written to the metronome, not through the sequencer's
  rescale: a loop take recorded while a DAW drives can land off-grid and the
  host tempo can reach `sequences.json` on the next save (sequencer parked).
- The click follows the host tempo within 0.02 BPM but is not phase-locked to
  the host's beats: ~0.08 beat of drift per 4 minutes at 90 BPM.

## Open (STM firmware)

- STM `midi_router`: the ESP's stream reaches TRS MIDI OUT since STM 1.14
  (`ESP2Midi`, [trs-midi.md](trs-midi.md)), the clock with it; not yet the
  STM's USB MIDI port (`ESP2USB`), and realtime bytes do not yet skip the TX
  queue.
- Forwarded-clock jitter as the host sees it (STM USB port) — measurable once the
  STM route exists.
- With the clock on the STM's USB port the host receives its own clock on a
  second port: a DAW syncing to that port closes a loop. To be settled before
  that route ships.
