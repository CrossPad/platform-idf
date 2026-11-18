# DAW control

Contributor notes: the DAW protocol, FL Studio, Ableton, REAPER, the MIDI hub. The short version of what a change in this area
must not break; the pages it links to have the detail.

## DAW Control (FL Studio, Ableton)

One app (`components/crosspad-dawcontrol`), one device-shaped SysEx protocol
`F0 7D 1E sub … F7` (`CROSSPAD_CMD_DAW`; catalog of record: crosspad-hil
`knowledge/sysex.yaml` `daw`): pads send `base_note+pad` notes + aftertouch,
the host paints LEDs (`led`/`led_all`), three LCD rows (`text`), `beat`,
`transport`, `mode`, `config`. The firmware knows no DAW; each DAW gets a
self-contained script in the app repo (`host/fl_studio/device_CrossPad.py`,
FL's embedded Python has no pip). Host frames reach the app through
`PlatformServices::stm32MessageHandler->setDawCallback()`; replies go out via
`IMidiOutput::sendSysEx`. The crosspad-hil MIDI hub (systemd --user, from
login) is the host endpoint: every 2 s it sends `HOST_INFO` / `FW_LATEST` as
companion notify frames (`0x1F 04`, run and never answered) — status-bar USB
glyph, DAW Control host row, persisted "update available" in Settings → Info
(`main/host_info.cpp`, `docs/subsystems/ble-midi.md`); the same report's
`daws=` says which DAWs the hub can start, and DAW Control then offers
**Launch FL Studio / Ableton / REAPER** while no script answers (`0C launch`
back to the hub, `crosspad_hil/daw_launcher.py`). REAPER's script is
`host/reaper/CrossPad.lua` (`crosspad-hil reaper install`); REAPER opens raw
MIDI only, so on Linux it talks to a `snd-virmidi` card the hub bridges. Encoder is not the app's: firmware emits CC 1
(detent 0..15) / CC 64 globally. Kit sync is host-side only
(`crosspad-hil kit push <dir>`), never from inside the DAW. Test:
`crosspad-hil run daw_control`. Design: `docs/design/2026-09-06-daw-control-design.md`.
**FL under Wine:** one open MIDI input at a time, a board reboot or script
reload leaves it allocated but unlinked (relink with `aconnect`, then restart
the app so it hellos again), and Wine routes MIDI to the device by ALSA source
address — a host tool cannot inject into FL from its own client. FL colour
ints are `0xRRGGBB`; `mixer.getCurrentTempo()` is milli-BPM. `SYSEX_ECHO 1`
(CDC, off at boot) makes the board bounce foreign-manufacturer SysEx back to
the host; that is how the `flstudio` MCP (fl-studio-mcp bridge, 0x7C, piggybacked
on the CrossPad script) reaches FL. FL script Python has no sockets, threads
or file I/O. **A mixer track's audio input is not reachable from the script
API** (no `mixer` call, no REC event — probing that id range crashes FL): the
mixer only ever shows the driver's "In 1 - In 2", which is `[Default input]`
= the CrossPad Microphone node. `crosspad-hil fl template install` writes an
FL project template (Insert 1 "CrossPad", input pair 0) into the Wine
prefix's `Projects/Templates`; File → New from template → CrossPad.
