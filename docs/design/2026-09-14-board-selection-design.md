# Board selection in one place

2026-09-14. Status: approved, implementing.

## Problem

The board revision a build targets is decided in eight places that disagree:

| Consumer | Today | Result |
|---|---|---|
| CP Tools (TUI) | `idf.py build` → `build/` + `sdkconfig` | a v1 image without BLE |
| crosspad_build / crosspad_flash (MCP) | `idf.py build`, default `build/` | same |
| ESP-IDF VS Code extension | `build/` | same |
| bench.py, factory_provision.py | `build_v2` hardcoded | v2 |
| ota_flash.py | newest `build*/` | whichever was built last |
| CI | matrix v1 + v2 | both, explicit |

That can put a v1 image on a v2 board: it then reboots every ~19.5 s
(udev: `303a:3456` → ROM `303a:1001` → `3456`), CDC enumerates and never
answers. Nothing on the host said "wrong revision", because nothing on the
host knows the revision of the board plugged in. A user may own several
CrossPads of different revisions, so the answer has to come from the board.

## Two signals, from two chips

| Signal | Source | Answers |
|---|---|---|
| hardware | STM bridge (`0483:5740`) `bcdDevice` = `0x3000 + BCD(PCB)` from OTP, e.g. `0x3020` = PCB 20 | which board is this |
| firmware | ESP (`303a:3456`) `bcdDevice` = `0x0500 \| rev << 4 \| profile`, e.g. `0x0521` = v2, CDC profile | what is flashed |

- Hardware wins. PCB → build revision (16, 19 → `v1`; 20 → `v2`) is one table,
  crosspad-hil `PCB_REVS`; the resolver consumes its `board_rev`.
- A legacy STM image reports `0x0200` (no PCB) and a legacy ESP image reports a
  zero revision nibble: both mean "unknown", never a guess.
- Mismatch (hardware v2, firmware v1) is shown in CP Tools and blocks a flash of
  the wrong image in ota_flash.py, bench.py and crosspad_flash unless forced.
- ESP USB serial = the chip MAC in the ROM's format (`44:1B:F6:xx:xx:xx`) instead
  of the constant `123456`, so several boards are distinguishable and a board has
  one identity in download mode and in the app.
- `APP_VERSIONS` gains a `BOARD: fw=v2 pcb=20` row for paths without sysfs
  (Web Serial, SysEx companion).

## Resolver

`tools/crosspad_board.py` — one question: which revision do I build and flash
for. Returns revision, the source of the decision, `build_<rev>`, `sdkconfig.<rev>`.

Order:
1. `--board v1|v2` given explicitly.
2. The connected board: hardware revision, else firmware revision
   (`crosspad-hil devices --json`, which stays the only owner of device I/O).
3. `"board"` in `crosspad.local.json` — the last choice (gitignored, per developer).
4. Nothing: CP Tools asks (v2 preselected); non-interactive callers exit 2.

Every successful decision (explicit or detected) is written back to
`crosspad.local.json`, so a build with no board attached uses the last one.

Several boards: same revision → no question; different revisions → CP Tools
lists them, non-interactive callers need `--device` or `--board` (exit 2 with the
list otherwise). A flasher that already holds a port resolves for the board
that owns it.

Entry points: `idf.py board` (show decision + source), `idf.py board --set v2`,
`python3 tools/crosspad_board.py --json` (for crosspad-mcp).

## Consumers

1. **CP Tools** — dashboard row `Board: v2 · dev_xxxx PCB 20 · fw v2 ✓`, `[R]`
   to change; red on mismatch. Build & Flash, clean build, Quick OTA and
   `get_build_info()` use `build_<rev>` + `sdkconfig.<rev>`. The Configure
   screen's `CROSSPAD_BOARD` row reads the resolved sdkconfig.
2. **VS Code** — choosing a board writes `idf.buildPath` and
   `idf.sdkconfigFilePath` into `.vscode/settings.json`, so the ESP-IDF
   extension's build/flash/menuconfig follow.
3. **crosspad-mcp** — `crosspad_build platform=idf` takes an optional `board`,
   default from the resolver, builds with `-B build_<rev> -DSDKCONFIG=sdkconfig.<rev>`;
   `crosspad_flash` defaults to the resolver's build dir; the existing
   `BOARD_REV_MISMATCH` guard gets real data.
4. **bench.py, ota_flash.py, factory_provision.py** — the resolver for the
   connected board replaces "newest `build*/`" and the hardcoded `build_v2`.
5. **Bare `idf.py build`** (default `sdkconfig` in the project root) is a hard
   CMake error naming `idf.py board` and `-B build_v2 -DSDKCONFIG=sdkconfig.v2`.
   CI already passes `-B` and is unchanged.

## Edge cases

- MIDI+UAC2 profile or download mode: `bcdDevice` is a descriptor, readable
  without any protocol. The ESP revision is read from the CDC tty's USB
  device, though, so in that profile (no CDC) only the STM hardware signal is
  seen — firmware revision is unknown until the board is switched to CDC.
- Detection reads sysfs (`ID_REVISION` / `bcdDevice`), Linux only for now; macOS
  and Windows fall back to memory and the prompt for builds. Deliberate limitation.
- Board connected but its revision unidentified (no STM PCB signal and no ESP
  revision, the port not listed by crosspad-hil, crosspad-hil missing): no
  memory fallback for flashing — memory names no particular board. ota_flash.py
  switches the board to CDC first, resolves for the device owning that port,
  and otherwise refuses (exit 2) until the user names the revision with
  `--board` (or forces).
- The ESP serial change (chip MAC instead of the constant `123456`) did **not**
  rename the crosspad-hil device id — that identity comes from the STM
  bridge's serial, which was already unique per board.

## Tests

- Resolver: pytest in `tools/tests/` over fake device lists — source order,
  several boards, write-back, exit 2.
- crosspad-hil: sysfs fixture → `board_rev`, `fw_rev`.
- crosspad-mcp: existing build/flash tests with a stubbed resolver.
- CMake: bare `idf.py build` fails; `-B build_v2 -DSDKCONFIG=sdkconfig.v2` passes.
- Bench: sysfs shows `bcdDevice 0x0521` and serial = MAC (same as ROM); CP Tools
  shows `v2 ✓`; `ota_flash.py build_v1/CrossPad.bin` is refused (no v1 flash, no
  bootloop); after the STM r20 update the bridge shows `0x3020`.

## Delivery

| Stage | What |
|---|---|
| 1 | host: resolver, CP Tools, VS Code, bench/OTA/factory, CMake guard, MCP — works on memory alone |
| 2 | ESP firmware: revision in `bcdDevice`, MAC serial, `BOARD:` row |
| 3 | crosspad-hil: sysfs → `board_rev`, `fw_rev` |
| 4 | STM r20: PCB in `bcdDevice`, shipped by the in-field update |
