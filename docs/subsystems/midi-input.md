# Host MIDI: what every message does to the board

Source of truth for this subsystem. Written 2026-11-18 with the Program Change
work (`docs/design/2026-11-18-midi-program-change-kits-design.md`); before this
page the only user-facing sentence about incoming MIDI was one line of the
manual — "CrossPad odbiera nuty i zegar" (`docs/manual/chapters/17-midi.html:7`)
— and nothing in the repo listed what the other messages do. That is the hole
this page starts to close: it is a chart of what arrives, what the board does
with it, and what it does not do.

## The three ways in, and where they meet

| In | How it reaches the ESP | Parsed by |
|---|---|---|
| USB MIDI, host straight into the ESP | TinyUSB, `0x303A:3456` (default profile, CDC + MIDI) or `0x303A:3457` (MIDI + UAC2, no CDC) — the MIDI interface is in both (`main/usb_descriptors.c:66`, `main/usb_descriptors_audio.c:57-58`) | `USB_MIDI` in `uartLoop` (`main/midiCallbacks.cpp:692-699`), started `MIDI_CHANNEL_OMNI` (`:579`) so all 16 channels are heard |
| USB MIDI and TRS MIDI IN through the STM32 | the STM routes bytes to the ESP over the UART link as **raw MIDI**, not wrapped (`components/stm32_hal/stm32_uart.cpp:47`, `components/crosspad-platform-idf/EspStm32MidiTransport.cpp:145`); the routing bits are `REG_CTL_ROUTING_0/1` and the honored paths are MIDI IN → USB/ESP/OUT, USB → ESP/OUT, ESP → OUT (`docs/subsystems/trs-midi.md:24-25`) | `STM32MIDI` in the same task and loop |
| BLE MIDI | the board's own NimBLE parser, running status included (`EspBleMidi.cpp:655`), and it only dispatches what it knows — see the table | `EspBleMidi::dispatchParsed` (`:677-690`) |

Both USB and STM parsing run **in one task** (`uartLoop`), which is also the task
that plays bench-injected pad hits; the rule that task must never block is why a
kit request from MIDI is parked and applied elsewhere
(`main/hil_control.cpp:459-464`, `main/include/lv_async_safe.h:24-29`).
Everything that is not the CrossPad protocol ends in
`crosspad::getMidiInputHandler()` (`components/crosspad-core/src/midi/MidiInputHandler.cpp`).

## What the board does with each message

| Message | What the board does | Where |
|---|---|---|
| Note on / note off (0x90/0x80) | a host's note is the pad it maps to (`stm32NoteToPad`, `noteOffset`) and reaches the sampler engine through the event bus; with DAW Control running the notes go to the app instead | `main/midiCallbacks.cpp:595-601`, `:326-386`, `MidiInputHandler.cpp:22-45` |
| Control change (0xB0) | CC in the pad-pan band (`ccPanBase … +15`, default base 48: `crosspad-core/include/crosspad/config/Configuration_adv.h:31`) is taken as the board's own pad pan stream coming back and is consumed there; every other CC is forwarded per the routing flags | `midiCallbacks.cpp:622-635` |
| **Bank Select MSB / LSB (CC 0 / CC 32)** | latched per channel as the bank for the next Program Change on that channel; also forwarded as an ordinary CC | `main/midi_kit_pc.cpp` (`kit_change_control_change`) |
| Channel aftertouch / poly aftertouch (0xD0 / 0xA0) | pad pressure (`notifyPadPressure`) and forwarding | `midiCallbacks.cpp:636-643`, `:659-665` |
| Pitch bend (0xE0) | forwarded; the sampler does not bend on it | `midiCallbacks.cpp:644-646`, `:670-672` |
| **Program Change (0xC0-0xCF)** | **selects the kit** — `flat`: the position in the board's kit listing (bank MSB = the next block of 128); `group`: bank MSB = kind, program = the place inside that kind; `off`: nothing beyond the forwarding it always did. Refusals are logged with a reason and counted. See below | `main/midiCallbacks.cpp:649`, `:677` → `main/midi_kit_pc.cpp` → the same loader as the kit browser |
| Clock / Start / Stop / Continue / Tick (0xF8, 0xFA, 0xFC, 0xF9, 0xF1) | the board follows a host's tempo (a 4-beat fit) and transport, hands back to its own metronome after 1 s of silence, and re-times its own clock; every tick also goes to BLE and to the STM, never back to the host | `main/midiCallbacks.cpp:581-584`, `main/midi_clock.cpp`, `docs/subsystems/midi-clock.md` |
| SysEx `F0 7D …` (manufacturer 0x7D) | the CrossPad control plane: pad/encoder/kit/audio/DAW/companion commands (`crosspad-core/include/crosspad/protocol/SysExProtocol.h:14-32`); foreign-manufacturer frames are echoed only when `SYSEX_ECHO 1` | `main/midiCallbacks.cpp:606-621`, `main/audio_route_control.cpp`, `main/companion_sysex.cpp` |
| System real-time other than clock (e.g. 0xF7 end, 0xFE sensing) | Active Sensing is never wired: no `setHandleActiveSensing` call exists in the tree, and the library default leaves it off | `components/arduino_midi_library/src/midi_Settings.h:79,85` |
| Song select / position, MMC, MTC | nothing: no callback is registered for them and the library does not deliver them to this code | (absence is the fact to know) |

What the board **sends** is a different switchboard — pad notes, aftertouch, the
pad-pan CC stream, the encoder's CC 1 and CC 64, the clock it generates, BLE and
TRS copies — gated by `keypad.send2STM/send2USB/send2BLE`
(`MidiInputHandler.cpp:92-163`, `docs/subsystems/ble-midi.md`), and described in
`docs/subsystems/trs-midi.md` for the jacks.

## Program Change and Bank Select

The decision is one pure header, `main/include/kit_pc.h`, pinned on the host by
`main/tests/test_kit_pc.cpp`; the request path reuses the board's existing one:

```
midi callback  ->  park {channel, msb, lsb, program} in one atomic word,
                   post ONE lv_async_call_locked            (never blocks, allocates nothing)
LVGL apply     ->  read the listing, resolve, guard (card scan done, id the manager holds),
                   kit_load_task_start(false, kitId)        (the loader the browser uses too)
```

So a kit asked for over MIDI is the same kit change the manual already promises:
loaded pad by pad in the background, a sounding pad finishes its sample, a hit on
a pad being rebuilt plays right after the reload (`docs/subsystems/kit-swap.md:29-35`,
`:196-202`, `docs/manual/chapters/03a-technologia.html:60`). It does not stop an
engine, does not take `LoaderLock` or `EngineArenaOwner` on anyone's thread, and
never parses kit JSON off the card on the calling thread.

A Program Change for the kit that is already current is **not** a load: the
loader is not asked, the counters still say a message arrived.

### The switch and the map, over CDC

`main/midi_kit_pc.cpp` keeps its setting in `cfg_midi/pc_kit` and `cfg_midi/pc_ch`
(the same shape as the USB host placement in `main/usb_host_profile.cpp:11-12`),
so a reboot or a settings export carries it.

```
KIT_PC                     -> KITPC: mode=flat ch=0 seen=12 loaded=7 refused=3 ignored=2
                                cur=53 last_kit=44 last=ok echo=1 announced=53 ann=1
                                kits=121 reach=121
KIT_PC ON | OFF            -> OK                     (ON selects mode flat)
KIT_PC MODE off|flat|group -> OK
KIT_PC CH 0..16            -> OK                     (0 = any channel; 1..16 as a DAW numbers them)
KIT_PC ECHO on|off         -> OK                     (whether the board tells a host which kit it is on)
KIT_PC MAP [first] [count] -> KITPCMAP: msb=0 pc=0 id=0 kit "POP_RETRO_OUMPH"
                              KITPCMAP: msb=0 pc=1 id=1 kit "LOOP_006"
                              …                       (one line per row, as KIT_LIST answers)
                              KITPCMAP: end first=0 total=121 shown=12
```

(measured on a v2 board with a 121-kit card, 2026-11-18; the counters and
`last=` exist because "I sent a program change and nothing happened" has to be
answerable from the board: `refused` counts a message that named nothing or hit a
guard, `ignored` counts one the mode or the channel filter did not accept.)

A row of the map prints the exact pair to send, in the ordering the mode
resolves: in `flat` row *n* is `msb = n/128`, `pc = n%128`; in `group` the rows
run kind by kind and `pc` restarts inside each kind. That is what makes the map
worth reading instead of guessing: the host sends what it read, and
`resolve()` walks the same ordering.

## Telling a host which kit the board is on

`KIT_PC ECHO on|off`, on by default and not persisted -- the Control Change map's
shape rather than the mode's, because it is a property of the session a host is in.
A kit that changed *at the board* (a person in the kit browser or the Sampler,
`KIT_LOAD` over CDC, a song that names its kit) is announced with the very Bank
Select MSB + Program Change a host would have sent for it, in the mode's own
ordering: `positionOf` walks it the same way `resolve()` and `pairAt` do, pinned
across both modes in `main/tests/test_kit_pc.cpp`, so the map the host reads and
the announcement the board sends cannot name different programs for one kit. A DAW
with a kit page open therefore follows what the fingers did, and the boot kit is
announced once, which is what a control surface does when it wakes up.

Three things make that safe rather than merely chatty:

- a kit the host reached *by* Program Change is **seeded as announced** before the
  load is even requested, so the board does not answer a host with what it was just
  told. The load is asynchronous: without the seed the board would report the old
  kit, then the new one, twice;
- with a TRS cable looped from the board's MIDI OUT into its own MIDI IN (the
  `midi_trs_loop` setup), the same announcement arrives back at the board and lands
  in `apply_parked`, where "the kit already current costs no load" (above) breaks
  the loop by construction -- no timer, no "ignore my own messages" table;
- a kit the listing cannot place (deleted under the loader, a card that changed
  since the scan) has no addressing to announce: it is remembered as announced
  rather than retried ten times a second, and the console says so.

The bank LSB (CC 32) is not sent: neither mode uses it in its addressing, and an
announcement should not write a latched value that means nothing. The channel is
the one the feature is filtered to (`KIT_PC CH`), or channel 1 when it listens on
any. The bytes go out through the pad-output routing -- USB, TRS, BLE, the same
doors as the pads' notes and the encoder's CC 1 -- not one fixed port, so a test
listens on whichever door the routing uses (`announced=`/`ann=` in `KITPC:` are the
board's side of the record, and do not depend on a listener being connected).

A slot that exists but is not a kit (a deleted kit keeps its id slot,
`crosspad-core/include/crosspad/control/KitList.hpp:18-20`) is shown with tag
`empty` and a `!`, and a Program Change naming it is **refused, not stepped
over** — skipping would move every later program up one and drift the host's
setlist under itself. In `group` such a slot has no kind to be counted in, so it
is not part of the bucket at all; the map shows each mode the way it resolves.

## Channels, and the one place they are off by one

The MIDI library gives callbacks channels **1..16**; the core API and the
BLE output are **0-based** (`crosspad-core/include/crosspad/midi/IMidiOutput.hpp:15-16`).
The note paths convert (`midiCallbacks.cpp:597-599`), but the `handleSample*`
family — CC, aftertouch, bend, program — passes the 1-based value straight
through, so a re-forwarded message lands one channel high
(`MidiInputHandler.cpp:85-88` → `EspBleMidi.cpp:964-966`, which masks `& 0x0F`:
channel 16 comes out as 1). Open PR #21 (`task/ble-fwd-channel`) fixes exactly
that in the forwards. `main/midi_kit_pc.cpp` converts once at its own door
(`kit_change_program(ch1, program)` takes 1..16 like a DAW and a person), so this
feature does not depend on that fix and does not add to it.

## Host Control Changes: a DAW's faders on this board's levels

The same pure idea as the kit map: a small table decides, code in
`main/include/host_midi_map.h` (pinned by `main/tests/test_host_midi.cpp`: 6
cases, 376 assertions), and `main/host_midi.cpp` takes the action on the board.

| | |
|---|---|
| What is addressed | a mixer **channel** by name (`channel "Sampler"`, `"Preview"`, `"USB"`, `"Sampler2"` with the prebuilt engine), a mixer **output** bus, a **hardware** output level (`IOutputLevels`, after the DAC, 0..100 %), or one **pad**'s live level (`IPadMix`) |
| Which messages | CC 7 for a level, `... PAN` for a balance (CC 10 by habit); a (channel, CC) pair is one rule, up to 48 live at once |
| Shipped defaults | CC 10 on channel 1 → the sampler channel's balance; CC 7 on channel 16 → the built-in speakers, in the hardware. Everything else is untouched and keeps its old life as a forwarded CC |
| Where it is kept | RAM, on purpose: which channel names exist depends on the kit and song loaded now ([sampler-engine.md](sampler-engine.md)), so a saved index would point at nothing after a reboot. A DAW template sets its mapping at session start; `MIDI_CC RESET` gives the two shipped rules back |
| Reading it | `MIDI_CC MAP [first] [count]` one line per rule with `cur=` -- the board's current value, expressed **as the CC number that would reproduce it**, which is what makes "put it back the way it was" a mechanical act for a test or a host |
| 7-bit to level | `levelFrom7` squares the ratio: half the fader is not half as loud, and a hardware step comes out of the same curve inverted (`percentFrom7`, `sevenFromLevel`) |
| Counters | `MIDI_CC STATUS` → `seen`, `applied`, `unmapped`, `refused`, `held`, `gives`, and `job=`/`mixer=` so a silent failure is a visible one: `MIDICC: rules=2 seen=0 applied=0 unmapped=0 refused=0 held=0 gives=0 job=host_midi mixer=0` (measured, v2 board, after a flash) |
| Handing the levels back | the first Control Change through a rule records what the board stood at, and `MIDI_CC GIVE` writes those values back. Clearing the map (`NONE`/`CLEAR`) and `RESET` hand over too: a level the host drove with no rule left to own it is a level nobody is looking after. `held=`/`gives=` are the record. A ride's later messages do **not** move the protected value -- otherwise a "give back" would restore wherever the fader happened to stop. Not covered: a **pad** level, because `IPadMix` has no getter, so the board cannot know what it had before the host touched it. Measured on the bench: `cur 90 → 18 → 90`, `held 0 → 1 → 0`, `OUTVOL` reading `out2=50` again (the 90/127 step is 50 %, and `20` comes back as `18` through the 0..100 percent step) |
| Answering back | a rule works both ways unless told otherwise: a 100 ms `host_echo` worker job reads the same getters `cur=` uses and, when a value moved on the board, sends the very CC the host would have sent (`MIDICC: … echoed=N echo=1`). Three rules keep it quiet instead of a loop: a slot is **seeded** on first read (a rule that has just been mapped says nothing), a Control Change the board applied **re-baselines its slot before the write**, to the value the echo will read back rather than the wire value (`echoBaseline()`: a hardware level is kept as a whole percent, so 40 of the 128 values come back off by up to 8 steps), so the host's own message never comes back at it — the ordering matters, the apply and the echo read run on different tasks, and a **handover deliberately leaves the baseline stale**, because a level going back to where the board stood is news a DAW wants. A balance goes out through the exact inverse of the pan curve (pinned across all 128 wire values in the host test); the send uses the same fan-out as the pads' own notes and the encoder's CC 1/CC 64, so it follows **the board's pad-output routing** (Settings: send2USB / send2STM / send2BLE) rather than one fixed port. `MIDI_CC ECHO off` silences it and seeds on the way back on, so it does not shout about everything that moved while off |
| Not echoed | a **pad** level (no getter to read, so nothing to compare against), and a mixer channel that is not on the desk right now (the read returns false, the rule is untouched) |
| Threading | the MIDI task parks four bytes in a small ring and returns; the shared `bsp_worker` job `host_midi` (25 ms) drains it and calls the mixer / the codec / the engine. `WORKER` over CDC shows its `max_us` -- it is a bookkeeping body, not a job that costs audio |
| Pad levels | live only, deliberately: `PAD_MIX` writes the kit model and marks it modified from the LVGL thread (`main/hil_control.cpp:393-415`); a CC fader riding a pad from a DAW is a playing decision, not an edit of kit.json, and it does not make the Sampler's save prompt appear |

## Host pad LEDs, and giving them back

The ask that every grid controller makes: a host paints the pads. What was
already there was the DAW Control protocol's `led`/`led_all` (`F0 7D 1E 02/03`),
which reaches the pads **only while the DAW Control app runs** -- frames go to
that app through `PlatformServices::stm32MessageHandler->setDawCallback()`. A
host that wants the pads while some other app is running needed its own plane,
and with it the part that is easy to forget: how the board gets them back.

```
SysEx (every profile, also the one with no CDC):
  F0 7D 1D 0D <pad> <r> <g> <b> F7      one pad
  F0 7D 1D 0E <r> <g> <b> x16 F7        the whole grid, one frame
  F0 7D 1D 0F F7                        give the pads back
CDC (the verbs, same effect, plus the animations):
  LED_HOST SET <pad> <r> <g> <b> | ALL <r> <g> <b> | GRID <96 hex>
  LED_HOST ANIM chase|ring|off [ms] | OFF | LEASE <ms>|AUTO | STATUS
```

Colours over SysEx are **7-bit**, doubled by the board -- the convention DAW
Control already uses (`components/crosspad-dawcontrol/src/DawProtocol.cpp:89-91`),
and the reason is that a SysEx payload byte of 0xF7 or 0xF0 would end the frame.
Over CDC they are 0..255.

- The first frame takes a **lease**: the board remembers its own sixteen resting
  colours, read from the LED model, so what comes back includes the running
  app's colours and not just global defaults.
- `0F` / `LED_HOST OFF` / `LED_HOST ANIM off` give it back, and so does the
  timeout, **by default after 5 s of host silence** (`LED_HOST LEASE <ms>` picks
  another length, `LEASE AUTO` returns to 5000, `LEASE 0` asks for the old
  hold-until-told). The clock is renewed by anything on this surface -- a painted
  pad, a whole grid, an animation step, or a Control Change that matched a rule --
  because the question the timeout answers is *is a host still there*, not *is it
  drawing this second*. Bench, 2026-11-19: paint the grid and go quiet and the
  pads return to the app's colours on their own inside the timeout; keep riding
  faders and the colours stay (`idle=400ms lease_ms=1400 lease=1 pad0=FF0000`).
  A bounded default is the safe direction because the person standing at the board
  cannot undo a stuck painting without a host, while a host that wants a sticky
  overlay says so in one message. The record is the counters
  (`leases=`/`releases=` in `LEDHOST:`), not the console: a line out of the UART
  costs ~2.7 ms on the task that also paints the pads, so the worker does not log
  a handover.
- Frames from either transport are parked **per pad** (one 32-bit slot per pad,
  plus one for an all-pads frame; a whole grid is staged by the main task and
  published with a flag) and applied by the `host_midi` worker job. Per pad
  matters: a chase sends several one-pad frames inside the 25 ms before the
  worker next looks, and with a single shared slot only the last of them would
  ever have been painted — the bench sends four SysEx pad frames back to back and
  checks that all four pads land. The strip is never written from the MIDI task,
  and the write lands on the same worker task that runs `pad_anim`, which is what
  the coalesced refresh assumes (CLAUDE.md, "Changing Kits While Playing").
  Measured: a SysEx pad frame reaches the LED model within one 25 ms pass.
- `LED_STATE` still answers with the model per pad (`colors=RRGGBB,…`,
  `main/hil_text.cpp:69-84`), which is how the bench check proves a frame landed
  instead of proving that a byte was sent.

## Known limits, said plainly

- **BLE MIDI hosts are not served yet**: `EspBleMidi::dispatchParsed` handles
  `0x90`, `0x80`, `0xB0` and `default: break;` (`:677-690`) — a Program Change
  from a BLE host is dropped at the transport. Making it work is a
  `case 0xC0:` there plus a dispatch hook in `IBleMidi.hpp` and
  `setHandleProgramChange` in `bleMidi_wireCallbacks()` — a crosspad-core change,
  deliberately not in this branch.
- **7 bits**: one Program Change reaches 128 slots, the MSB another 128. A card
  with more needs the MSB, and a re-scan (kits copied to the card, then reboot —
  the promise in `docs/manual/chapters/19-pliki.html:17`) moves the positions.
  Kit **names** are not an address: this card holds two kits called BASIC
  (ids 53 and 97).
- **Through the STM bridge** the pad-note path is documented as channel-1
  (`docs/subsystems/trs-midi.md:92`); what a host's Program Change looks like
  when it arrives that way is a bench question — measured in the worklog for this
  change, not assumed here.
- **No Settings row yet**: the switch and the channel filter are configured over
  CDC (`KIT_PC …`) and persist; a Settings → MIDI control belongs in
  crosspad-gui, with a hook, in a follow-up.
- **The verb is not in the harness catalog yet**: `crosspad-hil` refuses unknown
  verbs (`knowledge/cdc.yaml`), so until `KIT_PC` is added there — and to that
  repo's `tests/test_knowledge.py` verb set — reach it as
  `crosspad-hil cdc "KIT_PC" --expect KITPC:`. crosspad-hil PR #3 is making that
  catalog follow the firmware's table; `KIT_PC` must land in both.
- **The CC map and the LED lease are configured over CDC.** In the USB audio
  profile, which has no CDC, the LED side still works (SysEx `0x1D 0x0D-0x0F`)
  and the shipped CC defaults still ride levels, but the map itself cannot be
  pointed at a different target there. Adding `MIDI_CC` and `LED_HOST` to the
  companion allowlist (`main/companion_sysex.cpp`, over SysEx `0x1F` on USB MIDI
  or BLE) is the next step, and `MIDI_CC STATUS`/`LEDHOST:` print `job=` and
  `mixer=` so a missing verb or a missing job is visible rather than silent.
- **The echo is a second transmitter on the same wire.** A host that learns by
  ear, or a DAW template that maps a CC to a fader *and* keeps its own value, can be
  surprised by the board announcing a level; that is why `MIDI_CC ECHO off` exists and
  why the announcements ride the pad-output routing rather than appearing on every
  port at once. Measured on the bench: a handover of the Sampler channel's level came
  back on the wire as `CC 7 ch1 = 127` (`echoed 13 -> 14`) while the host's own
  `CC 7 ch1 = 30` produced nothing (`echoed 13 -> 13, wire []`).
- **No learn, no persistence, no Settings row** for the CC map or the LED lease:
  a person points a fader with `MIDI_CC SET`, and a Settings → MIDI control
  belongs in crosspad-gui with a hook, as the Program Change row does.
- **A host disconnecting does not restore levels by itself.** The handover is
  explicit (`MIDI_CC GIVE`) or comes with clearing the map; a DAW that muted an
  output and then vanished leaves it muted, which is what unplugging a hardware
  controller does on a real desk too. Restoring a level the moment a host goes
  quiet would undo deliberate mutes, so it is not what ships. Only the **pads** are
  taken back on silence, and they are the thing a person at the board cannot fix.
- **A lease is a snapshot, not a truce.** If the running app repaints a pad while
  a host holds the pads, giving the lease back restores what the board was
  showing when the host took over, and the app's newer colour is lost with it.
  The alternative -- diffing who painted what -- would need the LED model to
  remember a painter, which it does not (`PadLedController` has one resting
  colour per pad). Say so to anyone wiring a host and an app to the same pads.
- Kit loading from a host over the **MIDI+UAC2 profile** (no CDC) already exists
  as SysEx `F0 7D 1D 0B <kit id 0..127> F7` (`main/audio_route_control.cpp:411`,
  `crosspad-hil/crosspad_hil/knowledge/sysex.yaml:31`), and `KIT_LOAD`/`KIT_STATUS`
  are on the companion allowlist. A Program Change needs no such workaround: it
  is MIDI, and MIDI works in every profile.

## How this is tested

- `main/tests/test_kit_pc.cpp` — the decision and the map text, on the host, no
  board: 10 cases, 92 assertions (flat ordering and the 128 block boundary, a
  deleted slot refused rather than skipped, group buckets and their restarting
  programs, the channel filter, `off`, `addressable()` per mode, `pairAt`/`slotAt`
  agreeing with `resolve`, one-line-per-row and a name that cannot break the
  framing, and nothing half-written when a buffer is too small).
- The bench: a host (rtmidi on this machine) sends real Program Changes and Bank
  Selects to the board's own USB MIDI, and the board's `KIT_STATUS`, `KIT_PC
  STATUS` counters and console lines are read back
  (`.strata/tools/pctest.py` in the working copy of this branch; the run and its
  numbers are in the change's evidence).
- Regressions: `crosspad-hil run midi_bench` (both MIDI paths, loss and rate),
  `midi_stress`, `kit_churn` (the loader is the same one), and `smoke`.

A third door, for a host that would rather address a parameter than be assigned a
Control Change number, is the SysEx control surface: `F0 7D 1D 11/12/13/14/15` writes
blocks of addresses (pad colours and the palette they index, the board's own animations,
levels, the kit pair, the pad-LED timeout) and `14` hands them back where nothing else has
touched them since. Its address map, the byte counts that make it the cheaper transport
for a panel, and the things it refuses to answer for are in
`docs/subsystems/surface-sysex.md`; the console reads the same record with `SURFACE`,
`SURFACE PAD <n>` and `SURFACE LVL <sec> <addr>`.

Related: `docs/subsystems/midi-implementation-chart.md` (the same facts as a
chart a DAW owner can read at a glance: what is sent, what is recognized, on
which channel, with what default, and what is not implemented),
`docs/subsystems/kit-swap.md` (what a kit change costs and promises),
`docs/subsystems/trs-midi.md` (the jacks and the STM's routing),
`docs/subsystems/ble-midi.md` (the BLE side), `docs/subsystems/midi-clock.md`
(the one message family that was already documented), and
`crosspad-hil/crosspad_hil/knowledge/sysex.yaml` for the CrossPad frames.
