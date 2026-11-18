# BLE MIDI and WiFi

Contributor notes: NimBLE, the companion service, WiFi leases, network mode. The short version of what a change in this area
must not break; the pages it links to have the detail.

## BLE MIDI

Browser companion control shares NimBLE: custom GATT service `7e400001-b5a3-f393-e0a9-e50e24dcca9e`, bounded request/reply mailbox, main-task HIL dispatch. The same verb set rides USB MIDI SysEx `0x1F` (`main/companion_sysex.cpp`, `crosspad-hil companion "<VERB>"`) — that is the control plane of the MIDI+UAC2 profile, which has no CDC. A host's request for the CDC profile (SysEx `0x1B 01`) is confirmed **on the device** unless `USB_GUARD 0` (persisted; a development board sets it once). Protocol and limits: `docs/subsystems/ble-midi.md`. No files or OTA over BLE or SysEx.

NimBLE, both roles, routed through the same `MidiInputHandler` as every other
transport (`components/crosspad-platform-idf/EspBleMidi.{hpp,cpp}`, wiring in
`main/midiCallbacks.cpp`). Lazy-started after boot per `wireless.enableBLE`.
**Cost: ~40 kB of internal RAM while running, plus ~36 kB of static DRAM** — with BLE up the launcher sits
at ~8 kB free, which is why `v1.0.0-rc1` ships it off and why anything that needs
an internal stack (audio tasks, `fx_worker`) must be checked against `MEM` /
`MEM_BLOCKS`. Off-device: `BLE_ENABLE 0|1` (persisted — `crosspad-hil ble enable off`
puts a development board in the rc1 shape), `BLE_START|STOP|STATUS|SCAN|CONNECT|SEND|TXOFF`.
Design, memory measurements, status-bar fixes: [docs/subsystems/ble-midi.md](../subsystems/ble-midi.md).

## WiFi (on demand; network mode as the fallback)

Nothing turns WiFi on: a caller holds a lease (`wifi_link_acquire/release`,
`main/wifi_link.cpp`). **A board paired to a crosspad.app account with Remote
management on (the default) holds one all the time** (`"remote"`,
`main/cp_remote.cpp`, while WiFi is allowed), so its WiFi stays up and BLE off;
turning BLE on in Settings turns Remote management off. The first lease starts the radio and joins the saved
networks in order; the last one arms a 30 s linger, then the radio stops and
its RAM comes back. One control task (`wifi_ctl`, internal stack) makes every
`esp_wifi_*` call; BLE gives way while WiFi runs and comes back after.
**Four checks on three heaps run before anything is built** (internal
`DEFAULT` 10240 B, all internal 48128 B, PSRAM 116736 B, a free PSRAM block of
20480 B; `radio_fits()`), set from a connected link's measured peak. They hold
in a normal boot because LVGL's blend code left IRAM (Build Gotchas). **A
Sampler with a kit is refused by design** (`failed(no_mem)`; measured with the
earlier fixed engine arena, not re-measured with the dynamic one): the user-facing request then offers **network mode**
(`main/net_mode.cpp`, `wifi_fallback_offer()`), a boot with no engine, kits or
BLE and its own screen; background callers and verbs answer `ERR no_mem`.
Verbs: `WIFI_STATUS|ENABLE|SET|LIST|FORGET|UP|DOWN|SCAN|PING`, `NET_MODE
1|0` (CDC-only); `WIFI_SET` saves first and answers `ERR short_pass` under 8
characters. Saved networks live in NVS `wifi/nets`, shared by both slots.
Testing: `WIFI_ENABLE 1` first; the test network comes from `CROSSPAD_WIFI_SSID`
/ `CROSSPAD_WIFI_PASS` in the environment (keep credentials out of every repo),
then `python3 tools/bench.py test wifi`. The TLS client costs a 64 kB PSRAM page: run
`python3 tools/check_image_budget.py --rev v2` after touching the network code.
Details, numbers, limitations, traps: [docs/subsystems/wifi.md](../subsystems/wifi.md).
