# SysEx control surface: why addresses, and where the surface stops

Date: 2026-11-19. Branch: `feat/midi-program-change-kits` (local, not pushed).
Protocol and numbers: `docs/subsystems/surface-sysex.md`. This note keeps the
decisions and the open questions; the other page is what a host author reads.

## The request, in the words it arrived in

"what I want is a support of program change messages…" (delivered earlier on this
branch), then: SysEx "to control the pads like on the launchpad pro", a second SysEx
"to bring the LEDs back into the previous state before changed (in case there are not
updated by the crosspad firmware in meantime)", ".md is required to explain", a python
slide-show writing "crosspad" on the 4×4 grid, and -- "if not completed yet" -- the
volumes too, "following the roland typical implementations where you have something like
writing to different addresses. or when better use NRPN or similar. it would make sense
to keep the midi traffic efficient".

## Decisions, and the reasoning under each

**Address space, not more Control Changes.** A CC surface has to be *told* which number
means what, one parameter at a time; that is the right shape for a DAW automation lane and
the wrong shape for a grid of pads. The address map is the same parameter set the CC map
already points at (`Target::Channel/Output/Hardware/Pad`), plus the pad-LED state that has
no CC number space at all, so a host is not choosing between two representations of one
value. Blocks are what make it cheap: sixteen palette indices in one 24-byte frame.

**Palette + indices as the primary pad-colour path.** Direct RGB is available
(`0x12`, 55 bytes a grid) but 7-bit per component and three times the bytes for the same
result on screen. A control page repaints a grid constantly and changes its colours by
name, so the palette is loaded once and referenced by index afterwards -- which also makes
a colour walk free, because the grid frame already carries the indices.

**Give back only what is still ours.** This is the rule the request asked for in
parentheses, and it is a decision rather than an implementation detail: an app lights a
pad for what is playing, so "restore the previous colour" without asking whether the board
has since said something to the player is a surface talking over its own instrument. The
comparison happens in the board's own units (percent/float, not the wire's 7 bits) because
a hardware output's value goes through a coarse percent step on the way in and would come
back a step off, which would report "taken over" for a value nobody touched.

**Per-pad hold instead of a global sticky lease.** Yesterday's bounded lease (5 s of
silence and the board's colours come back) protects a board from a dead host; a Launchpad
panel legitimately wants a mode view to stay. `pad mode = 1` is per pad, so it keeps the
default safety for everything else. A held pad keeps its claim record even when the lease
expires -- the first version cleared it, which made the later explicit hand-back a no-op;
the bench caught that.

**Lease covers the surface too.** SysEx-painted pads are host activity, so they take the
lease. Without that, the exact failure the lease exists to prevent (a dead animation host
leaving a board painted) reappears through the second door. Caught while writing the bench,
before the bench ran.

**Sections with no honest answer are refused by the grammar.** `pad mix` (no getter in
`IPadMix`), `lease` (a timeout has no previous value a host could mean), `palette` (the
surface never saw the table it would be putting back) and `kit` (undoing a kit means
loading the previous one, which needs the kit listing, which the UI task owns while a
restore runs on the worker) are refused as `SEC_RESTORE` targets. Accepting a frame and
doing nothing is the failure mode a host cannot debug from the outside.

**NRPN is kept for what it is good at, and said so.** Four messages per parameter and 14
bits is not a colour, so it loses for a grid; it wins for one fader a person automates,
because it needs no vendor framing and lands in an automation lane. The CC map is the
board's NRPN-equivalent door today (plain CCs, 7-bit); a 14-bit NRPN layer over the same
targets would be a change of range, not of architecture.

## What the bench taught, that the design had wrong

- The mixer's own output-bus level is held at unity by the board's design for any output
  that has a hardware level of its own, so writing section `0x05` on this board is
  accepted, recorded and then re-asserted away. The surface's `SURFACE LVL` read-out is
  what showed it: `wrote=617` next to `now=1000`. Kept as a documented limit rather than
  made to look like a success.
- `IOutputLevels` index 0 is the built-in speakers on v2 (the list is speakers, phones,
  line out -- `OutputLevelsPort.cpp`). A "careful" choice of address in the bench had
  picked address 0 thinking it was Line out. The bench now uses address 2 and says so.
- The counter `values` counted every value the surface reached for, not the ones that
  landed. Split into `values` / `missed`. A counter that answers "anything" when the
  question is "did it do anything" is worse than no counter.
- `OUTVOL` (the audio-router's tracked state) and `IOutputLevels` are not the same
  numbering, and comparing one against the other let a check pass while nothing had moved
  -- the check that should have caught the wrong-address mistake above.

## Open questions for a person to decide

1. Is `0x05` (mixer bus) worth keeping, given it is inert on this board's routing? It is
   cheap and correct on a hypothetical routing where a bus has no hardware level; keeping
   it means a host can write into a place that does nothing. Currently: kept, documented,
   and its inertness asserted in the bench.
2. Should a host that holds a pad (`pad mode = 1`) be required to say so again every N
   seconds? A panel that dies mid-mode-view leaves that pad painted forever, which is the
   one thing this surface allows on purpose.
3. Should BLE MIDI carry the same address surface (the frames are SysEx, so the transport
   mostly does the work) -- and does the audio profile's no-CDC control plane (`0x1F`)
   want the `SURFACE` verbs, which are still CDC-only?
4. Is there a setlist use case for undoing a kit from SysEx? Refused for now (threading);
   a posted restore on the UI task would make it possible, and it is written in
   `.strata/notes/midi-ideas-next.md`.
5. Should a surface write to a level a CC rule also maps re-baseline that rule's echo?
   Today the echo treats it as a change the board made, so the host hears its own value
   back once. Harmless, but a host author will ask.

## Not delivered in this increment, said plainly

- Optical certification of the pad colours: the bench's three-frame webcam section exists
  and is skipped when there is no calibration; `cam.py calibrate` did not accept its
  lattice in the dark this evening, so the pads' *measured* colour is unproven this
  session. The LED-model read-outs (which is what the give-back rule turns on) are proven.
- A live run of `stratatest/tools/ledshow.py` against a board: it is written, its
  `--list` and `--dry-run` work (the 3×4 font, 35 scroll steps, 899 bytes, ~25 bytes per
  grid frame), and nobody has yet watched the word move across real pads.
- Settings → MIDI rows for the surface (still console-only, like the CC map and the LED
  lease), and the companion allowlist (`0x1F`) so the audio profile's hosts can reach
  `SURFACE`/`MIDI_CC`/`LED_HOST` verbs at all.
