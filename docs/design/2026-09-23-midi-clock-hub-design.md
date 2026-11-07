# MIDI Clock: follower, generator and hub output

Date: 2026-09-23. Status: design approved in conversation (section 1), spec for review.
Essential E3 of the sampler firmware kit, MIDI Clock half. Tap tempo is a
later, separate piece that plugs in as a third tempo source.

## Goal

CrossPad keeps one MIDI clock for everything plugged into it. While a DAW sends MIDI
Clock over USB, CrossPad follows its tempo and transport and passes the clock on to the
devices behind it. When the DAW is silent, CrossPad generates the clock itself from its
own BPM, so a synth on the TRS jack follows CrossPad with no computer attached.

"Good clock" is the requirement: CrossPad is a hub, and the devices behind it are only as
steady as what it forwards.

## Decisions (from the conversation)

| Question | Decision |
|---|---|
| What the host clock drives besides tempo | Start/Stop/Continue drive the metronome (click), in phase with the host |
| Manual BPM vs host clock | Host wins while 0xF8 keeps arriving; 1 s without a tick hands control back to the manual BPM, keeping the last host tempo |
| Where the clock goes | BLE MIDI, the STM's USB MIDI port, the TRS MIDI OUT jack |
| Where it comes from | The DAW when it plays, CrossPad's own BPM when it does not |
| Back to the DAW | Never — the host's clock is not echoed to the host (no loop) |

## Non-goals

- Sequencer transport sync (the sequencer is parked; the click is the only transport).
- Song Position Pointer (0xF2), MTC, Ableton Link.
- Clock input from BLE or TRS MIDI IN (USB from the host only, for now).
- Tap tempo (next piece).

## Architecture

Three layers, each built and tested on its own.

### 1. crosspad-core: `crosspad/midi/MidiClock.hpp` (portable)

One class, no platform calls; time comes in as microseconds.

- **Follower.** `tick(now_us)` for every 0xF8, `start(now_us)`, `cont(now_us)`,
  `stop(now_us)`. BPM from the mean interval over the last 24 ticks (one quarter note),
  `bpm = 60e6 / (24 * mean_us)`. A tempo change is published when it moves by more than
  0.05 BPM, so a steady host does not post an event per tick.
- **Source arbitration.** `poll(now_us)` releases the host source after 1 s without a
  tick; the BPM stays at the last host value.
- **Generator.** `nextTickDue(now_us)` for the internal source: tick *n* is due at
  `t0 + n * 60e6 / (24 * bpm)` from the moment the generator started, never from the
  previous tick, so timer latency does not accumulate into drift. A BPM change restarts the
  count from the current tick.
- **State.** `source()` (host / internal), `bpm()`, `running()`, tick counters.
- Tests (Catch2, `components/crosspad-core/tests/test_MidiClock.cpp`): mean over 24 ticks
  at 90/120/174 BPM with ±1 ms USB jitter on the input, the 1 s release, start/stop/
  continue state, generator drift < 1 tick over 10 minutes at 174 BPM, the 0.05 BPM
  publish threshold.

### 2. ESP (platform-idf)

- `main/midiCallbacks.cpp`: `USB_MIDI.setHandleClock/Start/Continue/Stop` feed the
  follower with `esp_timer_get_time()` taken in the handler.
- **Generator task.** One `esp_timer` one-shot re-armed to `nextTickDue()` — microsecond
  resolution, not the audio block (1.45 ms granularity would be the jitter).
  With the internal source it ticks continuously, so the devices behind CrossPad hold its
  tempo even while nothing plays; starting the click sends 0xFA, stopping it 0xFC.
- **Fan-out.** Every tick and every Start/Continue/Stop, whatever the source (forwarded
  from the host or generated), goes out as a single realtime byte:
  - BLE MIDI (`EspBleMidi`) when a peer is connected — BLE MIDI packets carry a 13-bit
    millisecond timestamp, set from the tick time, so a receiver that honours it undoes
    the connection-interval jitter;
  - the STM over the ESP↔STM UART, pushed ahead of queued traffic (MIDI allows a
    realtime byte inside any message);
  - never back to the USB host.
- **Click.** `MetronomeNode`: host Start → `start()` (phase from 0xFA), Continue → running
  without reset, Stop → `stop()`; tempo from `MidiClock::bpm()` for both sources.
- **UI.** `TempoChanged` on the event bus with a new source value; the Sampler's BPM field
  shows the host tempo and does not mark the kit tempo dirty (`seqBpmSyncing` path), with an
  "EXT" tag while the host drives. Manual edits while the host drives are ignored.
- **CDC verb** `CLOCK_STATUS` → `CLOCK: src=host|int bpm=… running=0|1 ticks_in=… ticks_out=…
  last_gap_us=… max_jitter_us=…`, in `main/hil_control.cpp`.

### 3. STM (CrossPad_STM32_r20)

- `midi_router`: bytes from the ESP go to USB MIDI OUT and to TRS MIDI OUT per the existing
  `ESP2USB` / `ESP2Midi` routing bits (today the ESP stream reaches only `esp_link`).
  Realtime bytes (0xF8–0xFF) skip the TX rings' queue: written to the UART data register /
  USB packet ahead of pending bytes.
- **TRS MIDI OUT.** Board r20 (PICO_drumpad PCB, 10.07): `MIDI_OUT_A` = PA4 (pin 21),
  `MIDI_OUT_B` = PA5 (pin 22), through R84 10 Ω / R85 33 Ω to tip and ring; `MIDI_IN` = PD6
  (USART2_RX). One line carries the data (current sink), the other sits high (source):
  Type A and Type B swap which is which.
  - Preferred: USART6 on PA4/PA5 (AF3) at 31250 baud, `SWAP` in `CR2` to move TX between
    the two pins, the other pin as GPIO high. **To verify in the G0B1 datasheet before
    coding** that AF3 on PA4/PA5 is USART6_TX/RX.
  - Fallback if it is not: software UART, TIM + DMA writing `BSRR` at 32 µs per bit, on
    whichever pin the type selects.
  - Setting `midiOutType` A/B (default A) in `Stm32Settings`, pushed like the other STM
    config registers (new `REG_CTL_MIDI_OUT_TYPE`), in Settings → MIDI.
- STM firmware bump (1.8), `STM_PROTO` minor if the register map grows, bundled
  `stm32_fw.bin` updated through the in-field update path.

## Verification

- Core: the Catch2 tests above.
- `crosspad-hil`:
  - `CLOCK_STATUS` in `knowledge/cdc.yaml` + `verbs.py`.
  - Scenario `midi_clock`:
    1. Host sends 0xF8 at 90 / 120 / 174 BPM on the ESP's USB MIDI port (timestamps from
       a dedicated sender thread) with FA/FC around each run; reads the forwarded ticks on
       the STM's USB MIDI port (`--port stm`). Pass: `CLOCK_STATUS` BPM within 0.1 % of
       the sent tempo; forwarded tick intervals: p99 added jitter ≤ 1 ms over the sent
       intervals (1 ms is the USB frame, the resolution of the measurement); no lost or
       doubled ticks; click running between FA and FC.
    2. Host stops sending: `src` goes to `int` within 1.0–1.2 s, the STM port keeps
       receiving ticks at the last BPM, mean error < 0.1 %, p99 jitter ≤ 1 ms.
  - TRS OUT: loop cable OUT → IN (hands, once) and the same measurement through
    `MIDI_IN`; Type A and B each checked with a known-good receiver.
- Release gate: `midi_clock` joins the gate once it passes three times in a row.

## Order of work

1. Core `MidiClock` + tests.
2. ESP: follower, click, UI, `CLOCK_STATUS`, generator, BLE and UART fan-out.
3. hil: verb + `midi_clock` (USB → STM USB leg needs step 4).
4. STM: ESP → USB MIDI OUT route, realtime ahead of the queue.
5. STM: TRS OUT (pin check, USART6 or soft UART, Type A/B setting).

Steps 1–2 are useful alone (follow the DAW, click in time, BLE devices clocked).

## Risks

- USB MIDI input jitter (host scheduling + 1 ms frames) sets the floor for forwarded
  clock; forwarding is immediate, not re-generated, so the hub adds latency but never
  drifts from the host. If receivers show the host's jitter, a follow-up can re-generate
  the forwarded clock from the follower's smoothed tempo (PLL) — not in this piece.
- The CDC drain loop is priority 1; the USB MIDI task and the timer callback must not wait
  behind it. The fan-out writes a single byte and returns.
- BLE timestamps only help receivers that honour them.
- TRS OUT depends on the pin alternate-function check; the soft-UART fallback costs a timer
  and a DMA channel on the STM.
