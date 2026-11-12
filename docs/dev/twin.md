# The browser twin and the live stage

Contributor notes: input mirroring, the twin protocol. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Browser Twin and Live Stage

The board mirrors its LVGL inputs (`main/twin_mirror.cpp`, SysEx `0x1F 09`) and
its IMU (`main/motion_stream.cpp`, `0x1F 0A`) to a host page while one keeps
asking (`TWIN 1` / `MOTION 1` notifies): the web twin (this firmware in
WebAssembly, crosspad-web-twin) and mucha's live stage. **A twin is only right
when built from the sources the board was flashed from**: `TWIN_HELLO` answers
the protocol (`twin_state.h kProtocol`) and the source id
(`tools/twin_source_id.cmake`, run by both builds), and the twin compares.
Changing `twin_state.h` means a reflash and a twin rebuild; a new `TWIN_STATE`
field goes at the end, within the 470-byte line. Frames, verbs, rules:
[docs/subsystems/twin-protocol.md](../subsystems/twin-protocol.md).
