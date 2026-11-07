# Launch the DAW from DAW Control, through the hub

## Goal

DAW Control with the hub alive but no DAW script answering shows a
**Launch FL Studio** / **Launch Ableton** button for each DAW the hub can start.
A DAW that is already running without its script answering is named as such
instead ("FL Studio running - script not answering") — a second launch would
change nothing. The DAW opens whatever it opens by itself (last project or
empty); the hub never touches projects.

## Wire

- Hub → board: `HOST_INFO` gains an optional trailing token
  `daws=<installed>/<running>`, two bitmasks indexed by `daw_id` (bit 1 FL
  Studio, bit 2 Ableton). Older firmware parses five fields and ignores the
  tail; an older hub sends no token, so the board shows no button.
- Board → hub: DAW SysEx sub `launch` = `0x0C <daw_id>`
  (`F0 7D 1E 0C id F7`). The hub consumes it and never forwards it to the DAW.

## Board

- core `CrosspadStatus::hostDawsInstalled` / `hostDawsRunning` (core 1.23.0),
  set from `HOST_INFO` in `main/host_info.cpp`; valid while `hostAlive()`.
- DAW Control, `hostAlive && !connected`: one button per installed, not
  running DAW; text for a running one; after a press "Starting <DAW>..." until
  a hello, the running bit, or 90 s. Hub dead: the old "no host" text.

## Hub (crosspad-hil)

- `daw_launcher.py`: FL = `Image-Line/FL Studio*/FL64.exe`, Ableton =
  `ProgramData/Ableton/Live*/Program/*.exe` in the Wine prefix
  (`fl_template.default_drive_c()`); running = an exe name in
  `/proc/*/cmdline`; launch = `wine <exe>` in a new session.
- No graphical session in the service environment (`DISPLAY` /
  `WAYLAND_DISPLAY`) → nothing is launchable (mask 0), not a silent failure.
- A second `launch` for the same DAW within 30 s is ignored.

## Test

pytest for detection, masks, interception and the 30 s guard; build v1 + v2;
`HOST_INFO … daws=2/0` over CDC shows the button without a hub;
`crosspad-hil run daw_control`; by hand: FL closed → press → FL up → hello.
