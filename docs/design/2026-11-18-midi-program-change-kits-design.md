# A host's Program Change picks the kit: what the board should do with it

Date: 2026-11-18. Status: **built on a local branch**
(`feat/midi-program-change-kits`, from `main` `697ac61`), host-tested and
bench-tested below; **not pushed**, not a pull request — the human reviews first.

## Why

A pad controller that a DAW can drive already receives notes, clock, CC and
aftertouch from that DAW. The one thing a DAW uses to change *what the pads are*
— a Program Change (`0xC0..0xCF`, "patch change") — arrives and does nothing:

- `main/midiCallbacks.cpp:645-650` (`pcHandler`, the STM32 bridge) and
  `:673-678` (`usbPcHandler`, TinyUSB) both hand it to
  `crosspad::getMidiInputHandler().handleSampleProgramChange(ch, program)`.
- `components/crosspad-core/src/midi/MidiInputHandler.cpp:85-88` — the whole
  body is "if `esp32Routing.Midi2BLE`, re-transmit it to the BLE peer". A Program
  Change therefore changes nothing on the board and, unless that routing is on,
  is dropped in silence.
- Nothing in the tree gives it a meaning: no bank/patch table anywhere, no MIDI
  learn for it, and the DAW Control protocol (`F0 7D 1E …`) has no equivalent
  sub-command. Over BLE MIDI it is worse still: `EspBleMidi::dispatchParsed`
  (`components/crosspad-platform-idf/EspBleMidi.cpp:677-690`) parses `0x90`,
  `0x80`, `0xB0` and `default: break;` — a Program Change from a BLE host is
  discarded at the transport.
- The user manual says exactly what the board promises today:
  `docs/manual/chapters/17-midi.html:7` "CrossPad odbiera nuty i zegar" — it
  receives notes and clock. Program change appears in no chapter, and in no page
  under `docs/`. Kit switching from a host exists only on the bench control
  plane: CDC `KIT_LOAD` and SysEx `F0 7D 1D 0B <kit> F7`, and
  `CLAUDE.md` says plainly "Kit sync is host-side only … never from inside the
  DAW".

So this is a hole in what the device offers, not a bug in an existing promise —
which is why this note writes the promise down before the code.

## What the device should do

A Program Change selects a kit, the way it selects a patch everywhere else:
without touching what is playing, without asking, and with a way for the host to
find out what it addressed.

### The address

| mode | what the pair names | why it is there |
|---|---|---|
| `flat` (default) | **bank MSB (CC 0) · program** = the *position in the board's own kit listing*, in the order and with the ids `KIT_LIST` prints; the MSB opens the next block of 128 | what the host sends is readable straight off a listing; positions do not depend on what kind of kit sits there, so a card that gets new kits only needs a fresh listing, not a new setlist logic |
| `group` | **bank MSB** = the kind (0 performance, 1 drums, 2 slice, 3 pitch, 4 the user's own — the buckets the kit browser uses), **program** = the position inside that kind | a setlist can be written per kind without knowing how many kits of each kind a particular card holds |
| `off` | nothing beyond what it does today (the BLE re-forward) | a board used as a pure controller in a rack must not change kits because a DAW recalled a bank |

Two ways of counting on purpose: in `flat` a position is the listing's own slot,
so a deleted kit stays where it was and is *refused by name* rather than skipped
— skipping would move every later program up one and let a setlist drift under
itself. In `group` a position counts only the slots that are kits at all,
because "the third drum kit on the card" cannot mean a hole where a drum kit
used to be. The map the board prints (`KIT_PC MAP`) is built the same way the
mode resolves, so a host never reads one rule and hears another.

### The behaviour, item by item

1. **It is a kit change, so it is the kit change the manual already describes.**
   `docs/manual/chapters/03a-technologia.html:60` promises: a new kit loads pad
   by pad in the background, what is playing finishes on the old sample, a hit on
   a pad that is being reloaded is played right after the reload. A Program
   Change goes through the same loader (`kit_load_task_start`) with its
   newest-wins queue and its render gate — no new loading path, no stopping
   engines, nothing that could break the swap contract
   (`docs/subsystems/kit-swap.md:29-35`, `:196-202`).
2. **It works whatever screen is up.** The current kit is global state
   (`IKitManager::getCurrentKitId`), not Sampler-app state, so a host can change
   the kit while the launcher, a song, or no app at all is in front.
3. **A refusal is never silent.** Console line with the reason (`off`, `channel`,
   `out_of_range`, `no_kits`, `not_loadable`, "the card is still being scanned"),
   counters a host can read (`KIT_PC STATUS`: `seen/loaded/refused/ignored`,
   `last_kit`, `last`), and a map a host can ask for (`KIT_PC MAP [first]
   [count]`). "I sent a program change and nothing happened" must be answerable
   from the board, not from a guess.
4. **A Program Change the board cannot address does nothing else.** It keeps the
   kit it has. It is not mapped to "next kit", not wrapped, not ignored-with-a-hint.
5. **A Program Change for the kit that is already current is not a load.** The
   loader is not asked, so a DAW that resends a bank at session start costs no
   card pass. It still counts, so the numbers explain what the host sent.
6. **The unsaved-pad-edit consequence is the manual's existing one.**
   `docs/manual/chapters/07-sampler.html:53-54`: loading another kit does not ask
   to save. A Program Change loading a kit has the same effect as tapping that
   kit in the browser, and that is the promise already made. It is worth saying
   out loud in the MIDI chapter, because a host-driven switch is easier to fire
   by accident than a tap.
7. **Persistence**: the mode and the channel filter survive a reboot and a
   settings export, in their own namespace (`cfg_midi/pc_kit`, `cfg_midi/pc_ch`).
   Not a field of `CrosspadSettings`: crosspad-core owns that struct and its
   settings table is being reorganised by an open PR, so this stays out of its
   way (precedent: the USB host placement `cfg_usb/mode`,
   `main/usb_host_profile.cpp:11-12`).

### Boundaries and limits, stated

- 7 bits: `flat` reaches 128 kits per bank MSB; a card with more needs the MSB,
  and a listing that is re-scanned moves positions — the same fragility kit ids
  already have, and the reason the board prints the map instead of assuming the
  host knows it. Kit names are not unique (two kits called BASIC on this bench,
  ids 53 and 97), so ids and positions are the address, never names.
- Channels: a Program Change carries one. The board's own outgoing notes use one
  pad channel (`REG_CTL_MIDI_CHAN`), but host-in messages are heard on all 16
  (`USB_MIDI.begin(MIDI_CHANNEL_OMNI)`, `main/midiCallbacks.cpp:579`). The filter
  is therefore a choice, not a necessity: 0 = any channel (default), 1..16 = one.
  The rest of this feature's channel handling is 0-based inside the module, with
  the library's 1-based value converted once — the existing `handleSample*`
  forwards pass 1-based into a 0-based API, which is the bug open PR #21 fixes
  for the BLE forward; this module does not add to it and does not fix it either.
- Over the STM bridge, `docs/subsystems/trs-midi.md:92` warns the ESP parses the
  STM's stream expecting the pad notes on channel 1; what a *host* Program Change
  looks like when it arrives through the bridge is a bench question, asked and
  answered in the test section, not assumed.
- BLE MIDI: still discarded at the transport. Making it work means a
  `case 0xC0:` in `EspBleMidi::dispatchParsed` plus a dispatch hook in
  `crosspad-core/include/crosspad/midi/IBleMidi.hpp` — a core change, so it is
  deliberately out of this branch and left as a follow-up with a name.

## How it is built here (the boundaries that shaped it)

- The MIDI parsers live in the `uartLoop` task, which the repo already refuses to
  block (main/hil_control.cpp:459-464: a pad hit that waits 1.1 s on the card
  made every CDC reply time out). So `kit_change_program()` does one thing: park
  `{channel, MSB, LSB, program}` in one atomic word and post one async apply —
  park-and-coalesce newest-wins, the pattern `main/hil_control.cpp:397-420`
  already uses for a host riding pad levels.
- The kit model is the LVGL thread's, and `kit_load_task_start()` may only be
  called from there (`crosspad-sampler/include/crosspad-sampler/logic/kit_load_task.hpp:18-31`),
  so the lookup and the load both happen in the async apply. `lv_async_call_locked`
  never makes the caller wait (`main/include/lv_async_safe.h:24-29`) — which is
  what makes this legal from a MIDI callback at all.
- Two guards, in the order the CDC verb and the SysEx sub use them
  (`main/hil_control.cpp:449-457`, `:4691-4701`): the card must not be scanning,
  and the id must be one the manager holds.
- The console line a load prints stays byte-identical to
  `hil_control: KIT_LOAD <n> queued|started`, because
  `crosspad-hil/crosspad_hil/knowledge/markers.yaml:75` waits on it and a change
  asked for over MIDI should be as visible to the harness as one asked for over
  CDC. A request over MIDI logs its own line too, naming the reason.
- The decision itself is a header with no ESP, LVGL or FreeRTOS in it
  (`main/include/kit_pc.h`) and no allocation, so it is pinned by
  `main/tests/test_kit_pc.cpp` under the same Catch2 harness the repo already
  uses for `hil_text.cpp` and `cdc_line.h` — and so a Program Change arriving at
  3 a.m. on a full card allocates nothing.

## Decisions left to the human

1. **The shipping default.** `off` keeps exactly the promise the manual makes
   today; `flat` (what this branch builds, so the feature can be heard) answers
   the request "a program change should change the kit". One constant:
   `kDefaultMode` in `main/midi_kit_pc.cpp`.
2. `flat` or `group` as the shipped mode.
3. Whether a Program Change should be refused while **DAW Control** owns the
   pads (a DAW that both plays the pads and recalls patches would then change
   the board's kit on purpose — or never, by policy).
4. Whether the Settings app should offer a row for it (a `crosspad-gui` change
   plus a hook, deliberately not here).
5. Whether the pure mapping should live in crosspad-core instead, so the
   simulator and the Arduino build share it. It is one commit to move; the
   reason it is not moved here is that crosspad-core has four open PRs of its own
   and a small feature should not create a fifth collision.

## Where this collides with the open queue (read before merging)

- PR **#21** (`task/ble-fwd-channel`) edits the same four lambdas in
  `main/midiCallbacks.cpp` (it adds `ch - 1` to the CC/AT/bend/program forwards).
  Whichever lands second takes a four-line conflict; both changes are correct and
  independent.
- PR **#24** (`refactor/verb-catalog`) moves the kit verbs out of
  `main/hil_control.cpp` into `main/hil_verbs_kit.cpp`, and rewrites `CLAUDE.md`.
  The `KIT_PC` verb added here would move with them; the docs here would move
  into `docs/dev/`.
- `crosspad-hil` PR **#3** keeps `knowledge/cdc.yaml` in step with the firmware's
  verb table. `KIT_PC` must be added there (and to that repo's
  `tests/test_knowledge.py` verb set) before `crosspad-hil cdc KIT_PC` answers —
  until then it is reachable only as `crosspad-hil cdc "KIT_PC" --expect KITPC:`.

## Addendum the same day: the rest of the host surface (levels and pad LEDs)

The next thing a person wiring a DAW to a CrossPad asks for, in the same sitting:
"volume control via MIDI for each track", and "SysEx messages to overwrite the
LED status like on a Novation Launchpad, to play back animations — and a reset of
the LEDs would be good". Both went onto this branch as one module, because both
are the same three parts: a table that decides, one place that acts on the board,
and a way to read back what the board is doing now.

**What is there now** — `main/include/host_midi_map.h` (pure: rules, the 7-bit
curves, one frame of colours, the hex parser), `main/host_midi.cpp` (the map, the
parking, the `bsp_worker` job `host_midi` that applies it, the LED lease), the
verbs `MIDI_CC` and `LED_HOST` in `main/hil_control.cpp`, SysEx `0x1D` subs
`0x0D`/`0x0E`/`0x0F` in `main/audio_route_control.cpp`, hooks in
`main/midiCallbacks.cpp`, host tests in `main/tests/test_host_midi.cpp` (6 cases,
376 assertions) and a bench run with 16 checks (`.strata/tools/surfacetest.py`).

**Decisions taken here, and why**

- *A table, not fixed assignments.* "Each track" does not name a thing on this
  board: the mixer's channels are whatever the engines registered, per kit and
  per song (`"Sampler"` natively, `"Sampler2"` with the prebuilt engine, plus
  `"Preview"`, `"USB"`; an Arrange seq track's fader is a *trim* on the engine
  channel its notes play through, `AudioMixerEngine::setChannelTrim`). So a CC
  points at a **name**, resolved when the message arrives and refused, with the
  name in the log, if it is not on the desk now. Pointing at song tracks later is
  a new `Target`, not a redesign.
- *RAM, not NVS.* A saved index would point at nothing after a reboot with
  another kit loaded; a saved name too, if the kit changed. A DAW template
  declares its mapping at session start anyway, and `MIDI_CC RESET` returns to
  the two shipped rules (CC 10 on channel 1 → the sampler's balance; CC 7 on
  channel 16 → the speakers, in the hardware so the hiss goes down with it).
- *`cur=` in `MIDI_CC MAP` is the CC number that reproduces where the board is*,
  not the board's own unit — the same curve inverted. That makes "put it back the
  way it was" a mechanical step for a host and for a test.
- *Pad levels over MIDI are live only.* `PAD_MIX` edits the kit model from the
  LVGL thread and marks it dirty (`main/hil_control.cpp:393-415`); a console
  fader riding a pad is a playing decision, and silently making kit.json dirty
  is the sort of surprise this project already documents elsewhere.
- *A lease for the LEDs, with an explicit give-back.* The board's own colours —
  a running app's per-pad colours included — are snapshotted when a host takes
  over and restored by `0F` / `OFF` / `ANIM off`. Hold-until-told is the default
  (`LEASE 0`) so painting one pad and walking away keeps working; an animation
  host is told to set a few seconds so a crashed host cannot leave a board
  painted.
- *7-bit colours over SysEx, doubled* — what DAW Control already does
  (`components/crosspad-dawcontrol/src/DawProtocol.cpp:89-91`); a payload byte of
  0xF0/0xF7 would end the frame. Over CDC they are full 0..255.
- *Everything is applied by one worker job.* The MIDI task parks four bytes and
  returns; the job runs on the shared `bsp_worker` task — the one that runs
  `pad_anim` — so the LED model is written where it is already painted and no
  strip write happens from a MIDI callback.

**What the tests caught, in this order** (the argument for having both kinds):

1. `parseFrameHex` first wrote the grid through the struct's raw bytes; a pad is
   four members (r, g, b and an "addressed by this frame" flag), so the frame
   landed on the wrong pads. A host test with two known pads failed at once.
2. The verb argument splitter (`next_arg`) nulled the separator and left the
   cursor **on** it, so every multi-argument form — `MIDI_CC SET …`, `LED_HOST
   SET …`, `GRID`, `ANIM` — answered `usage`, while single-word forms worked. No
   host test can see that (it is file-local to `host_midi.cpp`); the bench run
   did, and one probe line named it: `ERR LED_HOST SET 0 255 0 0 (usage)`.
3. The hardware read-back went through a 0..100 % step and so could not be
   restored by the same CC: 63 → 25 % → 31. The bench's "put it back" check
   failed with exactly those three numbers, which is how it was diagnosed — and
   the check then became the proof of the fix (64 → 0 → 64).
4. The bench test itself had a bug worth noting, because it is the classic one:
   after adding a `channel "Sampler"` **level** rule, the test read the first
   `target=channel` row it found — which is the shipped **pan** rule on the same
   channel and name, and CC 7 cannot move a pan. The board was right; the test
   was watching the wrong number. Distinguishing them by `cc=` and `mode=` in the
   reply is what fixed it.

A lesson to keep: an argument splitter whose only exercise is on hardware belongs
in the pure header with a host test as soon as a second verb uses it. It is
file-local here because these verbs were the first users.

**Left open on purpose** — `MIDI_CC`/`LED_HOST` over the companion plane (SysEx
`0x1F`) so the profile without CDC can re-point a fader (the LED side already
works there); a Settings → MIDI row in crosspad-gui for the Program Change mode,
the channel filter and the CC map; whether the pads' host animation should become
a `PadAnimator` type in crosspad-core with a token, like the boot/progress ones;
and BLE Program Change, still open from the first section.

## Tests (the evidence this change ships with)

Host, no board — `main/tests/test_kit_pc.cpp`, 9 cases, 60 assertions, run here
under Catch2 v3.5 (MinGW 7.3 has no sanitizer runtime, so ASan runs in their CI;
`.strata/tools/host_tests.bat` is the local recipe):

- flat: positions, the 128 block boundary, past-the-end, and that a deleted kit
  is refused **by name** without shifting the programs above it;
- group: buckets, a bucket that runs out, an unknown kind, and that a hole is not
  counted in the bucket;
- the channel filter and the `off` switch;
- `addressable()` in both modes (holes count in flat, not in group);
- the map line: prefix kept, one line per entry, a name containing a newline and
  a quote sanitised, a full buffer stopping cleanly, `first` past the end, and the
  group ordering;
- ids that are not `0..n-1`.

Bench, on this machine, against a real card with 121 kits — see the worklog for
the runs and the numbers.
## Addendum, 2026-11-19: the host's control is bounded

The first cut left two open decisions named in the limits section, and this
increment answers them in the safe direction, with a way back for anyone who wants
the other one.

**The pad-LED lease now times out on its own, after 5 seconds of host silence.**
It used to be "hold until told" (`LED_HOST LEASE 0` was the default), which means a
host that died — a crashed script, a laptop that slept, a cable pulled — left a board
painted, and the person standing in front of it had no way back: the front panel has
no "give the pads to the board" control, and the running app's own colours are
overridden until a host sends `0F`. So the *default* is bounded now, `LEASE 0` asks
for the sticky behaviour explicitly, and `LEASE AUTO` returns to the shipped 5000 ms.
The clock is renewed by anything the host surface receives — a painted pad, a grid,
an animation step, a Control Change that matched a rule — because what is timed is
whether a host is present, not whether it is drawing this second: a DAW riding
faders keeps the overlay it painted, and a host that stops talking loses it inside
the timeout. Bench evidence (`.strata/tools/surfacetest.py`, 29/29): paint, go
quiet, pads back to the app's colours; keep riding CC 10, `lease=1` after 2.4 s with
a 1400 ms timeout; go quiet, `lease=0`.

**Levels a host drove are handed back, but only when the host gives up control.**
The first Control Change through a rule remembers what the board stood at; later
messages of the same ride do not move that record, so `MIDI_CC GIVE` restores the
board's value and not wherever the fader happened to stop. Clearing the map
(`MIDI_CC NONE`/`CLEAR`) and `MIDI_CC RESET` hand over too, because a level driven
with no rule left to own it is a level nobody is looking after. `MIDI_CC STATUS`
counts `held=` and `gives=`.

What deliberately did **not** ship: an automatic restore on silence. A DAW that
muted an output over CC 7 and then sat quiet for five minutes would be unmuted by
the board, which is worse than the stuck fader it prevents — on a real desk,
unplugging a controller leaves its last position standing. Only the pads are taken
back on silence, because only the pads are unrecoverable from the panel. A second
gap, honestly recorded: a *pad* level driven by a CC cannot be handed back either,
because `IPadMix` has no getter, so the board cannot know what it had before the
host touched it. Measured round trip on the shipped speaker rule: `cur 90 → 18`
(the asked 20 becomes 18 through the 0..100 percent step — see finding F-18 in the
workspace notes), `held 0 → 1 → 0`, and `OUTVOL` reading `out2=50` again after the
give.

The unit tests for the decisions are in `main/tests/test_host_midi.cpp` (the
snapshot is idempotent, a give restores once and then no-ops, the boundary of the
timeout is "more than", and traffic renews it); the parts that need a board are in
the bench ladder. Two things the increment also fixed in the test harness itself:
`host_tests.bat` set an exit code it never returned, and a test of mine expected the
wrong thing *at* the boundary (exactly 5000 ms is not "more than 5000 ms") — the
code was right and the expectation moved.

The same increment also turned the mapping around, which is what makes a mapped
Control Change behave like a hardware controller rather than a one-way remote. A
`host_echo` worker job (100 ms) reads the desk with the same getters the `cur=`
read-out uses and, when a value moved at the board, sends the very CC the host
would have sent for it. Three rules keep that from being an echo chamber, and each
of them is a decision rather than an accident: a slot is **seeded** the first time
it is read, so mapping a rule never announces a level that was already standing
there; a Control Change the board applies **re-baselines its slot before writing**
— the apply runs on one worker job and the echo reads on another, so a value that
landed before its baseline could be mistaken for a board-side change and sent back
at the host that just asked for it; and a **handover deliberately leaves the
baseline stale**, because the level going back to where the board stood is exactly
the news a DAW wants to hear. A balance uses the exact inverse of the pan curve,
pinned across all 128 wire values by the host test — which caught a rounding bug I
had just written, where truncating instead of rounding would have answered 64 where
the wire said 65. The bytes go out through the same fan-out as the pads' own notes
and the encoder's CC 1/CC 64, which means they follow the board's pad-output
routing (USB / TRS / BLE) instead of appearing on every port at once, and
`MIDI_CC ECHO off` silences them and seeds on the way back on.

Bench, on the wire rather than in a counter: the host's own `CC 7 ch1 = 30` produced
nothing (`echoed 13 -> 13`, nothing heard), the handover that followed produced
`CC 7 ch1 = 127` (`echoed 13 -> 14`), `ECHO off` produced nothing while still
restoring the level, and turning echo back on produced nothing for what had moved
while it was off. 32/32 checks, with the board's MIDI heard on both MIDI doors the
routing might be using (`xpad.Sniff`, new: rtmidi's input side, which had not been
needed here before).

## 2026-11-19 (later): the kit travels both ways

The Control Change map answers back (the other addendum), so Program Change got the
same treatment: when the kit changes at the board, the board sends the Bank Select
MSB and Program Change that name it, in the current mode's ordering -- the exact
pair `KIT_PC MAP` shows for that kit. `positionOf` (pure, in `kit_pc.h`) is the
inverse of `pairAt`, and the host test pins the round trip for every kit in a
listing built to tell `flat` from `group`: kit 13 of the fixture is position 3 in
`flat` (a deleted kit keeps its place) and position 2 in `group` (a hole is not a
kit of any kind, so it is not counted). Without that identity the map and the
board's own announcements would name different programs for one kit, which is how a
setlist quietly drifts.

What is deliberately *not* announced, and why:

- a kit the host reached by Program Change is seeded as announced before the load
  is requested. The load is asynchronous, so without the seed the board would
  report the kit it was on and then the kit the host asked for, twice;
- the bank LSB (CC 32): neither mode reads it in its addressing, so sending it
  would write a latched value that means nothing to this board;
- a kit the listing cannot place (deleted under the loader, a card that changed
  since the scan) is remembered as announced rather than retried on every pass --
  an unresolved case should cost one log line, not ten announcements a second.

A loop that would otherwise bite: with a TRS cable from the board's MIDI OUT into
its own MIDI IN (the `midi_trs_loop` bench setup), the board receives its own
announcement. The seeding above handles the case it is asked about, and the
pre-existing rule "the kit already current costs no load" (the loader is not
called for the kit it holds) handles the general case -- no timer and no
"recognise my own messages" table, which is the kind of machinery that rots.

Measured on `dev_8dda`: at boot the board announced kit 53 once (`ann=1`,
`announced=53`); a kit loaded with `KIT_LOAD` over CDC produced `CC 0 = 0` and
`PC = 2` on the wire (heard on both MIDI doors, in case the routing answers on the
other one) with `ann 2 -> 3`; a kit the host reached by Program Change produced
nothing (`wire []`, `ann` unchanged) while `announced=` named it; `KIT_PC ECHO off`
produced nothing and `ECHO on` replayed nothing about the past. Kit ladder 14/14
(up from 10: those four cases are the new checks), surface ladder 33/33, host
tests 140 + 27 + 535 assertions. `KITPC:` gained `echo=`, `announced=` and `ann=`
without truncating, which is not a given for a verb line a 128-byte CDC reply has
to split (the measured line is 128 characters).

Two harness bugs this exposed, both mine and both worth naming: the local
host-test ladder ran the *previous* binary after a failed compile and printed
"All tests passed" for a header that did not build (fixed: the exe is deleted
before the compile, so a compile failure leaves nothing to run), and its
`exit /b %RC%` sat after `endlocal`, which throws the value away -- so the ladder
had been returning 0 whatever happened, including the fix I made the day before
(fixed: `endlocal & exit /b %RC%`, one line, the value read while it still
exists). A ladder that always passes is worse than no ladder: three of today's
real failures were visible only in the log text, not in the exit code.
