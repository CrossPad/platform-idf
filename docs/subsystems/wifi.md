# WiFi on demand

> Source of truth for this subsystem. CLAUDE.md carries only the summary; when behaviour changes, update this file and the summary together.

Code: `main/wifi_link.{h,cpp}` (the link), `main/wifi_creds.{h,cpp}` (saved
networks), `main/wifi_verbs.cpp` (`WIFI_*` verbs). Shared types and names:
crosspad-core `crosspad/net/WifiTypes.hpp`. Design:
`docs/design/2026-10-02-wifi-on-demand-design.md`.

WiFi runs on demand in a normal boot; network mode is the fallback (spec
Revision 2). Measured on v2 (PCB 20, 2026-10-03) with a connected link, traffic over it and the stop: in the launcher,
BLE on or off, and in network mode the radio connects within ~5 s; with a TLS
ping it takes ~39 kB of internal RAM (almost all from the DMA/stack pool) and
~92 kB of PSRAM at its peak; after the linger 11.4 kB stays (the network stack)
and up/down cycles leak nothing. **With a Sampler kit loaded the start is
refused by design** (`failed(no_mem)` at the first-lease gate, nothing built):
the kit leaves ~71-76 kB of PSRAM on the integrated image (the launcher
~149 kB) against a 116736 B gate; that is the case the network-mode fallback
exists for. See "Memory gates", "Memory" and "Known limitations". v1: not
measured; v1 builds.

## Lease model

Nothing turns WiFi "on". A caller that needs the network takes a lease
(`wifi_link_acquire(reason)`) and gives it back (`wifi_link_release()`).
**One caller holds its lease for good:** a board paired to a crosspad.app
account with Remote management on (the default) keeps the lease `"remote"`
while WiFi is allowed, so on such a board WiFi stays up and BLE off (see
[Remote management](#remote-management-mqtt)).

- **First lease** starts the radio: the first-lease memory gate, the control
  task, then on that task the network stack (once per boot), the radio gate,
  the STA netif, `esp_wifi_init` with `nvs_enable = 0` (the driver writes no
  flash; saved networks are ours), `esp_wifi_start`, then a connect to the
  first saved network, the next on failure.
- **Last release** arms a 30 s linger. A lease inside it cancels the stop.
  After it: `esp_wifi_stop`, `esp_wifi_deinit`, netif destroyed, and the
  control task deletes its queue, its four timers and itself, so its internal
  stack comes back.
- **Nothing exists at boot.** `esp_netif_init` (lwIP's `tcpip` task), the
  default event loop (`sys_evt`), our two event handlers and the event group
  are built by the first lease (or `WIFI_SET`) and **stay** until reboot:
  neither the netif init nor the default loop can be undone.
- **One control task** (`wifi_ctl`, internal stack, priority 4, core 0) makes
  every `esp_wifi_*` call; WiFi init and stop touch flash (PHY calibration)
  and must not run on a PSRAM stack. Callers post to its queue and read the
  state through atomics. `wifi_link_wait_up()` never on the LVGL thread.
- **The network stack is built on `wifi_ctl`**, first thing when the task
  runs, not on the caller's stack: the first lease comes from the LVGL thread
  (Settings, the network-mode screen) or the 5 kB main task (verbs), and
  `esp_netif_init()` + the default loop are the deepest calls of a start. The
  first-lease gate has already counted its bytes; if it still fails, the
  start ends in `failed(no_mem)` and the next first lease tries again.
- **BLE arbitration.** The radio holds BLE from its start to its stop
  (`bleMidi_wifi_hold()` / `bleMidi_wifi_release()`, `main/midiCallbacks.cpp`):
  a running stack is stopped 200 ms after the verb, so a reply over the BLE
  companion leaves first, and restarted at the stop if `enableBLE` is still
  set. A start asked for in between (Settings switch, `BLE_ENABLE 1`,
  `BLE_START`, the linger included) is not made beside the radio -- the gates
  were measured with BLE off -- but remembered and made at the stop: the verbs
  answer `OK deferred=wifi`, the Settings switch shows a toast and the BLE page
  "waiting for WiFi to stop". A stop asked for in between wins. One mutex
  covers every start and stop, so two callers can never init NimBLE twice.
- **Timeouts and retries.** 15 s per network to get an IP; an auth failure
  moves to the next saved network at once; all tried means `failed` with the
  most telling reason of the sequence, not the last one: `auth` over
  `timeout` over `no_network` (a wrong password on the first network is not
  masked by a second network that is out of range). A link lost while leased retries 3 times (1 s, 3 s, 10 s), then
  `failed(lost)`. `failed` clears on the next first lease.
- `WIFI_SET` (and a UI pick) start a connect without a lease; when that
  settles in `up` or `failed`, the linger is armed so the radio still stops.

## States and failures

States (`WIFI_STATUS state=`): `off`, `starting`, `connecting`, `up`,
`stopping`, `failed`. Nothing in this subsystem restarts the board.

| Situation | Outcome |
|---|---|
| Not enough internal RAM or PSRAM before anything is built (first-lease gate) | start refused, `failed(no_mem)`; nothing allocated, `WIFI_UP` answers `OK` and `WIFI_STATUS` shows `failed no_mem`, `WIFI_SCAN` / `WIFI_PING` answer `ERR no_mem` (Sampler with a kit) |
| Not enough internal RAM or PSRAM after BLE stopped (radio gate) | `failed(no_mem)` before `esp_wifi_init()`, BLE restored if it was stopped |
| Wrong password / network absent / no IP in 15 s | next saved network; all tried: `failed(auth \| no_network \| timeout)` (ranked, see above), radio stopped |
| Link lost while leased | `ip`/`rssi` cleared at once, 3 retries (1 s, 3 s, 10 s), then `failed(lost)` |
| `enableWiFi` switched off | leases refused (`failed(disabled)`), a running link stopped |
| USB Transfer mode boot | never starts, `failed(transfer_mode)` |
| Lease leak (a caller never releases) | `WIFI_STATUS leases=` shows it |

`WifiFail::NotNetworkMode` still exists in core and is never produced
(removing a value would be a MAJOR version); a scan that fails has no value
either (`ERR timeout` / `ERR scan`, `WIFI_STATUS fail=` untouched).

## Memory gates

The radio takes memory from three places, and each has a gate
(`radio_fits()` in `main/wifi_link.cpp`):

| Constant | Heap measured | Threshold | Measured peak (+ ping) | Why this heap |
|---|---|---|---|---|
| `kWifiMinInternalFree` | `MALLOC_CAP_INTERNAL \| MALLOC_CAP_DEFAULT` (`heap_*`) | 10240 | 1836 | what the driver's and lwIP's fallbacks can use when PSRAM is short |
| `kWifiMinInternalTotal` | `MALLOC_CAP_INTERNAL` (`int_*`) | 48128 | 39224 | the driver's DMA buffers and task stacks, from the DMA/stack pool first |
| `kWifiMinPsramFree` | `MALLOC_CAP_SPIRAM` (`psram_*`) | 116736 | 91792 | `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`: driver and lwIP there first; mbedTLS only there |
| `kWifiMinPsramBlock` | largest free PSRAM block | 20480 | ~16.7 kB (mbedTLS input buffer, full record) | enough bytes in smaller pieces still fail |

The two internal thresholds are the highest `init − low` of a start + connect
+ TLS ping over L0, L1 and N ("Memory"), plus `kSpawnMargin` (8192), rounded
up to the next 1024. The PSRAM one is the integrated image's peak (91792 B,
launcher, BLE on, a kit loaded, start + connect + five TLS pings; see
"Integrated image") plus 24 kB, rounded up to 1024: with 8 kB, a start that
passes with 100.3 kB free in a Sampler with a kit loses its ping to an SSL
EOF, since the rest of the firmware keeps allocating while the handshake
holds its peak. `low` is the heaps' own minimum from the start
(`heap_caps_monitor_local_minimum_free_size_start()` in `watch_start()`), so
a dip between two samples — a TLS handshake — counts; it is summed per heap
and may read a little low, which errs on the safe side. The largest single
PSRAM allocation was recorded with a heap allocation hook in a test-only
`CONFIG_HEAP_USE_HOOKS` build: 8192 B, the ping task's stack; the driver's
largest was 4368 B and mbedTLS stayed below 8192 B against crosspad.app,
whose records are short. The block gate is set for a server that sends full
records instead: with `MBEDTLS_DYNAMIC_BUFFER` the input buffer is sized to
the record, ~16.7 kB for a full one (`MBEDTLS_SSL_IN_CONTENT_LEN` 16384 +
overhead); TLSF rounds a request up to its next size class and the driver's
connect buffers may split the block first, hence 20480.

Why two internal figures. `SPIRAM_MALLOC_RESERVE_INTERNAL` keeps a 64 kB
internal pool that only explicit DMA/internal requests and task stacks may
take: it has no `MALLOC_CAP_DEFAULT`. The driver's fallback for a PSRAM
allocation asks for `MALLOC_CAP_DEFAULT | MALLOC_CAP_INTERNAL` and cannot
use it; its DMA buffers and the stacks of its tasks come from it first. In
the launcher the pool holds ~59 kB of the ~95 kB `MEM int_free` reports, and
of the radio's 39 kB internal peak only 1.8 kB comes from outside it. A gate
on `MALLOC_CAP_INTERNAL` alone, with no PSRAM figure, lets a Sampler with a
kit through and the driver fails half way (see "Traps found on hardware").

Where the gates run:

1. **Before anything is built** (`fits_locked()`, on the caller's thread
   before `wifi_ctl` is spawned, and in `radio_init()` before the network
   stack is built there). lwIP's `tcpip_init()` asserts on a failed
   allocation and the network stack never comes back, so the first lease asks
   for all four, with `kControlTaskBytes` (6188) and `kNetStackBytes` (10680,
   until built) added to both internal needs. With BLE running only the
   internal bytes about to be created plus `kSpawnMargin` are asked for (BLE's
   internal RAM comes back before the radio gate), and PSRAM with
   `kBlePsramBytes` (15360; BLE hands back 16068 B) credited; the largest
   PSRAM block is checked as it is, uncredited. What that branch cannot see
   is the all-internal gate (BLE's share is not back yet): if the radio gate
   then refuses, the network stack (10680 B) stays built and BLE comes back.
2. **Radio gate** (`radio_init()`, on `wifi_ctl`, after BLE is stopped,
   before `esp_wifi_init()`): all four.

A refusal at either is `failed(no_mem)` before the driver allocates anything;
`WIFI_STATUS` then shows `low == init` for all three heaps.

## Memory (v2, 2026-10-03)

### Integrated image (`.ext_ram.dummy` 3014624)

A v2 board, PCB 20. This image is one 64 kB PSRAM page larger than the one
below (`.ext_ram.dummy` 2949088 → 3014624) and its apps use more PSRAM at run
time, so the launcher and the Sampler have less than in the tables below. `MEM` with BLE
on; `init`/`low` from `WIFI_STATUS`.

| State | `MEM` int_free / psram_free / psram_largest | WiFi |
|---|---|---|
| Launcher, kit loaded | 64647 / 149376 / 79860 (after a long session: 47271 / 141216 / 47092) | starts: `psram_init` 157472 once BLE has handed its share back, 40.7 kB above the gate |
| Launcher, no kit (after smoke) | 56819 / 198684 / 79860 | above every gate (not started here) |
| Sampler, kit loaded | 64491 / 70836 / 59380 (Kraken Blue); 62603 / 75712 / 67572 (BLACK TESLA) | refused by design: `WIFI_UP` → `ERR no_mem`, `failed(no_mem)`, `stack_free=0`, BLE untouched |

Launcher, BLE on, a kit loaded, `WIFI_UP` then five `WIFI_PING`s (1823-4763
ms, all `status=404`): `heap` 26472 → 25204 (1268), `int` 72607 → 35411
(37196), `psram` 157472 → 85272 connected, 65680 after the first ping and
unchanged by the next four (**91792**). The internal peaks stay under the
earlier ones, so those gates are unchanged.

### Earlier image (`.ext_ram.dummy` 2949088)

The same board, one PSRAM page smaller. Each scenario
from a fresh boot (`bench.py hil console --reset`), `RESET_LOG` unchanged from
boot to the end of the scenario. Only the test network saved.

### With the three-heap gates (final image)

`init`/`low` from `WIFI_STATUS` after the connect and again 6 s after a
`WIFI_PING`; peaks include the ping.

| Scenario | `heap_init` → `heap_low` | `int_init` → `int_low` | `psram_init` → `psram_low` | ping | outcome |
|---|---|---|---|---|---|
| L0 launcher, BLE off | 33216 → 32012 (1204) | 79927 → 41967 (37960) | 111232 → 19484 (91748) | 1925 ms | up |
| L1 launcher, BLE on | 33544 → 32384 (1160) | 79847 → 41931 (37916) | 111224 → 19500 (91724) | 1807 ms | up; BLE `running=1` after |
| N network mode | 39416 → 38580 (836) | 88927 → 49703 (**39224**) | 5173360 → 5082412 (90948) | 1708 ms | up |
| S0 Sampler + kit, BLE off | 12516 = 12516 | 70991 = 70991 | 28248 = 28248 | `ERR no_mem` | refused at the first-lease gate |
| S1 Sampler + kit, BLE on | 11848 = 11848 | 40547 = 40547 | 11908 = 11908 | `ERR no_mem` | refused at the first-lease gate; BLE untouched |

With the gates off (a measurement build, same image otherwise) L0, L1 and N
gave the other maxima of the table in "Memory gates": `heap` 1152 / **1836** /
828, PSRAM **91760** / 91704 / 91000 (connected, before the ping: ~72.1 kB).

- **S0/S1 are the designed refusal.** Nothing was created: `stack_free=0`
  (no control task since boot), `MEM` identical before and after, `int_min`
  unchanged (31563 / 31527). PSRAM 28248 (largest 11764) and 11908 (largest
  11508, 27268 with BLE's share credited) against the then 100352; S0 also fails the
  `heap` gate (12516 free against 26084 with the first lease's bytes). The
  user-facing request then offers network mode (spec Revision 2).
- **L1:** BLE's first-lease PSRAM credit: 95156 + 15360 ≥ the then 100352. BLE came
  back after the linger.
- **Slack:** on this image the launcher had ~11 kB of PSRAM above the then
  100352 B gate (111.2 kB at the radio start). On the integrated image it has
  ~40 kB above the 116736 B gate (see "Integrated image").
- **N:** the network-mode screen's lease (LVGL thread) connected at boot; no
  canary or watchpoint hit: `RESET_LOG` moved only for the `NET_MODE 1` and
  `NET_MODE 0` restarts; `stack_free` 1204.

## Reclaim (not built)

Stopping the audio engine to make room frees ~4.3 kB of internal RAM, too
little to matter, so nothing but BLE gives way to the radio (plain
arbitration, see "Lease model"); what does not fit goes to network mode.

## Stacks

- **`wifi_ctl`** (`kTaskStack` 5120) builds the network stack on its own
  stack. A probe build with a high-water checkpoint after every step
  (2026-10-04) puts the peak at `esp_wifi_init()` (2928 B used), the BLE
  handover before it at 2608 B, and nothing later (connect, credentials write,
  TLS ping, scan, stop, the deferred BLE start with its NimBLE init) goes
  deeper. `kTaskStackHeadroom` adds 1024 B over a 4096 B stack (also counted
  in `kControlTaskBytes`): ~2.2 kB left at the peak.
- **`wifi_scan` / `wifi_ping`** verb tasks: PSRAM stacks, 4096 / 8192 B,
  one-shot (the FW_LIST shape).
- **Main task stack** (5 kB): builds nothing; `WIFI_UP` and
  `WIFI_SET` only post to `wifi_ctl`.

## Verbs

On CDC, the BLE companion and USB MIDI SysEx `0x1F` alike (the companion
allow-list passes `WIFI_*`, except the CDC-only four below). From the host:
`python3 tools/bench.py hil cdc <VERB …> --expect <reply prefix>` —
crosspad-hil refuses a verb it does not know without `--expect`.

| Verb | Reply |
|---|---|
| `WIFI_STATUS` | `WIFI: state=<s> ssid="<ssid>" rssi=<dBm> ip=<a.b.c.d> leases=<n> fail=<f> reason=<n> heap_int=<B> heap_init=<B> heap_low=<B> int_init=<B> int_low=<B> psram_init=<B> psram_low=<B> stack_free=<B> enabled=<0\|1>` |
| `WIFI_ENABLE 0\|1` | `OK`; persisted (`CrosspadSettings::wireless.enableWiFi`); `0` stops a running link |
| `WIFI_SET "<ssid>" ["<pass>"]` | saves first (a test host sets credentials before the network is in range), then connects: `OK`, result via `WIFI_STATUS` / `ERR short_pass` (1-7 characters) / `ERR bad_pass` (unparsable password) / `ERR save` / `ERR WIFI_SET <ssid> [pass]` (no SSID, or a `"` inside it) |
| `WIFI_LIST` | one line: `WIFI_NET: count=<n> \| <ssid> \| <ssid> …` (order = connect order) |
| `WIFI_FORGET "<ssid>"` | `OK` / `ERR not_found` |
| `WIFI_FORGET_ALL` | the factory reset's WiFi step (`wifi_factory_reset()`), run before the reply: `OK` once every saved network is erased from NVS and RAM (a `WIFI_LIST` after it is empty), then a radio stops if WiFi is not allowed / `ERR erase` (NVS refused; the list is unchanged) |
| `WIFI_UP` / `WIFI_DOWN` | CDC only. Take / drop the host's one manual lease: `OK` / `ERR disabled` (`enableWiFi` off) / `ERR <fail>` when the lease is refused outright (`transfer_mode`, `no_task`). The gates run on the control task, so a start they refuse is `OK` here and `state=failed fail=no_mem` in `WIFI_STATUS` |
| `NET_MODE [0\|1 [TAB=wifi\|store\|updates]]` | CDC only. No argument: `NET_MODE: active=<0\|1> tab=<name\|->`. `1` / `0`: `OK reboot=1` and the board restarts into / out of network mode (`OK unchanged` when already there; `ERR transfer_mode` from USB Transfer mode) |
| `WIFI_SCAN` | one line: `WIFI_AP: count=<n> \| <rssi> <auth> <ssid> \| …`, strongest first, one row per SSID, hidden networks left out, at most 20 and at most 1 kB (SSID last, may hold spaces); `<auth>` is `open` `wep` `wpa` `wpa2` `wpa2e` `wpa3` `wpa2wpa3` `owe` `other`. `ERR <fail>` when the lease or the radio start is refused, `ERR timeout` when the control task did not answer within 15 s, `ERR scan` when the driver refused the scan or its records (neither touches `WIFI_STATUS fail=`: a link can be up while a scan fails), `ERR busy` while one runs. ~3 s: pass `--timeout 20000` |
| `WIFI_PING` | `WIFI_PING: ok ms=<n> status=<http>` for 200 ≤ status < 500 (a 404 from crosspad.app still proves DNS + TLS) / `ERR status=<http>` / `ERR err=<esp_err name> tls=<mbedTLS code>` (`ESP_ERR_TIMEOUT` = a TLS step that ran out of time with no error recorded; `tls=-0x2700` style, `0x0000` when mbedTLS recorded none) / `ERR err=ESP_ERR_WIFI_NOT_CONNECT tls=0x0000 fail=<fail>` (link not up within 10 s, the server was never tried; `fail=none` while the connect sequence is still trying) / `ERR <fail>` / `ERR busy` |

`WIFI_SCAN` and `WIFI_PING` answer from a task of their own, so the CDC drain
loop never waits on them; the BLE companion and SysEx get `ERR use_cdc` for
both (the companion's reply must be ready when its handler returns). They get
it for `WIFI_UP` / `WIFI_DOWN` too: over BLE the radio stops BLE right after
the `OK`, the peer could never send the `WIFI_DOWN`, and the manual lease would
keep the radio up and BLE off until a reboot. The companion matches these by
verb name, arguments or not.

- `ssid` is quoted so a name with spaces stays one field; none is `ssid=""`.
  Every SSID in a reply (`WIFI_STATUS`, `WIFI_AP`, `WIFI_NET`) is escaped:
  backslash, `"` and `|` get a backslash in front, CR, LF and other control
  bytes become `\r`, `\n`, `\xHH` (`wifi_verbs.cpp` `escape_ssid()`; crosspad-hil
  `_unescape_ssid()` undoes it). A literal ` | ` therefore only ever separates
  rows. Arguments have no escapes: an SSID or password with `"` cannot be sent.
- `WIFI_SET` answers `ERR short_pass` for a password of 1..7 characters (or 64
  characters that are not all hex) before saving anything: a WPA passphrase is
  8..63 characters or 64 hex digits, and a shorter candidate drops a link that
  works. Empty stays an open network. The password prompt on the WiFi page asks
  again, with what was typed and "Min 8 characters", on the same check
  (`wifi_pass_acceptable()`).
- `enabled` is the persisted `wireless.enableWiFi`, so a test can record and restore it.
- `reason` is the last 802.11 disconnect reason not caused by us (our own
  `ASSOC_LEAVE` is left out), `0` = none since boot. It stays after a later
  success: `state=up … reason=202` means an earlier attempt was refused.
  The console logs every disconnect at INFO: `disconnected, reason <n>`.
- `heap_*` is internal RAM without the DMA/stack pool
  (`MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT`; `heap_int` is its current free),
  `int_*` all internal RAM, `psram_*` PSRAM. `*_init`: free when the last
  start began (after BLE handed its memory back, or at the first-lease gate
  when that refused); `*_low`: the heap's own minimum from then until the
  radio stopped, dips between reads included, read at each step, every 5 s
  RSSI poll and the stop. `init − low` is what the radio costs; the gates are
  set from it ("Memory gates"). A refused start reads `low == init`.
- While the radio runs, `MEM int_min` is the minimum since the radio start
  (the heap component's local-minimum window, opened by `watch_start()`);
  after the stop it is the boot-time minimum again.
- `stack_free` is `wifi_ctl`'s high-water mark while it exists, else the last
  retired task's; `0` means no control task has run since boot.

## Scan

`wifi_link_scan()` takes a lease, posts `Msg::Scan` to `wifi_ctl` and waits
(15 s bound) for a blocking active scan; the lease goes back afterwards.

- **Radio off:** `radio_init()` brings it up for the scan alone, no connect
  (with no saved network a connect sequence fails at once and would leave
  nothing to scan with). It stays on under the previous `failed(…)` state
  until the linger; a lease that arrives meanwhile starts a connect sequence
  on it.
- **Connect in progress:** the station refuses to scan while connecting, so
  the attempt is paused (timers stopped, `esp_wifi_disconnect()`) and started
  again on the same network after the scan. The pause's disconnect is
  reported as reason 36 (`STA_LEAVING`), not `ASSOC_LEAVE`, and is handled
  only after the resumed attempt started, so it cannot fail that attempt
  (`fail=timeout reason=36`). Every attempt boundary -- each
  `esp_wifi_connect()`, each `esp_wifi_disconnect()` -- posts a fence event
  through the default event loop; the loop stamps every disconnect and IP
  event with the last fence dispatched before it, so an event belongs to the
  current attempt only if no fence followed it, however late a task gets to
  run. A got-IP is also checked against the netif's address (zeroed on every
  disconnect), which covers a DHCP lease completing after the station left.

## TLS

`wifi_ping()` (`main/wifi_ping.cpp`) is one `GET /api/device/ping` over
esp-tls, reading only the status line; `esp_http_client` with its parser and
transport layer cost ~21 kB more image. The request names the board:
`?sn=<serial>&rev=v<N>&fw=<version>`, the serial from `device_serial()` (the
PCB serial in the STM's OTP, `CPnnnn`, or the MAC as 12 hex digits on a board
without one); the Worker logs it as a `device_ping` event and stores nothing.
`WIFI_PING` answers `... sn=<serial>`.

On this board the image size is PSRAM (`SPIRAM_FETCH_INSTRUCTIONS` +
`SPIRAM_RODATA`, 64 kB pages; see CLAUDE.md, "Build Gotchas").
`esp_http_client`, the full Mozilla certificate bundle and mbedTLS as
configured by default add **192 kB** of `.ext_ram.dummy` (2883552 → 3080160)
and leave 58 kB of PSRAM outside the engine arena: the kit scan's 32 kB
`reserve()` fails on every boot ("Discovered 0 kits") and the TLS handshake
runs out of MPI memory. The client as built adds **64 kB** (2949088), 198 kB
outside the arena:

| Step (v2) | `.ext_ram.dummy` |
|---|---|
| no TLS client | 2883552 |
| `esp_http_client` + full bundle, mbedTLS as before | 3080160 |
| custom bundle + client-only mbedTLS (list below) | 3014624 |
| + `ERROR_STRINGS`, `SSL_KEEP_PEER_CERTIFICATE`, `X509_CSR_PARSE_C`, `CLIENT_SSL_SESSION_TICKETS`, `FS_IO`, `GCM_SUPPORT_NON_AES_CIPHER` off | 3014624 (text −3.3 kB, page not crossed) |
| esp-tls instead of `esp_http_client` | **2949088** |

mbedTLS in `sdkconfig.v1`/`sdkconfig.v2` (`sdkconfig.defaults` sets none of
these): `TLS_CLIENT_ONLY` (no server, no server session tickets); key
exchange `ECDHE_RSA` and `ECDHE_ECDSA` only; curves secp256r1, secp384r1,
curve25519 only; no renegotiation, no PEM write, no CRL parsing;
`DYNAMIC_BUFFER` on. `SSL_ALPN` stays on: anything that links
`esp_http_client` calls
`esp_transport_ssl_set_alpn_protocol()` unconditionally and does not link
without it. CCM and CMAC stay: the WPA supplicant (PMF, SAE) and ESP-NOW
security use them; WPA3 SAE uses mbedTLS crypto, not the SSL layer.

**Certificates:** `main/certs/crosspad_roots.pem`
(`CONFIG_MBEDTLS_CUSTOM_CERTIFICATE_BUNDLE_PATH`, default bundle `NONE`):
GTS Root R1, GTS Root R4, GlobalSign Root CA, ISRG Root X1, ISRG Root X2.
Cloudflare serves crosspad.app as leaf → WE1 → GTS Root R4 (ECDSA) or leaf →
WR1 → GTS Root R1 (RSA), the GTS root cross-signed by GlobalSign Root CA, and
`esp_crt_bundle` looks up the issuer of the topmost certificate sent — so
GlobalSign Root CA is what today's chain needs; the others cover a chain sent
without the cross-signature and a switch to Let's Encrypt. GlobalSign Root CA
expires 2028-01-28. How to regenerate is in the file.

Measured 2026-10-03 (v2), the TLS-client image: normal boot
`WIFI_PING: ok ms=2203 status=404`, network mode `ms=3017`. Pings that take
the lease first (cold radio) measured 2.0-7.8 s, typically 2-4 s, network mode
1.6-3.0 s; on a warm link (radio kept up by the linger) ~2 s, 20/20 ok. The ping has no overall bound
(`wifi_ping.h`): up to 10 s for the link, then 20 s for DNS + connect +
handshake together and 20 s per read or write after it. The handshake alone
costs 1.27 s of CPU (software ECC, code in PSRAM); right after a cold radio
start an access point can stall it for up to 12.5 s with the CPU idle
(measured), which a 10 s bound would turn into
`ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`. hil waits 35 s for the reply.

## Saved networks

`main/wifi_creds.cpp`: NVS namespace `wifi`, key `nets`, one blob, up to 5
networks (SSID ≤ 32 B, password ≤ 64 B), the last one that connected first.

- Outside `CrosspadSettings`, so **shared by both firmware slots**; the A/B
  switch's settings import never touches it.
- **Erased by a factory reset**: Settings → System → factory reset calls
  `IGuiPlatform::onFactoryReset()` (gui), which on this board is
  `wifi_factory_reset()`: `wifi_creds_clear()` erases the namespace and zeroes
  the RAM copy on a one-shot internal-stack task, then
  `wifi_link_enable_changed()` stops a running radio (the reset turned WiFi
  off). `WIFI_FORGET_ALL` runs the same step from a host, synchronously.
  `SETTINGS_RESET` erases the `cfg_*` namespaces, clears the saved networks
  through `wifi_creds_clear()` (lock and RAM copy included, so nothing in the
  200 ms before its reboot writes the old list back) and reboots.
- **In RAM**: the saved list, its NVS blobs, the connect sequence's copy and
  the verbs' buffers sit in PSRAM (`EXT_RAM_BSS_ATTR`); the blobs, `WIFI_SET`'s
  password and `WIFI_LIST`'s snapshot are zeroed after use, the sequence's
  copy at every radio stop.
- **Stored in clear**: `CONFIG_NVS_ENCRYPTION` is off, as on most ESP32
  products.
- A test network comes from the environment (`CROSSPAD_WIFI_SSID`,
  `CROSSPAD_WIFI_PASS`), never from a repo.

## The WiFi page

`main/gui/wifi_page.cpp`, one page for both places it appears: Settings →
WiFi in a normal boot (`cat_build_wifi()` in `main/app/settings/settings_app.cpp`,
the last category) and the WiFi tab of network mode (`wifi_page_build()` from
`net_mode_screen.cpp`). Rows, top to bottom:
*WiFi allowed*, *Status* (state, or a job's note for 5 s), *Find networks*,
*Test connection*, then *Saved networks* (one row each, *...* →
Connect / Forget / Cancel). The page scrolls; the encoder follows the focus.

- **Who holds the lease.** Settings: the page, while it is open and WiFi is
  allowed (`holdLease`) -- opening the category starts the radio, and BLE
  gives way while the lease is held (a running BLE is stopped 200 ms later and
  comes back after the linger). Closing the category or leaving Settings gives
  the lease back; the radio lingers 30 s and stops. Network mode: the screen,
  for the whole boot; the page holds none there.
- **Blocking work runs on one worker at a time**, its result picked up by the
  page's 250 ms timer: the scan (`wifi_link_scan()`, PSRAM stack), the test
  (a lease + `wifi_ping()`, PSRAM stack) and Forget (`wifi_creds_forget()`,
  internal stack -- an NVS write). A result for a page already closed is
  dropped by generation.
- **Test** names the side that failed: "Connection OK (<ms> ms)",
  "crosspad.app answered <status>", "crosspad.app unreachable (<esp_err>)" only
  when the link was up and DNS / TLS / the request failed, and "No connection:
  <reason>" when the lease was refused or the board never joined a network
  (`WIFI_PING_NO_LINK`): wrong password, network not found, no answer from the
  network (also when the 10 s ran out with the sequence still trying).
- **Find networks** opens a full-screen list on the top layer (SSID, dBm,
  auth; strongest first) with *Close*; touch, the encoder and a power click
  close it (the click closes the list, not the screen). A pick opens the
  password keyboard (`ui_text_prompt`, dots, eye toggle); an `open` network
  connects with no keyboard.
- **Saved rows are built once** (five, hidden past the list's end) and
  relabelled in place: a row deleted and made again would join the encoder
  group after Exit, and deleting the focused one would hand the focus to Exit.

**Candidate connect** (spec §2): a network picked on the device -- from the
scan list, or a saved row's *Connect* -- goes to
`wifi_link_connect_candidate()`: tried first and alone, **saved (first in the
list) only once it has an IP**; a failed attempt saves nothing and leaves the
saved order as it was, and the link ends `failed(<reason>)` with the radio
stopped (the leaseholder's *Connect* starts it again). Any stop -- every
network failed, WiFi switched off, the linger -- drops a candidate unsaved.
`WIFI_SET` is the other way round: it saves first, then connects (see
"Traps").

**The no_mem fallback** (`wifi_fallback_offer()`, spec Revision 2): a
user-facing request refused with `no_mem` offers *Restart in network mode?*
on the tab that asked, once per page visit (Settings: the page's lease or its
scan ending in `failed(no_mem)` while WiFi is allowed). Only the user's
*Restart* restarts the board. Never offered by background callers (they log
and try later), never by verbs, never inside network mode (nowhere further to
go: the page shows the error). `WIFI_UP` answers `OK` for the lease it takes:
the gates refuse the radio start on the control task, so the refusal shows in
`WIFI_STATUS` (`state=failed fail=no_mem`), not in the reply.

Hardware check, network mode (2026-10-03, v2): scan 20 networks, list closed by
encoder and by power click; saved *Connect* up again with `WIFI_LIST`
unchanged; *Test* "Connection OK (3288 ms)"; a wrong password typed with the
encoder alone ended `failed(auth)` (reason 202), `WIFI_LIST` unchanged,
`NET_MODE active=1`; Forget of a `WIFI_SET` scratch network left only the
test network. Walking keys: 26 steps, 31 flushes of ~4 kB (two keys),
render ≤ 17 ms. **Open network: not exercised.**

Encoder on the keyboard (gui `ui_text_prompt`): the first key (top left,
`1#` / `1` on the symbols layout) is highlighted on open, and turning left
from it -- or from no highlighted key at all -- leaves the keys for the eye; a
press there shows/hides the password, and turning on from the eye goes back
into the keys (straight into editing, on the first key). A key that acts and
leaves no key selected puts the highlight back on the first key, because
LVGL's left turn from no selection lands on the last key, OK. The eye is not
on a long press: the button matrix acts on press, so a long press would also
type the highlighted key. Touch is unchanged.

**OK on an empty password keeps the prompt open** (`UI_TEXT_REQUIRED`): the
page would otherwise hand `""` for a secured network to
`wifi_link_connect_candidate()`, which drops a working link and ends
`failed(auth)`. Open networks never show the keyboard and still connect with
no password. Hardware check (Settings, link up): `1#` pressed, one left turn -> the
eye, press -> eye toggled, nothing submitted; OK on the empty field (selection
proved on OK by the wrap to the eye) -> prompt still open, link `up`; the
test password typed with the encoder alone, OK -> prompt closed, link `up`,
`reason=0`, `WIFI_LIST` unchanged (twice).

Hardware check, normal boot (2026-10-03, v2, kit loaded, BLE advertising): Settings →
WiFi lists its rows; opening it takes the lease (`leases=1`, `connecting`,
then `up` on the saved network within ~6 s) and BLE stops (`running=0`).
*Find networks* lists 18–20 networks, *Close* closes the list; *Test* holds a
second lease while it runs (`leases=2`, then 1; `WIFI_PING` beside it: ok,
1838 ms, 404). `APP_STOP` gives the lease back (`leases=0`), `state=off` 36 s
later and BLE advertising again. On the keyboard: one left turn after opening
reached the eye, a press flipped it to "hide", one turn back landed in the
keys in editing mode. The status bar was not checked here: `SCREENSHOT`
answers "no memory" in a normal boot.

**Leaving mid-operation** (Settings, normal boot): twenty visits that pressed
*Find networks* and left with `APP_STOP` before the list (ten at once, ten
after 0.4 s) -- no reset, no list left over the launcher, `leases=0` after
each, and the next visit's *Find networks* listed networks (the dropped
result cleared `busy`). Leaving during *Test*: the test's own lease ends
with the ping, `leases=0`. Leaving with the password keyboard open: the
encoder goes back to the launcher; a power click afterwards cancels whatever
is left of the prompt, harmlessly. `app_churn` passes with the page in
Settings.

**The fallback on the board** (test-only image with the internal gate raised
above what the chip has, never committed): Settings → WiFi ended
`failed(no_mem)` with `leases=1` and showed **one** dialog (*Restart* /
*Cancel*). *Cancel*: the board stayed, the status line read "Error: not
enough memory", and *Find networks* raised no second dialog in that visit.
Re-entering offered it again; *Restart* rebooted into network mode
(`NET_MODE active=1 tab=wifi`, WiFi tab showing). In network mode the radio
was refused too and only "Error: not enough memory" showed (screenshot), no
dialog. After `NET_MODE 0`: `WIFI_UP` → `OK`, `WIFI_STATUS` →
`failed(no_mem)`, no dialog on the screen.

## WPA2/WPA3 transition access points

On a WPA2/WPA3 transition AP the station uses SAE, whose auth step *is* the
password check, so a wrong password looks exactly like an AP refusing the
station: disconnect reason 202 (`AUTH_FAIL`) on every attempt. Check the
password before the STA config. The STA config: `pmf_cfg.capable = true`,
`sae_pwe_h2e` left at zero, fast scan, and the auth threshold
`WIFI_AUTH_WPA2_PSK`, so a same-name WPA1/TKIP access point cannot take the
password over a downgraded handshake.

## Network mode (the fallback)

Normal mode fits in the launcher (BLE on or off) and on any screen that
leaves the gates' memory: ~48 kB of internal RAM (DMA/stack pool included) and
~117 kB of PSRAM. Network mode is for when it does not: BLE plus a heavy app,
and today any Sampler with a kit loaded (~71-76 kB PSRAM free), where the
first-lease gate refuses by design and the user-facing request offers it.

- **What it is.** A boot shaped like USB Transfer mode (`main/net_mode.cpp`,
  `net_mode.h`): entering arms a one-shot flag in RTC memory (`CP:N`, honoured
  only after `ESP_RST_SW`, so a power cycle, panic or watchdog boots normal)
  and restarts; leaving is another restart. `NET_MODE 1|0 [TAB=wifi]` over CDC
  does the same from a host; there is no SysEx or BLE way in.
- **What that boot skips.** No sample engine (`AudioTasks_Start`), no kit scan,
  no BLE, no SD hotplug, no STM reflash decision, no UAC2 (it stays on the CDC
  profile; the stored `usbAudioEnabled` is untouched). The radio then has the
  heap to itself: 89 kB internal at `int_init`, ~5 MB PSRAM.
- **The screen** (`main/gui/net_mode_screen.cpp`): header with the WiFi bars and
  Exit, the shared WiFi page as the WiFi tab (only tab so far; the flag already
  carries `store` and `updates` for later work). The screen holds the lease
  for the whole boot. Exit: touch, encoder or power click, then a restart.
  OTA progress overlay and toasts still work; the STM reflash overlay does not.
- **Entry points.** A user-facing request refused with `no_mem`
  (`wifi_fallback_offer()`, see "The WiFi page"), `NET_MODE 1`. A board left
  in it answers no `APP_LIST`: `bench.py check` names it and `bench.py ready`
  leaves it.

## Status-bar icon

Four bars in the status bar (`ui_wifi_bars`, plain LVGL objects, no font or
image), drawn as core's `wifiBarsLook()` says: hidden while WiFi is not
allowed; lit by RSSI when up (`wifiRssiBars`); a chasing bar while starting
or connecting; the first bar red after a failure; dimmed when allowed and off.
`status.wifiState` / `wifiRssi` are written by the link, so the status bar
never calls into it. Same bars in the network-mode header.

## Browser companion

The browser companion has a WiFi card (USB serial
link only): it sends `WIFI_SET "<ssid>"
"<pass>"`, which saves and connects. A `"` or line break in either field is
refused before sending, an `ERR short_pass` reads "at least 8 characters",
any other `ERR` is shown as it came; a refusal does not drop the link.
`WIFI_SCAN` / `WIFI_PING` / `WIFI_UP` / `WIFI_DOWN` answer `ERR use_cdc` over
the companion paths.

## Remote management (MQTT)

`main/cp_remote.cpp` keeps esp-mqtt connected to the board's own broker on
crosspad.app (`wss://<account base>/api/device/mqtt/<serial>`, a `ws://` URI
when `CROSSPAD_ACCOUNT_URL` is `http://`, as in a dev server) and runs what the
browser sends through `hil_control_companion_handle()`, so the **companion
allowlist** is the whole command surface; `REMOTE_STATUS` joined it (read-only).
Username = serial, password = the device token (never logged, wiped from the
RAM copy once esp-mqtt has its own). Topics `cp/<serial>/req|rsp|state|peers`;
frames are core's `RemoteFrame` (`"<id> <line>"`).

**Lease.** A `bsp_worker` job ("cp_remote", 1 s) holds the lease `"remote"`
while the board is paired, `enableWiFi` and `enableRemote` (Settings -> WiFi ->
Remote management, on by default) are set. **BLE is off for as long as it
runs** (the lease stops it, as for any lease); switching the toggle off, an
unpair or `WIFI_ENABLE 0` releases it. Transfer-mode and network-mode boots
never start it (`cp_remote_init(false)`, `REMOTE_STATUS` says `off`,
`reason=boot_mode`).

**BLE switch.** The radios are exclusive and remote management would keep WiFi
up indefinitely, so the two switches settle it this way: turning **BLE MIDI on**
in Settings while remote management would run (`cp_remote_wanted()`) turns
Remote management **off** (saved) and says "Remote management turned off: BLE
and WiFi remote cannot run together"; the worker's next pass (<= 1 s) tears
the client down and releases the lease, and BLE starts once the radio has
stopped (the 30 s linger, if nothing else holds a lease). Turning **Remote
management on** in the WiFi page while BLE is on is allowed -- WiFi wins, as
for any lease -- and the page's status line says "BLE MIDI is off while remote
management runs". The job never blocks: the client is
built there (init + start only), but `esp_mqtt_client_stop()` can wait seconds
inside a TLS handshake, so a one-shot PSRAM-stack task (`cp_remote_off`) stops
and destroys the client and releases the lease.

`REMOTE_STATUS` -> `REMOTE: state=<s> reason=<r> peers=<n> queued=<n> done=<n>
busy=<n> uri=<uri|->`:

| state | meaning |
|---|---|
| `off` | WiFi or Remote management is off, or the board was never paired (`reason=not_paired`) |
| `unlinked` | the pairing was revoked by the server (the broker refused the token: see below), "link again" is pending |
| `waiting` | lease taken, the link is not up yet |
| `no_wifi` | the lease was refused or the link failed (including a link lost after it was up); `reason` is the `wifiFailName` token (`no_mem` with a Sampler kit, `no_network`, `auth`, `lost`...). Link-failure backoff below |
| `connecting` / `online` | MQTT handshake / subscribed and `cp/<serial>/state` published (retained `online fw=<ver> rev=v<n>`, last will `offline`) |
| `backoff` | the connection dropped or failed: reconnect after 5 s, doubling to 300 s, plus a random 0-50 % of that delay (`esp_random()`, so boards a broker restart dropped together do not return together), reset on a CONNECT |

**Link-failure backoff.** A refused lease or a failed link (`no_network`,
`auth`, `timeout`, `lost`...) is retried after 60 s, doubling to **30 min**: a
saved network that is gone would otherwise start the radio every minute
forever. `no_mem` (the memory gate: a Sampler kit) stays at 60 s, since it
clears as soon as the app lets go. The delay goes back to 60 s on a successful
link (`online`) and when the user opens the WiFi settings page
(`cp_remote_retry_now()`, which also makes the next pass try at once).

`reason` is also `no_token`, `no_mem` (the 15 kB buffers below), `mqtt_init`,
`mqtt_start` when the client could not be built, `mqtt_timeout` when it stayed
in `connecting` for 30 s (DNS, transport/TLS up to 10 s and CONNACK up to 10 s
add up; the client task can end without a DISCONNECTED) and `reconnect` when `esp_mqtt_client_reconnect()`
refused: all end in `backoff` and a rebuild after 60 s. Every `REMOTE_STATUS`
line ends in `\r\n`. `peers` is the browser count the
broker publishes on `cp/<serial>/peers` (the status-bar icon, `cp_remote_peers()`).

**Keepalive 60 s.** esp-mqtt pings at half the interval, so every 30 s: a
Cloudflare WebSocket idle for 100 s is closed, and the pings keep it open.

**Not authorized = unpaired.** CONNACK `not authorized` means the token is
gone on crosspad.app (the user removed the board there): `cp_account_revoked()`
drops it, the state is `unlinked` and nothing reconnects until the board is
paired again. If the board was paired again in the meantime the revoke is
ignored and the client is rebuilt with the new token. The broker never answers
5 for anything else: an address over the failed-login limit (possibly shared
with the board behind a NAT) gets CONNACK 3 (server unavailable) or an HTTP 429
on the upgrade, and the board just backs off.

**Per-board cost on Cloudflare.** Idle and online, a board sends one PINGREQ
every ~30 s (keepalive 60 s, see above): 2880 incoming WebSocket messages a
day, which Durable Objects bill at 20:1, so **~144 billed requests/day**. Only
while a browser is connected as well does the broker run a 60 s alarm (it
closes a board silent for more than 90 s, so the browser sees it offline): with
nobody watching, an idle online board costs the pings alone. Commands add one incoming message each (the reply, from the
board) plus the browser's request.

**Queue.** The MQTT task only copies a request into a 5-slot ring (the running
command + 4 waiting) and wakes the main loop; `cp_remote_poll()` runs one command
per pass on the main task, exactly where BLE and SysEx run theirs, and only while
the state is `online`. The ring is dropped on a disconnect and on teardown, and a
request is queued only by the client that is current (a stale client's DATA event
cannot leave a command that runs minutes later). A full ring answers `ERR busy` at
once (`busy=` counts them). The reply is collected in an 8 KiB buffer (the
companion's overflow guard replaces a reply that does not fit by
`ERR response_too_large`, so the buffer is larger than core's `kRemoteReplyMax`
of 4096 B) and `formatRemoteReply()` cuts it after the last whole line with
`ERR truncated`. `done=` counts replies handed to the outbox.

**Reply hand-off.** `cp_remote_poll()` never calls into esp-mqtt: the MQTT task
holds the client's api lock through connect and TLS (up to 10 s), and the main
loop would wait for it. The finished frame goes to a **sender task**
(`cp_remote_tx`, 6 kB PSRAM stack, created with the client, same size class as
the updater's TLS install task) with `xTaskNotifyGive()`; the sender calls the
blocking `esp_mqtt_client_publish(..., qos 1)`, which writes at once. A frame
posted to the MQTT task instead (a user event, or `esp_mqtt_client_enqueue()`)
would wait for the task's next loop pass: the task polls its socket for up to
1 s outside the lock and looks at its queues only afterwards, so about half
the replies would take ~1.0 s instead of ~60 ms. One frame is in flight at a time
(`s_frameBusy`, with `s_frameLen`, set under `s_lock` before the notify; the
sender clears it after the publish and wakes the main loop if more is queued).
`sdkconfig.v1/v2` set `CONFIG_MQTT_USE_CUSTOM_CONFIG=y` with
`CONFIG_MQTT_EVENT_QUEUE_SIZE=8` (the other custom values are esp-mqtt's
defaults) and `cp_remote.cpp` has an `#error` for a regenerated sdkconfig that
brings the 1 back: a publish failing in the sender dispatches DISCONNECTED from
that task while the MQTT task posts its own events, and with a queue of 1 the
second post is dropped (`esp_event_loop_run(0)` handles one event per pass and
`esp_mqtt_dispatch_event()` ignores the post result). With more than one slot
esp-mqtt also keeps the client struct in internal RAM (a few hundred bytes).

**Teardown order and lifetimes.** `teardown()` (worker) nulls `s_client` and
`s_sender` under `s_lock`, so `cp_remote_poll()` (which notifies under the same
lock) can never notify a stopped task or use a dead handle, and drops the
queue. The one-shot `cp_remote_off` task then: sets `s_senderStop` and notifies
the sender; `esp_mqtt_client_stop()`, which itself takes the api lock (to send
DISCONNECT) and then waits for the MQTT task to end. A publish already blocked
on that lock goes first not because the stop releases anything but by
**priority**: the api lock is a FreeRTOS mutex, which hands over to the
highest-priority waiter, and the sender (priority 2) outranks `cp_remote_off`
(priority 1), both on core 0. The teardown then waits for `s_senderAlive` to clear
(the sender sets it as its last act and deletes itself with
`vTaskDeleteWithCaps`, so nothing calls into the client afterwards);
`esp_mqtt_client_destroy()`; waits for `s_pollBusy`; frees the buffers;
releases the lease. The sender is never deleted from outside, only asked to
leave: it may be inside a write holding the api lock.

**Remote-only deny list.** The companion allowlist is what a hand-held BLE or
SysEx peer gets; over the internet `cp_remote_poll()` answers `<id>
ERR not_remote` for every `WIFI_` verb but `WIFI_STATUS` and `WIFI_LIST` (the
allowlist's `WIFI_SET`, `WIFI_FORGET`, `WIFI_FORGET_ALL`, `WIFI_ENABLE`; `UP`,
`DOWN`, `SCAN`, `PING` the companion refuses anyway). The rest of the allowlist
has no firmware upload, file write, bootloader or USB-drive verb. **The whole
UI is reachable:** `ENC_ROTATE`/`ENC_PRESS` and `APP_START` drive the launcher
and every app, Settings included, exactly as over BLE and SysEx -- the scope
the owner chose for the companion; the deny list covers the verbs, not what the
encoder can reach on screen.

**Locking.** esp-mqtt calls the event handler with its own api lock held, and
every blocking `esp_mqtt_client_*` call from another task takes that lock, so the
module's `s_lock` (queue, client and buffer pointers) is never held across such a
call. The only things done under `s_lock` besides bookkeeping are a task
notify and a flag, neither of which waits (so the MQTT task, holding its lock and
waiting for `s_lock` in `on_event()`, cannot be waited on).
The buffers are another matter: `cp_remote_poll()` runs a command outside the
lock, so it sets `s_pollBusy` under `s_lock` and the teardown task waits for it
(after the sender has exited and the client is destroyed, when no handler or frame can touch them) before freeing. A
queue generation counter, bumped on every disconnect and teardown, makes a poll
that spans one drop its result instead of popping a slot that is no longer its own.

**`no_mem` in the Sampler.** The lease is subject to the same four memory
gates as any other; with a kit loaded it is refused (`no_wifi`/`no_mem`) and
retried every minute. **Memory of the client:** only esp-mqtt's 6 kB task stack
is internal; with `SPIRAM_USE_MALLOC` (`ALWAYSINTERNAL` 64 B) its 1 kB input and
4.2 kB output buffers and the outbox items are PSRAM. `cp_remote` itself
allocates one 15 kB PSRAM block (queue 2.6 kB, reply 8 kB, frame 4.1 kB) when
the client is built and frees it after the client is destroyed, so nothing is
held while remote management is off; a failed allocation is reason `no_mem`.
`outbox.limit` is 8 kB (replies not yet acknowledged by the broker; they stay
QoS 1): a full outbox drops the newest reply rather than growing. One frame is
in flight at a time and a PUBACK takes ~60 ms, so it normally holds one reply;
8 kB is a largest reply (`kRemoteReplyMax` 4096 B + id, ~4.1 kB) with room for
a second, short one -- two largest replies unacknowledged at once (a stalled
link) lose the second.

**Image cost** (v2, `build_v2`): esp-mqtt + `cp_remote` took `.flash.text` from
0x1E9570 to 0x1EED50 bytes (+22496 B, measured 2026-10-08; end 0x421E9590
-> 0x421EED70, limit 0x421F0000, 4752 B left); `.ext_ram.dummy` unchanged at
2686944. v1 (`build_v1`): `.flash.text`
end 0x421D70C0, `.ext_ram.dummy` 2621408.
`CONFIG_MQTT_TRANSPORT_SSL` stays on: the secure WebSocket transport depends on
it (without it esp-mqtt refuses `wss://` at runtime).

## Known limitations

- **Touch is not exercised by HIL**: there is no touch-injection verb. The
  page, the keyboard and the fallback dialog were driven by encoder, power
  click and verbs; touch is believed unchanged.
- **Sampler with a kit loaded is refused by design**: it leaves ~71-76 kB of
  PSRAM against a 116736 B gate, and the answer is network mode, not a bug.
  The launcher has ~40 kB of PSRAM above the gate; a feature that takes that
  much in the launcher turns WiFi there into network mode too.
- **GlobalSign Root CA expires 2028-01-28.** The certificate bundle
  (`main/certs/crosspad_roots.pem`) must be regenerated against the live
  chain before then (the file says how); the GTS and ISRG roots cover a chain
  sent without the cross-signature.
- **Cold-start TLS stalls.** On one access point, a ping right after a cold radio
  start occasionally stalls: the board's ACKs and its own segments leave 100-900
  ms late (the server retransmits), the CPU idle and memory fine. About 2 TLS
  timeouts in ~35 cold pings, the slow tail up to 12.5 s; on a warm link none in
  26. Ruled out: BLE (stalls stayed with BLE disabled), WiFi modem sleep,
  CPU load, memory, `ESP_WIFI_RX_IRAM_OPT` (+10.5 kB IRAM, no change). The 20 s
  bound covers it. Root cause needs a second AP.
- **No `WifiFail` value for a failed scan**: `ERR timeout` / `ERR scan`, with
  `WIFI_STATUS fail=` untouched. `NotNetworkMode` exists and is never produced.
- **Arguments have no escapes**: an SSID or password with `"` cannot be set
  with `WIFI_SET` (a UI pick can).
- **An open network was not exercised** on hardware.
- v1 is built but not measured; the gates are v2 numbers.
- Saved passwords are stored in clear, and typed ones linger in RAM buffers
  (the prompt's textarea, the page's candidate) until overwritten.

## Traps found on hardware

- **A wrong WPA3 password fails at the auth step** (reason 202 `AUTH_FAIL`),
  not at the 4-way handshake as on WPA2: it reads like the AP refusing the
  station. Check the password before the STA config (see "WPA2/WPA3
  transition access points").
- **`MALLOC_CAP_INTERNAL` free is not what the driver can fall back to.** It
  counts the 64 kB pool of `SPIRAM_MALLOC_RESERVE_INTERNAL`, which
  `MALLOC_CAP_DEFAULT` requests cannot use, and the driver goes to PSRAM
  first. A gate on it alone lets a Sampler-with-kit start through; the driver
  fails half way (`esf_buf_setup_static: alloc eb fail`, `ESP_ERR_NO_MEM`)
  with internal RAM down to ~15 kB. Gate on all three heaps (see "Memory
  gates").

- **Nothing of WiFi in IRAM or at boot, or the board goes headless.**
  `ESP_WIFI_IRAM_OPT` / `ESP_WIFI_RX_IRAM_OPT` (≈ 18 kB of IRAM text) plus
  `tcpip`, `sys_evt` and `wifi_ctl` created at boot take ≈ 36 kB of internal
  RAM with the radio off: the LCD draw buffer (19200 B, one DMA block) is not
  allocated (`Display LCD init failed - running headless`) and every SD read
  fails with `not enough mem`. Both options stay off, nothing is created at
  boot.
- **The first-lease gate runs before anything is built.** Otherwise a lease
  on a nearly full heap builds the network stack (10.7 kB, never freed) and
  only then hits the radio gate; on a fuller heap `tcpip_init()` can assert
  instead.
- **`WIFI_SET` with a wrong password overwrites the saved one** for that SSID
  (it saves before connecting). After a wrong-password check, `WIFI_SET` the
  right one again. Check with only the test network saved: a second saved
  network is tried next and can mask `fail=auth`.
- **`stack_free=0`** in `WIFI_STATUS` is "no control task yet", not an
  overflow.
- **Our own `esp_wifi_disconnect()`** reports `WIFI_REASON_ASSOC_LEAVE` (8 in
  `esp_wifi_types_generic.h`); `on_disconnected()` drops it. One issued while
  still connecting (a scan's pause) reports 36 (`STA_LEAVING`) instead,
  dropped by the attempt fence (see "Scan").
- **A TLS client is paid in PSRAM.** Linking `esp_http_client` with the full
  certificate bundle costs 192 kB and the board loses its kit list on every
  boot (see "TLS"). Check `xtensa-esp32s3-elf-size -A build_v2/CrossPad.elf | grep
  ext_ram.dummy` against 2949088 after touching the network code.

## HIL scenario `wifi`

`crosspad-hil run wifi` (`tools/bench.py test wifi`, test network credentials from
`CROSSPAD_WIFI_SSID` / `CROSSPAD_WIFI_PASS`; exit 2 without them). Normal boot with BLE
stopped for the first three checks, then one pass through network mode; the
board always leaves in a normal boot with WiFi allowed, the test network saved
and BLE enabled. A run on v2 (2026-10-03), exit 0 in ~21 min:

| Check | Result |
|---|---|
| `connect` | `state=up` within 10 s of `WIFI_SET` |
| `ping` | `ok`, 3562 ms (first lease: DNS + TLS), `status=404` |
| `wrong_password` | `failed` / `auth` (reason 202), `RESET_LOG` unchanged |
| `no_leak` | `int_free` 48171 -> 48171 after 20 `WIFI_UP`/`WIFI_DOWN` cycles |
| `ble_handover` | BLE off while the link is up, back after `WIFI_DOWN` + linger |
| `network_mode` | `leases=1` without `WIFI_UP`, ping `ok` 4417 ms `status=404`, `BLE_STATUS running=0`, `BLE_START` refused |

`rt_glitch` (§7.5): no WiFi `B_sustain` -60.8 dBFS / `C_hammer` -56.5 dBFS,
0 clicks; with `WIFI_UP` held (state=up, rssi -31, BLE handed over) the same
numbers, 0 clicks. Host-side crosspad-hil reads the verbs through
`parse_cdc_reply()` kinds `wifi`, `wifi_ap`, `wifi_net`, `wifi_ping`, `net_mode`.
