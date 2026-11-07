# WiFi on demand (sub-project A)

Date: 2026-10-02. Status: implemented (Revision 2).

## As built — deviations from this design

What the code on `feat/wifi-on-demand` does where it differs from the text
below. The sections below stay as designed; where they disagree with this
list, this list and `docs/subsystems/wifi.md` win.

**History.** Revision 1 (same day) moved WiFi into a network-mode boot after
the first measurement showed the radio could not start beside the sample
engine (launcher 31 kB internal free). Experiments then showed the LVGL
software blend code in IRAM (~68 kB) and three unused renderers (41 kB) were
the cause; Revision 2 shipped both sdkconfig changes (launcher 31 -> 99 kB
internal, Settings scroll ~4 % slower), put WiFi back on demand in a normal
boot, kept network mode as the fallback for a refused user-facing request, and
dropped the reclaim list.

**Deviations decided while planning**
- Saved-network list logic and blob format live in crosspad-core
  (`crosspad/net/WifiNetworkList`, unit-tested); `main/wifi_creds.cpp` only
  reads and writes the blob.
- `WIFI_SCAN` / `WIFI_LIST` answer one line each (`WIFI_AP: count=N | ...`), not
  one row per network.
- `WIFI_ENABLE 0|1` exists (persisted); `WIFI_STATUS` gains `reason=`, `heap_*`,
  `int_*`, `psram_*`, `stack_free=`, `enabled=` and quotes the SSID.
- Network mode keeps only the WiFi tab (R12), holds a lease for the whole boot
  (R13), boots the CDC profile and skips UAC2 (R16), `NET_MODE` is CDC-only (R18).
- `WifiFail::NotNetworkMode` stays in core 1.39.0, never produced (removing it
  would be MAJOR); `CrosspadStatus::wifiReclaimed` was removed (R19);
  `wifiBarsLook()` lives in core.
- The network stack is built on the control task (R23); a network picked on
  the device is saved only once it has an IP, `WIFI_SET` saves first (R14); a
  scan brings the radio up without a connect and pauses one in progress (R15).
- No "Manage network" row in Settings; the ways into network mode are the
  `no_mem` fallback (`wifi_fallback_offer()`, once per page visit), `NET_MODE 1`
  and sub-projects B/C. The page holds a lease only in Settings.
- `WIFI_SCAN` / `WIFI_PING` / `WIFI_UP` / `WIFI_DOWN` are CDC-only; the
  companion and SysEx answer `ERR use_cdc` (R2; UP/DOWN after the final
  review, since over BLE they strand the radio up and BLE off).
- The factory reset forgets saved networks through
  `IGuiPlatform::onFactoryReset()` → `wifi_factory_reset()`, not
  `NvsSettingsStore::eraseAll()` (unused, and blind to the RAM copy);
  `WIFI_FORGET_ALL` runs the same step from a host.
- A BLE start while the radio is held is deferred to the radio's stop
  (`OK deferred=wifi`), under one lock with every BLE start and stop.
- `rt_glitch` with the link up runs in a normal boot (R17 superseded).
- The gui gets `ui_text_prompt` (on `getOverlayParent()`, R20) and
  `ui_wifi_bars`; gui 1.19.0, core 1.39.0.

**Rulings that changed behaviour**
- R3: the 200 ms delay before BLE stops is implemented transport-agnostically,
  so a reply over BLE leaves first.
- R11: the memory gate runs before the first-lease network stack is built
  (`tcpip_init()` asserts on a failed allocation).
- R16: network mode boots CDC, no UAC2; the stored USB setting is untouched.
- R23: network stack on `wifi_ctl`, so the LVGL and main-task stacks never carry it.
- R24 / R36: the gate is the measured peak of a connected link plus a TLS ping,
  plus 8 kB, rounded to 1 kB, on three heaps: internal `DEFAULT` 10240 B, all
  internal 48128 B, PSRAM free 100352 B, and a free PSRAM block of 8192 B.
  Refused = `failed(no_mem)` before anything is built; a Sampler with a kit is
  refused by design (12-28 kB of PSRAM).
- R32: the lazy network-stack build moved to the top of `radio_start()`, so every
  start retries it.
- R33 / R34: mbedTLS trimmed to a TLS client (client-only, five-root custom
  bundle, ECDHE only, three curves) and `wifi_ping` on esp-tls, not
  `esp_http_client`: the TLS client costs one 64 kB PSRAM page, not three
  (`.ext_ram.dummy` 2949088). GlobalSign Root CA expires 2028-01-28.
- R37: `wifi_ping` uses the last error only if positive; the BLE-running
  first-lease path checks the largest PSRAM block before building the stack.
- R38: `ui_text_prompt` cleans up like `finish()` when its overlay is deleted
  from outside (contract: the callback runs exactly once).
- R39: the engine arena shrank 4848 -> 4608 kB (TLSF 128 kB size classes) to
  give the UI and the radio PSRAM; app_churn passes again and launcher WiFi
  passes its PSRAM gate with margin.
- R41 / R42: the keyboard's first key is highlighted on open and the eye is
  reachable by encoder; OK on an empty password for a secured network keeps the
  prompt open; a WPA passphrase of 1-7 characters is refused (`ERR short_pass`,
  and the prompt asks again).
- R44: the ping's TLS bound is 20 s per handshake, read and write (10 s turned a
  stalled cold-start handshake into a timeout); `WIFI_PING`'s `ERR` carries
  `tls=<mbedTLS code>`.
- R45 (known limitation): on a test AP a cold radio start occasionally stalls
  the first TLS handshake for seconds (late ACKs, CPU idle); root cause open,
  needs a second AP, revisit with sub-project B.
- R47: on the integrated image (origin/main a817e4c, +64 kB of PSRAM image) a
  start that passed with 100.3 kB free lost its TLS ping, so the PSRAM gates
  are now 116736 B free (peak 91792 + 24 kB) and a 20480 B block (mbedTLS's
  ~16.7 kB input buffer for a full record + TLSF rounding); `test wifi` starts
  from the launcher, and a Sampler with a kit (~70 kB) is refused by design.
- `WIFI_UP` answers `OK`, and a start the gates refuse shows as
  `failed(no_mem)` in `WIFI_STATUS` (the gates run on the control task).
- Also: a failed scan has no `WifiFail` value (`ERR timeout` / `ERR scan`);
  SSIDs in replies are escaped and an SSID with `"` cannot be sent by
  `WIFI_SET`.

**Measured.** Radio start + connect + TLS ping: ~39 kB internal (almost all
from the DMA/stack pool), ~92 kB PSRAM; 11.4 kB stays after the stop; up/down
cycles leak nothing. The test AP's early "rejected at auth" was a wrong
saved password (reason 202; on a WPA2/WPA3 AP the SAE auth step is the
password check). `bench.py test wifi` passes 6/6; `rt_glitch` is clean with
and without the link. Traps found on hardware are in the subsystem doc.

**Errata (R40).** Section 1 said "the engine's 4848 kB arena"; corrected in
place to 4608 kB, the size since R39.

## Revision 2 — WiFi on demand in normal mode again; network mode as the fallback (supersedes Revision 1 where they differ)

Measured on v2 (2026-10-02): turning off
the LVGL renderers nothing draws (`LV_DRAW_SW_SUPPORT_AL88`, `_L8`, `_I1`; RGB888
stays — `LV_COLOR_DEPTH=24` draws opaque layers in it) and
`LV_ATTRIBUTE_FAST_MEM_USE_IRAM` moves the heap start by 67.8 kB: launcher
99.3 kB internal free, Sampler + kit 95.9 kB (64.7 kB with BLE running),
LVGL render cost +4 % (Settings worst frame 90 → 94 ms). The radio's start +
connect attempt peaks at ~56 kB and leaves ~17 kB allocated after it stops.
The user chose:

- **Both sdkconfig changes ship** in `sdkconfig.v1`/`sdkconfig.v2`, and
  `sdkconfig.defaults` stops forcing `FAST_MEM_USE_IRAM` (it wins Kconfig
  choices over `sdkconfig.v*`).
- **WiFi is on demand in normal mode** (the original §1 lease model): the
  lease API works in normal mode again; `not_network_mode` is no longer
  returned. The memory gate is raised above the measured ~56 kB peak (set by
  measurement of a connected link, with margin).
- **Network mode stays as the fallback**: when a user-facing request (Settings,
  store, update, scan) is refused with `no_mem`, the dialog offers *Restart in
  network mode* (on the matching tab) instead of the old reclaim list. Background
  callers never do this. Network mode keeps its screen and tabs.
- **Restored from the original design:** Settings → WiFi in normal mode with
  scan, password keyboard, saved networks, test connection (§3); the status-bar
  WiFi icon in normal mode (§3 table); `WIFI_UP/SCAN/PING` in normal mode (§4).
- **Still dropped:** the reclaim dialog and `WIFI_UP RECLAIM`.

## Revision 1 — WiFi runs only in network mode (superseded in part by Revision 2)

Measured on a v2 board (docs/subsystems/wifi.md): with the WiFi driver linked,
WiFi IRAM optimisations off and nothing WiFi-related created at boot, the
launcher has 31.4 kB of internal RAM free; the network stack takes 10.7 kB on
the first lease and keeps it; ~15.6 kB is left (12.4 kB in the Sampler with a
kit) — the radio cannot start. Stopping the audio engine frees ~4.3 kB. The
user chose a boot mode over chasing the RAM:

- **Network mode** is a boot mode, shaped like USB Transfer mode
  (docs/subsystems/usb-transfer-mode.md): a one-shot `RTC_NOINIT_ATTR` flag +
  `esp_restart()`, honoured only after `ESP_RST_SW`, so a cold boot, a panic or
  a watchdog always comes back normal. That boot brings up STM, display, LVGL,
  USB (profile unchanged), the SD card with FATFS, and WiFi — and starts **no
  sample engine or arena, no kit scan or loader, no BLE, no SD hotplug, no
  sequencer, no RT mixer**. Entering: the Firmware app's Store/Updates tabs
  (sub-project C), Settings → WiFi → *Manage network*, Settings → Updates →
  *Check now* (B), and `NET_MODE 1` over CDC. No SysEx or BLE entry point.
  Leaving: *Exit* (restart into normal mode) or, after an install, *Run the new
  version* (restart into the new slot via `fw_switch_request()`).
- **The network-mode screen** replaces the launcher on that boot: a header with
  the WiFi bars (the §3 status-icon states) and the network name, tabs **WiFi**
  (scan, password keyboard, saved networks, test connection), **Store**,
  **Updates** (C's views), and an *Exit* reachable by touch, encoder and power
  click.
- **Normal mode never starts the radio.** The lease API refuses there with a
  new fail reason `not_network_mode`; `WIFI_SET`/`WIFI_LIST`/`WIFI_FORGET`
  still edit the saved networks; `WIFI_UP`, `WIFI_SCAN`, `WIFI_PING` answer
  `ERR not_network_mode`. Settings → WiFi in normal mode shows *WiFi allowed*,
  the saved networks (Forget) and *Manage network* (enters network mode) — no
  scan.
- **Dropped:** the reclaim dialog and `WIFI_UP RECLAIM` (§1 "Reclaiming memory"),
  the status-bar WiFi icon in normal mode (§3 — the bars live in the
  network-mode header instead), the Sampler-with-kit measurement (§6 — the
  measurement that sets the gate is taken in network mode).
- **Kept unchanged:** the leased link and its failure rules (§1, §5), BLE
  arbitration (a safety net — BLE does not start in network mode), saved
  networks (§2), `ui_text_prompt` (§3), the verbs (§4) with the refusals above,
  "nothing restarts the board", the tests of §7 run in network mode (step 4 BLE
  becomes "BLE stays off in network mode"; step 6 reclaim is dropped).

## Goal

The CrossPad can join a WiFi network, remember it, and give other subsystems
(update check, store, downloads — sub-projects B–D) an HTTPS-capable link for
as long as they need it, then switch the radio off and return the memory. A
user sets it up on the device itself (scan + on-screen keyboard), from the
browser companion, or a bench sets it over CDC.

Done means: `WIFI_SET` → link up in under 10 s → `WIFI_PING` reaches
crosspad.app over TLS; 20 up/down cycles leave internal RAM where it started;
BLE comes back after WiFi; audio shows no new glitches with the link up.

## Where things stand today

- The WiFi driver is linked (`CONFIG_ESP_WIFI_ENABLED`, `main/main.cpp`
  includes `esp_wifi.h`, ESP-NOW calls `espnow_storage_init()`), but
  `esp_wifi_init()` is never called anywhere — WiFi has never run on this
  firmware, and its internal-RAM cost is unmeasured.
- `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS`, `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`
  and `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` are on. `MBEDTLS_HAVE_TIME_DATE` is
  off, so certificate dates are not checked and no SNTP is needed for TLS.
- `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is on; `ESP_WIFI_IRAM_OPT` and
  `ESP_WIFI_RX_IRAM_OPT` are on.
- `WirelessSettings::enableWiFi` exists in core (NVS `cfg_wireless/wifi`),
  read and saved, used by nothing.
- BLE costs 30–60 kB of internal RAM and leaves the launcher at ~8 kB free;
  `bleMidi_start()` (`main/midiCallbacks.cpp`) refuses below a free-RAM gate
  instead of letting NimBLE assert.
- `CONFIG_NVS_ENCRYPTION` is off: a saved WiFi password sits in NVS in clear,
  as on most ESP32 products.
- Companion verbs reach `hil_control_companion_handle()`, so a verb added to
  `main/hil_control.cpp` answers on CDC, BLE companion and USB MIDI SysEx
  `0x1F` alike.

## 1. Link: `main/wifi_link.{h,cpp}`

A leased link. Nothing turns WiFi "on"; callers hold a lease.

```c
typedef enum { WIFI_LINK_OFF, WIFI_LINK_STARTING, WIFI_LINK_CONNECTING,
               WIFI_LINK_UP, WIFI_LINK_STOPPING, WIFI_LINK_FAILED } wifi_link_state_t;
typedef enum { WIFI_FAIL_NONE, WIFI_FAIL_DISABLED, WIFI_FAIL_NO_MEM,
               WIFI_FAIL_NO_NETWORK, WIFI_FAIL_AUTH, WIFI_FAIL_TIMEOUT,
               WIFI_FAIL_LOST, WIFI_FAIL_TRANSFER_MODE } wifi_fail_t;

bool wifi_link_acquire(const char *reason);   // false: disabled / transfer mode / no_mem
void wifi_link_release(void);
wifi_link_state_t wifi_link_state(void);
wifi_fail_t wifi_link_fail(void);
bool wifi_link_wait_up(uint32_t timeout_ms);  // never from the LVGL thread
bool wifi_link_info(char *ssid, size_t n, int8_t *rssi, uint32_t *ip);
uint8_t wifi_link_leases(void);
```

- **First lease** starts the radio: memory gate → `esp_netif_init` (once) →
  STA netif → `esp_wifi_init` with `nvs_enable = 0` (the driver writes no
  flash; credentials are ours) → `esp_wifi_start` → connect to the first
  saved network, then the next on failure.
- **Last release** arms a 30 s linger; a new lease inside it cancels the
  stop. After it: `esp_wifi_stop` → `esp_wifi_deinit` → destroy the netif.
  The event loop and `esp_netif_init` stay (they cannot be undone).
- **One control task, internal stack**, owns every `esp_wifi_*` call —
  WiFi init and stop touch flash (PHY calibration data) and must not run on
  a PSRAM stack. Callers post requests to its queue and read state through
  atomics. Size the stack from a measurement, not a guess.
- **Memory gate** before start, shaped like the one in `bleMidi_start()`: if
  free internal RAM is below the threshold, `FAILED(no_mem)` and return; never
  assert, never restart. The threshold is set from the measurement in §6.
  A user-facing caller may then offer to free memory and retry (see
  "Reclaiming memory" below); a background caller never does.
- **Timeouts and retries:** 15 s per network to get an IP; auth failure moves
  to the next saved network at once; all exhausted → `FAILED(no_network |
  auth | timeout)`. A link lost while leased retries 3× (1 s, 3 s, 10 s), then
  `FAILED(lost)`. `FAILED` clears on the next first-lease.
- **USB Transfer mode** never starts WiFi (`FAILED(transfer_mode)`), the same
  rule as BLE: that boot starts nothing beyond display, USB and the raw card.
- **`enableWiFi == false`** refuses every lease (`FAILED(disabled)`), and
  switching it off while leased stops the link after the leases are told.

### BLE arbitration

The first lease stops BLE if it is running and remembers that it did; the
last stop (after the linger) restarts it with `bleMidi_start()` if
`enableBLE` is still set. One radio user at a time keeps both inside the
internal-RAM budget. A command that arrived over BLE gets its reply before
BLE stops (see §4).

### Reclaiming memory (user consent only)

When a lease is refused with `no_mem` and the request came from the user
(Settings, later the store and the update button), the UI asks, through
`ui_modal_confirm`:

> Not enough memory for WiFi. Stop the audio engine and other background
> work and try again? Sound comes back when WiFi is done.

The dialog lists what will stop. On *yes*, `wifi_link_acquire_reclaim()`
runs an ordered list of reclaim steps, re-checking free internal RAM after
each and stopping as soon as the gate passes, then starts the link:

| Step | Stop | Restore |
|---|---|---|
| 1 | BLE (already part of arbitration) | `bleMidi_start()` if `enableBLE` |
| 2 | audio engine: RT render task, I2S, the engine's internal-stack tasks | engine restart, current kit reloaded through the kit load worker |
| 3+ | further consumers the §6 measurement finds | their own restart |

- The list holds only steps that free **internal** RAM measurably — the
  engine's 4608 kB arena is PSRAM and does not help WiFi. §6 decides which
  steps exist; a step that frees less than 2 kB is left out.
- Every step has a restore; the link's final stop (after the linger) runs
  the restores in reverse order. Sound is back without a reboot.
- While reclaimed, the status bar's WiFi icon is joined by a muted-speaker
  glyph so a silent pad is explained.
- Background callers (sub-project B's daily check) never reclaim: they get
  `no_mem`, log it and try again at the next opportunity.
- Over verbs: `WIFI_UP RECLAIM` does the same for the bench (consent is the
  operator typing it).

### Scan

`wifi_link_scan(cb)` takes a lease, runs an active scan, hands back up to 20
results (SSID, RSSI, auth mode) in a PSRAM buffer, releases the lease. Same
task, same rules.

## 2. Saved networks: `main/wifi_creds.{h,cpp}`

- NVS namespace `wifi`, up to 5 networks (SSID ≤ 32 B, password ≤ 64 B), the
  last one that connected first. Saved only after a successful connection,
  except over `WIFI_SET`, which saves first (a bench sets credentials before
  the network is in range).
- Outside `CrosspadSettings`, therefore **shared by both firmware slots**: the
  per-firmware settings import on an A/B switch never touches this namespace.
  Checked: nothing on the A/B path erases a namespace (`fw_slots.cpp` erases
  only its own keys).
- **Factory reset forgets them.** `NvsSettingsStore::eraseAll()` and the
  `SETTINGS_RESET` verb erase only the `cfg_*` namespaces today; both gain
  `wifi`, so a board that changes hands does not keep the owner's password.
- API: `wifi_creds_list()`, `wifi_creds_put(ssid, pass)`,
  `wifi_creds_forget(ssid)`, `wifi_creds_promote(ssid)`. NVS writes go through
  the control task (internal stack), never from the LVGL thread.

## 3. UI

### Settings → WiFi

A new category registered in `main/app/settings/settings_app.cpp` beside
"BLE MIDI", built only from the existing `settings_row_*` helpers:

1. **WiFi allowed** — switch on `enableWiFi`.
2. **Status** — one line: `Off` / `Connecting to <ssid>…` /
   `Connected: <ssid>, -54 dBm, 192.168.1.23` / `Error: wrong password`.
3. **Find networks** — scans, lists SSID, signal and a lock; picking one
   opens the keyboard for the password (none for an open network), then
   connects; success saves it.
4. **Saved networks** — one row per network; press → *Connect* / *Forget*.
5. **Test connection** — `GET https://crosspad.app/api/device/ping` (DNS,
   TLS and the Worker in one call; sub-project B adds the endpoint, until then
   any 2xx/404 from crosspad.app counts as reachable).

The category holds a lease while it is open and releases it on leave. Every
blocking call (scan, connect, ping) runs on the control task or a worker,
results come back through an `lv_timer` — nothing waits on the LVGL thread.

### On-screen keyboard: crosspad-gui `ui_text_prompt()`

```cpp
namespace crosspad_gui {
enum TextPromptFlags : uint32_t { UI_TEXT_PASSWORD = 1u << 0 };
using TextPromptDone = void (*)(bool ok, const char* text, void* user);
void ui_text_prompt(const char* title, const char* initial, uint32_t flags,
                    size_t max_len, TextPromptDone done, void* user);
}
```

- Full screen on `lv_layer_top()`: title, `lv_textarea` at the top,
  `lv_keyboard` (~150 px) below. Both are already enabled in `sdkconfig.v*`
  and used by nothing.
- `UI_TEXT_PASSWORD`: dots, with a *show* toggle.
- Touch types; the encoder walks the keys and a press presses; a power
  click cancels (`ui_back_push`/`ui_back_remove`); the encoder group is saved
  and put back with `lv_indev_restore_group()`.
- Lives in crosspad-gui because it is portable — it runs in the PC simulator
  and the store's search (sub-project C) reuses it. Uses the theme's
  `remove_style_all` + `ANIM_OFF` pattern so a key press is not a full-screen
  redraw. crosspad-gui MINOR bump → 1.18.0.

### Status-bar icon

`LV_SYMBOL_WIFI` is one glyph with no levels, so the icon is a small widget
in crosspad-gui's `status_bar.cpp`: four bars of rising height drawn as
plain `lv_obj` rectangles (no font, no image).

| State | Look |
|---|---|
| WiFi not allowed (`enableWiFi` off) | hidden |
| Not connected (allowed; radio asleep, or disconnected) | 4 bars dimmed (grey, 30 % opacity) |
| Connecting (`STARTING` / `CONNECTING`) | bars light up 1→4, one every 250 ms, looping |
| Connected (`UP`) | N bars white, the rest dimmed: RSSI ≥ −55 dBm → 4, ≥ −65 → 3, ≥ −75 → 2, weaker → 1 |
| Failed | bars dimmed, the first one red |

With WiFi on demand, "not connected" is what the bar shows most of the time:
the radio sleeps between uses.

Data path: `CrosspadStatus` (core) gains `wifiState` and `wifiRssi`; the
platform updates them from the control task; the status bar reads them on
its existing refresh and drives the connecting animation from `lv_tick_get()`
the way the missing-battery blink does. crosspad-core MINOR bump → 1.38.0;
`main/include/crosspad_deps.h` declares both new triples after building and
running against them.

## 4. Verbs (`main/hil_control.cpp`)

| Verb | Reply |
|---|---|
| `WIFI_STATUS` | `WIFI: state=up ssid=<s> rssi=-54 ip=<a.b.c.d> leases=1 fail=none heap_int=<n>` |
| `WIFI_SCAN` | one `WIFI_AP: <ssid> <rssi> <auth>` row per network, then `OK` |
| `WIFI_SET <ssid> <pass>` | saves, then connects; `OK` (result via `WIFI_STATUS`) / `ERR <reason>` |
| `WIFI_LIST` | one `WIFI_NET: <ssid>` row per saved network |
| `WIFI_FORGET <ssid>` | `OK` / `ERR not_found` |
| `WIFI_UP [RECLAIM]` / `WIFI_DOWN` | take / drop the host's manual lease; `RECLAIM` frees memory first when the gate refuses |
| `WIFI_PING` | `WIFI_PING: ok ms=<n> status=<http>` / `ERR <reason>` |

- SSIDs with spaces: the argument grammar takes a quoted SSID
  (`WIFI_SET "My Net" secret`); the parser lands with the verbs.
- Over BLE, a verb whose lease would stop BLE replies first; the lease is
  taken 200 ms later. The caller reads the outcome with `WIFI_STATUS` over
  another transport or after WiFi releases.
- Host side: `crosspad_hil/verbs.py` + `knowledge/cdc.yaml` gain the verbs;
  the browser companion (crosspad.app `/device`) gains a small form that
  sends `WIFI_SET` — a separate commit in that repo.

## 5. Failure rules

Nothing in this subsystem restarts the board.

| Situation | Outcome |
|---|---|
| Not enough internal RAM at start | `FAILED(no_mem)`, BLE restored if it was stopped; a user-facing caller offers the reclaim dialog (§1) |
| Reclaim ran and the gate still fails | every step restored at once, `FAILED(no_mem)`, the dialog says so — no second offer |
| Wrong password / network absent / no IP in 15 s | next saved network; all tried → `FAILED(<reason>)`, radio stopped |
| Link lost while leased | 3 retries (1 s, 3 s, 10 s), then `FAILED(lost)` |
| `enableWiFi` switched off while leased | leases refused from then on, link stopped |
| USB Transfer mode boot | never starts, `FAILED(transfer_mode)` |
| Lease leak (a caller never releases) | `WIFI_STATUS leases=` shows it; the HIL leak test catches it |

## 6. Measurement before the threshold

Before the memory gate's threshold is fixed: `crosspad-hil mem` in the
launcher with the link down and up, on v1 and v2, and once with the Sampler
open and a kit loaded. Those numbers set the gate and go into
`docs/subsystems/wifi.md`. The same session measures what each reclaim
candidate frees in internal RAM (audio engine stopped, each internal-stack
task stopped in turn) — that table decides the reclaim steps in §1. If WiFi
does not fit beside a loaded kit even after reclaim, the work stops there
and comes back with the numbers before anything is worked around. `xtensa-esp32s3-elf-size -A build_v2/CrossPad.elf | grep ext_ram.dummy`
before and after — the image size is PSRAM too.

## 7. Tests (crosspad-hil scenario `wifi`)

1. `WIFI_SET` with a test network from an environment variable (never in a
   repo) → `state=up` in < 10 s → `WIFI_PING` ok.
2. Wrong password → `FAILED(auth)`, board alive, `RESET_LOG` unchanged.
3. 20 × `WIFI_UP`/`WIFI_DOWN` (waiting out the linger) → internal free RAM
   back within ±1 kB of the start; more is a leak and fails.
4. BLE enabled before → after `WIFI_DOWN` and the linger, `BLE_STATUS` running.
5. `hil_rt_glitch` with the link up: no glitches beyond the baseline.
6. Reclaim: Sampler open with a kit, BLE on, `WIFI_UP RECLAIM` → link up;
   `WIFI_DOWN` + linger → engine running, same kit loaded, a `PAD_PRESS`
   sounds (checked through `SMPL_*` counters), BLE back.

Scenario exits 2 (bench) when the WiFi environment variable is unset.

## 8. Documentation

`docs/subsystems/wifi.md` (source of truth: lease model, memory numbers,
verbs, traps found on hardware) and a short WiFi section in `CLAUDE.md`
pointing at it.

## Out of scope

Catalog, downloads and anything under `/api/device/*` beyond reachability
(sub-project B); store UI (C); cloud builder (D); SoftAP provisioning; NVS
encryption; enterprise (802.1X) networks; SNTP.
