# The SysEx control surface: addressing the board instead of being assigned a CC number

<!-- crosspad-docs/site/listing needs a row for this page (see the note at the bottom). -->

A DAW drives one parameter at a time: a fader is a Control Change number, and the
Control Change map (`docs/subsystems/midi-input.md`) says which number means what. A pad
panel does the opposite: it paints sixteen pads, then sixteen more, sixty times a second,
and it does not want to be told which of its buttons happens to be CC 23 today.

This page describes the second style: address writes in SysEx, the shape Roland and
Novation use for their surfaces, on CrossPad's frames. The decisions are in
`main/include/surface_map.h` and pinned by `main/tests/test_surface_map.cpp`; the part
that touches the board is in `main/host_midi.cpp` under "applying the SysEx surface"; the
frames are decoded and dispatched in `main/audio_route_control.cpp`.

    F0 7D 1D 11 <section> <address> <count> <values…> F7    write a block
    F0 7D 1D 12 <address> <count> <r,g,b …> F7              write pad colour directly
    F0 7D 1D 13 <index> <count> <r,g,b …> F7                load palette entries
    F0 7D 1D 14 <section> <address> <count> F7              hand back what a write changed
    F0 7D 1D 15 F7                                          the counters, as a SysEx reply

`7D` is the manufacturer byte and `1D` the audio-route command every one of CrossPad's
control SysEx frames already uses (`crosspad-hil`'s `knowledge/sysex.yaml` is the catalog
of record). Values are 7-bit; the board masks to 7 bits rather than refusing a byte, which
matches how the older `LED_PAD`/`LED_GRID` subs of the same command behave.

## What the addresses mean

| Section | Addresses | Value | Given back on request? |
|---|---|---|---|
| `0x00` pad mode | 0..15, one per pad | 0 = the board's own colours again, 1 = the host holds this pad | yes: writing 0 hands that one pad back at once |
| `0x01` pad colour | 0..15 | a palette index 0..31 | yes |
| `0x02` pad mix | 0..15 | live level, 0..127 through the same curve as a mapped CC | **no** — `IPadMix` has no getter, so there is nothing to compare and nothing to put back |
| `0x03` channel volume | 0..15 | `levelFrom7` | yes |
| `0x04` channel balance | 0..15 | `panFrom7`, 64 = centre | yes |
| `0x05` output bus volume | 0..7 | `levelFrom7` | yes, but see "held at unity" below |
| `0x06` hardware output level | 0..7 | `percentFrom7` | yes |
| `0x07` animation | 0 = which (0 off, 1 chase, 2 ring, 3 strobe), 1 = period/10 ms | | yes (the previous animation is recorded) |
| `0x08` kit | 0 = bank MSB, 1 = program — the pair `KIT_PC MAP` prints | | **no**, refused by the grammar (below) |
| `0x09` pad-LED lease | 0 = timeout in 100 ms units, 0 = hold until told | | refused: a timeout has no previous value a host could mean |

A section the board cannot answer honestly is refused **by the grammar** rather than
accepted and quietly ignored: `F0 7D 1D 14 aimed at `0x02`, `0x08` or `0x09` comes back as
a refusal with a reason (`unknown_section` in the counter and the log) and changes
nothing. A surface that accepts a frame and then does nothing is the worst thing it can do
to the person holding the cable, so the refusal lives in the decoder and the host test
pins it.

Colour: `0x01` writes a palette index; the palette is loaded by `13` (32 entries, 7-bit
triples, doubled to the strip's 8-bit exactly as the older `LED_PAD`/`LED_GRID` frames
do). `0x12` writes colour straight into the frame instead — 7-bit per component, so half
the resolution the pads physically have, and the price of a 7-bit wire. Both end up in the
same place: one pad, one colour, held until something gives it back.

## Why blocks of addresses, and what they cost

The byte counts are arithmetic on the frame, so they can be checked by building the frame
— which `main/tests/test_surface_map.cpp` does, rather than reciting a number:

| Same sixteen pads, painted once | Bytes |
|---|---|
| Palette indices (`11` + 16 values, palette loaded earlier) | **24** |
| Direct colour (`12` + 48 bytes) | 55 |
| One pad per frame (`LED_PAD`, sixteen frames of 9) | 144 |
| NRPN (four CCs of three bytes per parameter) | 192 |

At a TRS line's 31250 baud a byte is ~320 µs of line time, so 24 bytes is ~7.7 ms:
more than a hundred grid frames a second on one wire (the loopback bench measures 1027
messages/s on that line, so frames, not bytes, are the limit there). Over USB MIDI the
same message travels in 4-byte packets, which rounds these up; the ordering is the same.

Palette indices are also what makes a colour-cycle animation free: the grid frame already
carries sixteen indices, so walking the word's colour through six palette entries costs
no extra bytes (`stratatest/tools/ledshow.py` does exactly this).

**NRPN is not the wrong choice in general.** For one parameter a DAW automates, NRPN is
better: no vendor framing, per channel, 14-bit, and it shows up in an automation lane as
something a person can pick with a mouse. For sixteen pads it is four messages per pad and
14 bits is not a colour. So this board keeps both doors and does not make a host choose
between two representations of the same parameter: a CC the map points at, and an address
in this surface, are the same thing at the mixer.

## Giving it back, and what "previous state" means here

`F0 7D 1D 14` hands back what a previous write changed -- with one rule, which is the
reason this section exists:

> A value is restored **only if it still equals what the surface left there**. If the
> board (or another host, or a verb at the console) moved it since, the surface leaves it
> alone and counts a skip.

An app lights a pad to show what is playing, and a sampler presses a pad under a
finger: a restore that ignored that would stamp a colour over information the player in
front of the board is being given. The same rule applies to levels, and it is why the
comparison happens in the board's own units rather than the wire's: a hardware output's
7-bit value goes through a 0..100 percent step and comes back a step off (finding F-18),
so comparing wire values would report "not mine" for a value nobody touched.

`before` is taken at the surface's **first** write of an address and never moved by later
ones, so a series of animation frames ends with the board's colour from before the series
started, not with wherever the last frame stopped. That is the same reasoning as the
Control Change map's snapshot.

Two more pieces of the same story:

- **The lease covers surface pads too.** Pad colours a host paints are host-owned, and the
  bounded pad-LED lease (`docs/subsystems/midi-input.md`, added after a dead animation
  host left a board painted forever) takes them back when the host goes quiet -- 5 s by
  default, `0x09` writable, `LED_HOST LEASE` from the console. `pad mode = 1` is the
  per-pad exemption: a panel painting a mode view can mean that view to stay until it
  says otherwise, without making the whole board sticky for every other host. A held pad
  keeps its record, so a later explicit hand-back is still honest about what it may touch.
- **A kit write is not undoable from this task.** Undoing a kit means loading the previous
  one, which needs the kit listing, which the UI task owns -- and a restore is applied on
  the worker. Rather than accept the frame and do nothing, the grammar refuses it. The
  host has the pair already (it wrote it), so keeping it costs one byte of host state
  rather than a round trip and a threading dance.

## Held at unity: what the board's own design does to one address

`0x05` (mixer output bus) is real -- the mixer engine accepts it -- but on CrossPad an
output that has its own hardware level plays at unity in the mixer and gets its actual
level from `0x06`. A write to `0x05` on this board is recorded and then re-asserted away.
Measured on `dev_8dda` (v2, PCB r20) with the bench ladder: the surface was asked for
0.617, its record says `wrote=617`, and the bus reads `now=1000` -- both numbers shown
side by side by `SURFACE LVL out 0`, which is the point of that read-out. A host that
wants the level of a physical output writes `0x06`.

Which index of `0x06` is which physical output is worth knowing before writing: the
board's list is **speakers, phones, line out** (`OutputLevelsPort.cpp`), so address 0 is
the built-in speakers. Bench ladders that value their ears use address 2 (Out 3/4).

## Reading the record from the console

    SURFACE                     writes/values/missed/restores/skipped/refused/drops,
                                pad claims and holds as bitmasks, palette revision,
                                the current lease timeout and animation
    SURFACE PAD <0..15>         that pad's colour now, what the surface wrote, what the
                                board stood at before, and whether the surface may still
                                take it back (claim / mine / hold)
    SURFACE LVL <sec> <addr>    the level address's record in the board's units and in the
                                7-bit form a host writes; `pad_mix` says NO_READBACK out
                                loud rather than leaving a blank field
    SURFACE CLR                 forget the surface's records (a panel re-syncing from
                                scratch should not be judged against frames it has
                                forgotten sending)

`missed` counts values whose target was not there (no such output, a channel not
registered right now); `values` counts the ones that landed. Before that split they were
one number, and a host asking "did my write do anything?" got an answer that meant
"anything". `F0 7D 1D 15` carries the same counters to a host with no CDC -- 15 bytes,
ending at the terminator.

## Threading

Unchanged from the rest of the surface: a frame arrives on the MIDI parsing task and only
parks into a small ring (8 actions; the worker drains every 25 ms; a full ring drops the
newest and counts `drops`); one `bsp_worker` job applies, and it is the same task that
already paints the pads, so the LED model and the mixer each have one writer. Measured on
`dev_8dda` with the bench running: `host_midi period=25 last_us=18`, `host_echo
period=100 last_us=37 … overruns=0`. The `SURFACE` verb's own fields report which job is
registered (`ring=host_midi`, or `NONE` if it was never added, which is a board where
nothing applies).

A surface write to a level that a Control Change rule also maps looks like a change the
board made to the level echo (`MIDI_CC ECHO`), because from the desk's point of view it
is: the echo compares the desk's value against its baseline per rule, and does not know
which door the value came in by. A host that writes by address and also rides the fader
will get its own value back once. Stated here because it is a consequence of the design,
not a bug to fix in one place.

## Not measured yet, and what to be careful about

- **Two numberings for one output.** The surface's `HwLevel` addresses follow
  `OutputLevelsPort`'s order -- speakers, phones, line out -- so address 2 is Out 3/4 on
  v2, and address 0 is the built-in speakers. The console verb `OUTVOL 1 <pct>` is
  labelled by the board's own `OUTVOL` line as "out1 = codec1 = Line out 3/4", which sounds
  like the same output; driving it to 55 left `SURFACE LVL hw 2` reading 30, so they are
  not the same control. A host should address by this page's numbering and read the answer
  with `SURFACE LVL`; a bench that wants to show "someone else moved this level in the
  meantime" has to find a door that actually writes the value the surface reads. (The
  hardware level also goes through the squared curve: a 7-bit 70 is ~30 %, not 55 %.)
- **The optical checks exist but did not run on this bench.** The bench ladder
  (`.strata/tools/sysxsurface.py`) has a section that grabs three webcam frames -- before,
  SysEx-painted, given back -- and compares them with the calibration's per-pad
  measurement. It is skipped unless `stratatest/out/calib.json` exists, and it did not
  this session: the pad grid was lit and in frame (visible in a captured PNG) but
  `cam.py calibrate` would not accept its own lattice, and the room was dark, which is a
  different light from the one the calibration was made under. So the colour values in
  this page's evidence come from the board's LED-model read-out, and what the pads look
  like to a person is not yet certified. `.strata/findings.md` F-24 has the detail.
- **The slideshow tool has not been run against a board yet.**
  `stratatest/tools/ledshow.py` prints its font, counts its bytes offline (`--dry-run`:
  35 window frames, 899 bytes, ~25 bytes per grid frame) and its `--camera` hook needs the
  calibration above. A live run would show the word `crosspad` walking across the pads;
  nobody has seen it do that yet.
- **The kit animation pair (`0x08`) is one frame, but the load is still a card operation**
  of seconds. A host that writes kit pairs rapidly gets newest-wins, and the refusal
  reasons are the ones `KIT_PC` already reports (`off`, `channel`, `out_of_range`,
  `no_kits`, `not_loadable`).
- **A pad at `pad mode = 1` stays painted forever if its host disappears without saying
  anything.** That is what "hold" means, and it is the one way to make this surface
  unbounded on purpose; it is per pad, not global. A panel that wants safety and
  persistence should repaint, not hold.

## Where the tests are

- `main/tests/test_surface_map.cpp` (100 assertions, 8 cases, host, in the tree's Catch2
  harness): the frame grammar and every way a frame can be wrong; the byte counts against
  built frames; the small sections' widths; the give-back rule including the
  taken-over pad; the sections refused a restore; the parked action's layout and its
  round trip through bytes.
- The bench: `.strata/tools/sysxsurface.py` -- **40 checks, 0 failed**, exit 0, on
  `dev_8dda` (v2, PCB r20) against the image built from this tree (0x34f930, flashed and
  re-hashed by `.strata/tools/flash_pc.py`) -- palette load and a refused out-of-range
  load, painting by index and the record that makes a give-back possible, a four-wide block
  painting four pads and no fifth, four honest hand-backs (counted as a delta, because the
  board's counters survive a boot and an absolute zero is a claim about the bench's
  history), a pad the other door took over (seen, refused to restore, counted as a skip), a
  held pad surviving the lease timeout while its unheld neighbour was given back, direct
  colour, the board's own animations switched on and off by SysEx, the lease timeout
  written by SysEx and put back by the verb, the kit pair loaded from the map's own row
  plus the refused undo, the Line-out level moved/undone/left-as-found with two independent
  read-outs agreeing (`SURFACE LVL hw 2` and `OUTVOL`'s out1: 30 after the write, 64 again
  after the hand-back), the pad-mix no-read-back answer stated out loud, five refused
  frames with nothing painted, and the status reply heard on the wire (15 bytes, its
  counters equal to the verb's). The run printed two notes rather than verdicts, which is
  the right shape for both: the optical section was skipped, and the level take-over could
  not be demonstrated because the desk's verb does not move what this surface writes.
- Offline for the harness side: see `crosspad-hil`'s `feat/midi-host-surface-catalog`
  branch -- the catalog rows for these subs and the `SURFACE` verb, which
  `tests/test_knowledge.py` pins against the firmware's own list.

## Publishing note

`docs/subsystems/*.md` are published to https://crosspad.app/docs/subsystems/ ; the site's
listing needs a row for this page, and the manual's chapter 17 paragraph about panels now
points here. Both are crosspad-docs changes, not a code change (see the note at the top of
this file and `.strata/notes/`).
