# TRS MIDI (the 3.5 mm IN and OUT jacks)

The STM owns both jacks. MIDI IN is USART2 RX on PD6; MIDI OUT (STM 1.14) is
USART6 on PA4/PA5, 31250 baud, 8N1. Code: `CrossPad_STM32_r2`
`app/src/midi_trs.c` (the jack), `app/src/midi_router.c` (who sends where);
register map 0x0012 (crosspad-core 2.24.0, `crosspad/stm32/reg_map.h`).

## Type A and Type B

Board r20 wires `MIDI_OUT_A` = PA4 (R84, 10 Ω) and `MIDI_OUT_B` = PA5 (R85,
33 Ω) to the jack. One line carries the data (current sink), the other sits
high (current source); the two types swap them. Type A puts USART6 TX on PA4
(AF3) and holds PA5 high as a GPIO; Type B sets `CR2.SWAP`, which moves TX to
PA5 (USART6_RX's pin, AF3), and holds PA4 high. Settings → MIDI → MIDI Out
Type (`Stm32Settings::midiOutType`, NVS `cfg_stm/midi_out`,
`REG_CTL_MIDI_OUT_TYPE`). Both pins stay in the ADC scan, so the STM sees
which one moves: `MIDI_TRS` reports the lowest reading of each since the last
`RESET` (`line_min_a/b`, ~305 on the data line, 4095 on the held one).

## Routing

`REG_CTL_ROUTING_0/1` carry crosspad-core's `STMRoutingSettings` bytes
(`packByte1()` / `packByte2()`); the STM decodes that layout (STM 1.14 and
later). Honoured: MIDI
IN → USB / ESP / OUT (thru), USB → ESP / OUT, ESP → OUT. Not wired: ESP → USB
(it would hand a DAW its own clock back on a second port), the USB and ESP
thru bits, the button bits. Without `ENABLED` (before the ESP has written it):
MIDI IN → USB and ESP, USB → ESP, nothing to OUT.

Three sources can feed OUT. `midi_trs_offer()` merges them a whole message at
a time (real-time bytes go out between any two bytes), puts the running
status back when another source spoke in between, and frees the jack from a
source that stops mid-message after 20 ms. The ESP's stream reaches OUT
without its heartbeat (FE) and without the STM's own commands (`F0 7D … F7`).
Messages for the ESP link and the host go into their rings whole, interrupts
off, status in front: the pads' notes and the STM's own frames are pushed from
interrupts and would otherwise land between a forwarded message's status and
data.

## Measuring it: the loop test

A straight stereo 3.5 mm cable from OUT to IN, STM 1.14, then
`crosspad-hil run midi_trs_loop`. The stream is numbered note-ons on channel 1
(note = seq % 32, velocity = 1 + seq / 32), sent by the host on the STM's USB
MIDI port or by the ESP (`MIDI_LOOP SEND`), and checked message by message on
the host (STM USB MIDI IN) and on the ESP (`MIDI_LOOP CAPTURE 1`). The STM
counts every segment (`MIDI_TRS`, `REG_DEBUG_MIDI`): bytes from the host,
queued and sent on OUT, received on IN with FE/NE/ORE, handed to the ESP and
to the host, taken from the ESP.

Bench hooks, reset to off with the STM:

| Verb | What |
|---|---|
| `MIDI_TRS TYPE A\|B` | the type, unsaved |
| `MIDI_TRS SKEW <n>` | TX baud off by n × 0.1 % (real bit errors at the receiver) |
| `MIDI_TRS FLIP <seg> <N>` | each bit through one segment flips with 1/N: 1 OUT, 2 IN, 3 to ESP, 4 to host, 5 from host, 6 from ESP |
| `MIDI_TRS ROUTE <r0> <r1>` / `ROUTE SAVED` | routing in RAM (Stm32Manager pushes it every 3 s) / back to the stored one |
| `MIDI_TRS RESET` | clear the counters |
| `MIDI_LOOP CAPTURE 0\|1`, `SEND <count> <rate>`, `STOP` | the ESP's end |

## Measured (2026-10-25, v2, straight cable)

- **Both types carry the loop.** Type A drives PA4, Type B PA5 (ADC minimum
  ~305 on the data line, the other at 4095), and MIDI IN receives either
  polarity: 300/300 each, and 300/300 again on A after B.
- **Throughput, no errors:** host → OUT → IN → host and → ESP, 0 lost at 250,
  500 and 900 msg/s; at 1200 msg/s offered, 1027 msg/s delivered (the line
  carries 1042 three-byte messages a second), still 0 lost — the host's USB
  OUT is held back (NAK) rather than dropped. ESP → OUT → IN: 0 lost at 500
  and 900 msg/s. Host round trip p50 1.3–1.5 ms, p99 ~4 ms below the line
  rate.
- **Baud skew:** clean from −5.0 % to +5.0 % (the receiver flags noise from
  ±4 %, framing errors from ±5 %, the data still right); −6 % loses 16 %,
  +6 % everything.
- **Injected bit errors**, any segment: 1/10⁵ costs 0–1 message in 1000,
  1/10³ 2–3 %, 1/10² ~20 % — what a 24-bit message corrupted with that
  probability predicts, so a bad byte costs its own message and no more. An
  error on the way to the host leaves the ESP untouched and the other way
  round. After every error leg the next clean run is 1000/1000 at both ends.

What the test guards: a byte corrupted into F0 must not silence the STM's USB
MIDI IN to the host (or the ESP's parser, the vendored Arduino MIDI library)
until an F7 comes; FE/NE/ORE on MIDI IN are read before HAL's one-byte re-arm
resets the error code; no forwarded message is lost on either direction of
the ESP link (no PAD_TOUCH frame between status and data, the 8-byte RX FIFO
as slack for the DMA re-arm).

## Open

- ESP → USB on the STM's port (see Routing).
- The pads' channel: the ESP parses the STM's stream on channel 1 only
  (`STM32MIDI.begin()`); a `midiChannel` other than 1 is not heard.
