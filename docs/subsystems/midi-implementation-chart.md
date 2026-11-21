<!--
  CrossPad — MIDI Implementation Chart.
  The shape of this page is the one hardware makers have printed on the back of
  their manuals since the mid-1980s ("MIDI Implementation Chart", Roland style:
  what the box sends, what it recognizes, on which channels, with what default).
  It is here for the same reason: someone wiring a CrossPad into a DAW or to a
  synth needs a table, not a story. The story is
  [midi-input.md](midi-input.md); the measurements below were taken on a real
  v2 board (PCB r20, firmware v1.1.3-34-g697ac61-dirty with the host control
  surface) on 2026-11-18, over the ESP's own USB MIDI in the default profile.
  When a message is *not* implemented, say so here: an unlisted controller is a
  guess, and guesses are what people phone about.
-->

# MIDI Implementation Chart

| | |
|---|---|
| Model | CrossPad (ESP32-S3, PCB r20 = board revision v2) |
| Firmware | v1.1.3 and later (this chart: v1.1.3-34-g697ac61 + the host control surface) |
| Date | 2026-11-18 |
| Basic channel | pad/transmit channel: Settings → MIDI, `stm.midiChannel` (default 1); receive: OMNI (all sixteen are heard on USB and on the bridge) |
| Default channel | 1 for everything the board sends, unless the routing or the app says otherwise |
| Mode | 3 (OMNI ON, POLY) — no MTC, no MMC, no song messages |
| Note range | 36–51 (`pad 0..15`, shifted by `stm.noteOffset`); the DAW Control app sends `base_note + pad` instead |
| Polyphony | 16 pads; the sampler voices per kit (drums 12, pitch 4, native engine's own cap, see [sampler-engine.md](sampler-engine.md)) |
| MIDI implementation | `main/midiCallbacks.cpp` (callbacks), `main/midi_kit_pc.cpp` (Program Change → kit), `main/host_midi.cpp` (Control Change → levels, host pad LEDs), `main/midi_clock.cpp` (clock), `main/audio_route_control.cpp` + `crosspad-core` `SysExProtocol.h` (the CrossPad frames) |

**Three ways in, one behaviour:** USB MIDI (both USB profiles), the TRS jacks and
the USB port of the STM32 bridge (relayed as raw MIDI, [trs-midi.md](trs-midi.md)),
and BLE MIDI ([ble-midi.md](ble-midi.md)). Where a transport cannot carry
something, that is said in the Remarks column — the chart describes the board,
not one cable.

## 1 · Note messages

| Message | Sent | Recognized | Remarks |
|---|---|---|---|
| Note on (9n) | **yes**, vel ≠ 0 | **yes** | Pads send note `36 + pad + noteOffset`, velocity from the hit's force (pressure-sensing: the harder the hit, the higher). A host's note is the pad it names through the same map and reaches the engine (`stm32NoteToPad`); with DAW Control running, notes go to the app instead |
| Note off (8n) | **yes**, vel = 0 (pads), and 0x40 for a release the app reports | **yes** | A pad's release also closes the note; a held pad released by an app switch sends note-off first |
| Key aftertouch / channel pressure (An / Dn) | **yes** when Pad MIDI Output → aftertouch is on; per-note aftertouch as poly aftertouch | **yes** (poly → pad pressure, `notifyPadPressure`) | Pressure while a pad is held, 0–127. Keypad feedback `AFTERTOUCH` makes the pad's LED follow it |
| Pitch bend change (En) | no (no bend source on the board) | **yes** (forwarded per routing) | The sampler does not bend on it; a pitched kit's bends come from its own pads/inst |

## 2 · Program change

| Message | Sent | Recognized | Remarks |
|---|---|---|---|
| Program change (Cn) | **yes** — announces the kit the board is on (§6b) | **yes** — selects the kit | Modes `off` / `flat` (default) / `group`, and a channel filter, over CDC `KIT_PC`; persisted in NVS `cfg_midi/pc_kit`, `pc_ch`. `flat`: program = the position in the board's own kit listing, bank MSB (CC 0) the next block of 128. `group`: bank MSB = 0 PERFORMANCE, 1 DRUMS, 2 SLICE, 3 PITCH, 4 USER; program = the place inside that kind. Read the exact pairs with `KIT_PC MAP`. Refused with a reason (log + `KITPC:` counters) when nothing is named, when the card scan is not through, or when the kit is not loadable. `KIT_PC ECHO on|off` (on, not persisted) switches what the board sends back; `announced=`/`ann=` count it. **Not over BLE yet** (BLE parses note on/off and CC only) |
| Bank select (Bn cc0 / Bn cc32) | **CC 0, with a kit announcement** (§6b); CC 32 never | **yes** as the kit bank; also forwarded as a normal CC | Latched per channel: it applies to the next Program Change on that channel. MSB picks the block (flat) or the kind (group); LSB is carried and reported, not used for choosing |

## 3 · Control changes

| CC | Sent | Recognized | Default value | Affected parameter / remarks |
|---|---|---|---|---|
| 0 Bank MSB | **yes** — with a kit announcement (§6b) | **yes** | 0 | Kit bank for the next Program Change (§2), then stays for that channel |
| 1 | **yes** | no (not mapped) | — | The rotary encoder, detent 0..15 — the board emits it, it does not listen (CLAUDE.md, DAW Control) |
| 32 Bank LSB | no — an announcement never sends it | **yes** (reported) | 0 | Carried for the kit bank's sake; a 7-bit pair is all Program Change can ask for |
| 48–63 (`ccPanBase` .. +15) | **yes** (the pads' left/right stream) | **yes** | 64 = centre | `stm.ccPanBase` default 48, one per pad: pad pan for the active pad logic. These are **consumed** as pad state, not treated as a controller — that band belongs to the pads ([midiCallbacks.cpp:624-633](../../main/midiCallbacks.cpp)) |
| 64 | **yes** | no (not mapped) | — | The encoder's switch (sustain by convention); global to the board |
| 7 (channel volume) | no | **yes** where mapped | channel 16 → the built-in speakers, in the hardware (0..100 % after the DAC) | `MIDI_CC SET <ch> 7 channel "<name>" \| output <i> \| hardware <i> \| pad <i>` points a CC at a mixer channel, a mixer bus, a codec output, or one pad's live level. The map is in RAM (which mixer channels exist depends on the kit and the song loaded: [sampler-engine.md](sampler-engine.md)), `MIDI_CC MAP` shows it with the board's current value, `MIDI_CC RESET` returns to the two shipped rules. A 7-bit value becomes a level on a hardware taper (`levelFrom7`, squared — half a fader is not half as loud), pinned by `main/tests/test_host_midi.cpp` |
| 10 (pan) | no | **yes** where mapped | channel 1 → the sampler's balance | Same mechanism with `PAN` asked for at the end of the line: `MIDI_CC SET 1 10 channel "Sampler" PAN`; −1..+1 with unity at the centre |
| 11 (expression) | no | no (forwarded only) | — | Not mapped by default; free for `MIDI_CC SET` |
| anything else | no | forwarded per the routing flags | — | Nothing local; `MIDI_CC STATUS` counts what was unmapped, so "did it reach?" is a read, not a guess |

No RPN, no NRPN, no registered parameters — nothing on this board is a
parameter number space yet.

## 4 · System exclusive (the CrossPad frames, manufacturer 0x7D)

The frames are the board's own control plane; the catalog of record is
crosspad-hil's `knowledge/sysex.yaml`, and `crosspad-core/include/crosspad/protocol/SysExProtocol.h`
is the code's copy of the numbers. Foreign-manufacturer SysEx is ignored unless
`SYSEX_ECHO 1` (then echoed, which is how the FL Studio bridge rides it).

| Frame | Direction | What it does |
|---|---|---|
| `F0 7D 1D <sub> … F7` (audio route) | host → board, replies on the same id | subs 0x01 ADC input, 0x02 mic source, 0x03 DAC output, 0x04 volume, 0x05 mute, 0x06 task override (HIL), 0x07/0x08 pad press/release (HIL), 0x09 echo, 0x0A preview pad, 0x0B kit load, 0x0C bounce scene/track, **0x0D pad LED (`pad r g b`)**, **0x0E pad LED grid (16 × `r g b`)**, **0x0F give the pads back**, 0x10 query, **0x11 address block write, 0x12 pad colour direct, 0x13 palette load, 0x14 give a range back if it is still ours, 0x15 status reply** ([surface-sysex.md](surface-sysex.md)). Colours are **7-bit** and doubled by the board, the DAW protocol's convention ([DawProtocol.cpp:89-91](../../components/crosspad-dawcontrol/src/DawProtocol.cpp)) |
| `F0 7D 1E <sub> … F7` (DAW Control) | host ↔ board | 0x01 hello, 0x02 `led`, 0x03 `led_all`, 0x04 text rows, 0x05 beat, 0x06 transport, 0x07 mode, 0x08 config, 0x09/0x0B pad state up, 0x0A ping, 0x7F bye. **Reaches the DAW Control app only** — the app owns the pads while it runs; that is what 0x1D 0x0D-0x0F is for, when some other app is running |
| `F0 7D 1F …` (companion) | host ↔ board | the verb set over MIDI/BLE for the profiles with no CDC: `0x01 <ASCII verb>`, replies chunked `0x02`/`0x03`, notify `0x04`. Allowlist in `main/companion_sysex.cpp`; `MIDI_CC` and `LED_HOST` are CDC-only for now |
| `F0 7D 15/1B/19/1C …` | host → board | power off, USB profile request (confirmed on the device unless `USB_GUARD 0`), DFU request, STM version reply |

## 5 · System real-time and common

| Message | Sent | Recognized | Remarks |
|---|---|---|---|
| Clock (F8) | **yes** (24 ppqn, when it is the master) | **yes** | The board follows a host's tempo (4-beat fit) and its transport, takes its own clock back after 1 s of silence, and forwards every tick to BLE and to the STM — never back to the host ([midi-clock.md](midi-clock.md)) |
| Start / Stop / Continue (FA / FC / FB) | yes (its own transport) | **yes** | Start and Stop move the board's playing; the Sequencer's BPM field locks while a host drives |
| Song position / Song select | no | **no** | Not implemented, and not on the roadmap in this shape — the board is not a song machine over MIDI |
| Active sensing (FE) | no | **no** | The library's handling is never enabled anywhere in the tree (`midi_Settings.h:79,85`), so a host sending FE does nothing and its absence is not a fault either |
| Tune request (F6), MMC (F7 subsystem), MTC | no | **no** | Not implemented |
| Reset (FF) | no | **no** | A reboot is `RESET` over CDC or the power button; a soft reboot is not a MIDI function |

## 6 · Pad LEDs a host can drive

This is the part a Launchpad-style host asks for, and the chart's promise is
that it behaves the way the chart says:

- One frame paints: 0x1D 0x0D one pad, 0x1D 0x0E all sixteen; over CDC the same
  plus `LED_HOST SET/ALL/GRID <96 hex>/ANIM chase|ring|off [ms]`.
- Or address-style, which is what a panel wants: 0x1D 0x13 loads a palette of 32
  colours once, then 0x1D 0x11 with section `0x01` writes sixteen palette indices in
  one 24-byte frame, and 0x1D 0x12 writes colour straight into the frame instead.
  0x1D 0x14 hands a range back **only where the board has not repainted it since**,
  and 0x1D 0x11 with section `0x00` says "this pad I am holding" per pad, so a
  painted mode view can stay while every other pad still falls under the lease
  timeout ([surface-sysex.md](surface-sysex.md), §"Giving it back").
- The first frame **takes a lease**: the board remembers what it was showing
  itself, including the running app's own pad colours.
- `0F` / `LED_HOST OFF` / `ANIM off` **gives it back**, and the board also takes
  it back **on its own after 5 s of host silence** -- the shipped default, changed
  by `LED_HOST LEASE <ms>`, `LEASE AUTO` (back to 5000) or `LEASE 0` (hold until
  told). "Silence" means nothing at all arrived on this surface: a paint, a grid,
  an animation step or a mapped Control Change each renew the clock, because what
  is being timed is whether a host is present.
- Frames are parked by whichever task received them and applied on the shared
  `bsp_worker` task, which is the task that already runs `pad_anim` — the strip
  is never written from the MIDI task, and audio does not pay for an animation
  (CLAUDE.md: "Pad LED frames go out from the `pad_anim` worker only").
- While a host holds the lease, the running app's own painting is what gets
  restored when it ends: the board does not silently fall back to global default
  colours.
- Levels the host drove are handed back the same way when the host says so
  (`MIDI_CC GIVE`) or when the map is cleared; `MIDI_CC STATUS` counts `held=` and
  `gives=`. Pad levels are the exception: no getter to snapshot, so nothing to
  hand back.

## 6b · What the board sends without being asked

| | |
|---|---|
| Encoder | CC 1 (detent 0..15) and CC 64 (the switch), as the board's own playing — the path every outgoing CC uses |
| Pads played at the board | notes and aftertouch, per the pad routing |
| **A level the board moved itself** | the mapped `(channel, CC)` again, with the value the board is at now — so a DAW's fader follows a person at the desk, or a handover. On by default, `MIDI_CC ECHO off` for a host that does not want it, `echoed=`/`echo=` in `MIDI_CC STATUS` as the record |
| **The kit the board is on** | Bank Select MSB + Program Change for it, in the mode's own ordering — the pair `KIT_PC MAP` shows for that kit, so the map and the announcement are one language. Announced once at boot (a board waking up says where it is), on by default, `KIT_PC ECHO off` to silence it, `announced=`/`ann=` in `KITPC:` as the record. The LSB is not sent: neither mode reads it |
| Where those bytes go | the board's pad-output routing (Settings → send2USB / send2STM / send2BLE), not one fixed port — a test listens on whichever door the routing uses |
| Deliberately silent | the host's own messages (re-baselined when applied), a freshly mapped rule (seeded), a kit the host reached by Program Change (seeded before the load is even asked for — and a TRS loop back into the board's own input is broken by the same "the already current kit costs no load" rule, not by a timer or an "ignore my own messages" table), a pad's live level (`IPadMix` has no getter), and everything that moved while echo was off (turning it back on seeds from where the desk is now) |

## 7 · What else a host can and cannot do

| Ask | Answer |
|---|---|
| Change the kit | Program Change (§2), SysEx 0x1D 0x0B, `KIT_LOAD` over CDC or companion — all of them the same loader the kit browser uses |
| Change levels | §3's map (mixer channel / bus / codec output / one pad's live level). Pad *mix* over MIDI is live only: it does not mark kit.json modified, unlike `PAD_MIX` ([audio-song.md](audio-song.md), Pad Mixer) |
| Paint pads | §6 |
| Press a pad | SysEx 0x1D 0x07/0x08 (the HIL path, same as a physical touch), or notes (§1) |
| Ask the board's state | `MIDI_CC`, `LEDHOST:`… over CDC; the companion verbs over MIDI/BLE; `MIX_LVL`, `OUTVOL`, `LED_STATE`, `KIT_STATUS`, `CLOCK_STATUS`, `SMPL_ENGINE`, `MEM` |
| Rename/delete a kit, edit samples, files, OTA | not over MIDI — those stay on CDC, and BLE/files/OTA are excluded by design ([ble-midi.md](ble-midi.md), [file-transfer.md](file-transfer.md)) |
| Local control off (`Bn 122`), omni/omni-monopoly switching (`Fc`-`Ff`) | **no** — there is no local-control switch; the pads' transmission is Settings and routing, and a DAW that assumes one will not find it here |

Related: [midi-input.md](midi-input.md) (what each message does and where, in
prose), [kit-swap.md](kit-swap.md) (what changing a kit while playing promises),
[midi-clock.md](midi-clock.md), [trs-midi.md](trs-midi.md),
[ble-midi.md](ble-midi.md), [audio-routing.md](audio-routing.md), and the design
note `docs/design/2026-11-18-midi-program-change-kits-design.md`.
