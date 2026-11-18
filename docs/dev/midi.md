# MIDI clock and the TRS jacks

Contributor notes: the clock, TRS MIDI in and out. The short version of what a change in this area
must not break; the pages it links to have the detail.

## MIDI Clock

`main/midi_clock.cpp` over core's `MidiClock`: follows a USB host's 24-PPQN clock
(tempo from a 4-beat fit, click transport from FA/FB/FC), hands back to the
metronome's tempo after 1 s without host traffic and then generates the clock
itself (`esp_timer`, drift-free). Every tick and transport byte goes to BLE MIDI
(with a peer) and the STM, never back to the host. The Sequencer app's BPM field turns
blue and refuses edits while the host drives. `CLOCK_STATUS` over CDC;
`crosspad-hil run midi_clock`. The clock reaches TRS MIDI OUT (STM 1.14), not yet
the STM's USB port. Details:
[docs/subsystems/midi-clock.md](../subsystems/midi-clock.md).

## TRS MIDI (IN and OUT jacks)

The STM owns both: IN on USART2 (PD6), OUT on USART6 (STM 1.14), Type A
(data on PA4) or B (`CR2.SWAP`, data on PA5), Settings → MIDI → MIDI Out Type.
`REG_CTL_ROUTING_0/1` are core's `STMRoutingSettings` bytes; MIDI IN → USB/ESP/
thru, USB → ESP/OUT, ESP → OUT. Loop test (straight cable OUT → IN):
`crosspad-hil run midi_trs_loop` -- both types, throughput, TX baud skew and a
bit-flip injector per segment (`MIDI_TRS`, `MIDI_LOOP` over CDC). Measured:
1027 of the line's 1042 msg/s, clean within ±5 % skew, a bad byte costs its
own message. Details: [docs/subsystems/trs-midi.md](../subsystems/trs-midi.md).
