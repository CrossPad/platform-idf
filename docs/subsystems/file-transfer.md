# Assets Flashing and File Transfer over CDC

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

App OTA writes the app partition and nothing else, so a board kept up to date
with `tools/ota_flash.py` keeps whatever `assets` image its last UART flash left
behind — every icon and image (PNG), the on-device docs (`main/docs/*.md`) and
`stm32_fw.bin`, the bundled STM32 image (`main/collect_assets.cmake`; the fonts
are compiled into the app image, not stored here) — a board can run STM 1.4
and still report `bundled=1.3`. MSC exists only as
[USB transfer mode](usb-transfer-mode.md), a separate boot that shows the SD
card as a USB drive and never the `assets` partition; the normal profiles have
no endpoints left for it, so both features ride the CDC control port
(`0x303A:0x3456`) that already carries OTA.

Device side: `main/file_transfer.cpp` (+ `include/file_transfer.h`).
Host side: `tools/fs_transfer.py` — same VID:PID auto-detect and MIDI
mode-switch fallback as `ota_flash.py` (it imports them), exit 0/1/2 like
`crosspad-hil`.

```bash
python3 tools/fs_transfer.py assets                       # build_<rev>/assets.bin (idf.py board names <rev>)
python3 tools/fs_transfer.py assets build_v2/assets.bin   # per-rev build dir
python3 tools/fs_transfer.py roots                        # mounts + usage
python3 tools/fs_transfer.py ls   /sdcard/crosspad/kits
python3 tools/fs_transfer.py push kit.json /sdcard/crosspad/kits/kit.json
python3 tools/fs_transfer.py pull /sdcard/rec.wav out.wav
python3 tools/fs_transfer.py rm   /sdcard/rec.wav
python3 tools/fs_transfer.py mkdir /sdcard/crosspad/kits/NEW
```

Mounts, and nothing else is reachable — a path outside them is refused, and so
is any path containing `..`:

| Root | Partition | Notes |
|------|-----------|-------|
| `/assets` | `assets`, LittleFS 1 M | mounted at boot; what `assets` reflashes wholesale |
| `/spiflash` | `storage`, FAT 4 M | nothing else uses it; mounted (and formatted) on first use |
| `/sdcard` | SD card | absent when no card is in |

**Framing is a mode switch, not an escape.** `ASSETS_BEGIN <size> <crc>` and
`FS_PUT <size> <crc> <path>` announce a byte count; the pipe leaves newline
framing for exactly that many bytes and returns to text afterwards. It is the
same exemption `cdc_dispatch_lines()` already makes for OTA and the same place
makes it (`fx_stream_active()` beside `ota_is_active()`), so there is one
decision point for framing rather than two. SLIP/COBS was rejected: it expands
the payload and costs CPU on both ends to solve a problem a known length
already solves. The RX callback blocks on a full `app_queue` during a stream
for the same reason it does during OTA — a dropped chunk is unrecoverable, and
the host is lockstep so the queue cannot run away.

**Nothing that touches a filesystem runs on the drain loop.** All of it — the
FAT mount, `fopen`, `readdir`, the reads and the writes — is on a worker task
(`fx_worker`, prio 4, created on first use, 5 kB stack). That stack has to be
internal RAM — the worker writes SPI flash, and a task on a PSRAM stack does
not survive the cache being disabled for that — so with BLE running (launcher
at ~8 kB internal free, largest block under 4 kB) the worker cannot be created
and every verb answers `FS_ERR no_worker int_largest=<n> need=5120`.
`BLE_STOP` first; `crosspad-hil run fs_transfer` reports that state as exit 2. The drain loop parses,
memcpys into one of two 4 KB PSRAM slots, and posts; the worker writes and
answers. `FS_PUT` flips the framing *synchronously* and posts the open, so the
framing never depends on when the worker gets scheduled; a failed open puts it
back and answers `FS_ERR open_<errno>`.

**The assets image is staged whole in PSRAM and CRC-checked before anything is
erased**, then written, then read back and CRC-checked again. A stream that
dies halfway costs nothing at all; only a power loss inside the erase+write
window (~10 s) can damage the partition. The image must also carry the
LittleFS superblock magic unless `FORCE` is given — that is what stops a
firmware binary handed to the wrong subcommand. On success the device restarts
(the launcher and LVGL hold icon state that a remount would not refresh).

**If it does fail mid-write**, the assets partition is not needed to boot:
`esp_vfs_littlefs_register()` in `app_main` has `format_if_mount_failed`, so
the board comes up with blank tiles and a missing bundled STM image and every
CDC verb still answers — including this one, so the recovery is to run
`fs_transfer.py assets` again. UART (`idf.py flash`, which writes
`0x720000 build_<rev>/assets.bin`) is the fallback, not the requirement.

A file `push` needs no staging: it streams straight to `<path>.part` and is
renamed onto the target only once the CRC matches, so a target file is never
half-replaced. `pull` sends its CRC in the *trailer* (`FS_OK <crc32hex>`), not
the header — a header CRC would mean reading a 4 MB WAV twice. CRC32 is
`esp_crc32_le(0, …)`, which is `zlib.crc32`.

| Verb | Reply |
|------|-------|
| `ASSETS_BEGIN <size> <crc32hex> [FORCE]` | `ASSETS_READY`, then payload, `OK n/size` per 4096 B, then `ASSETS_OK` (reboots) or `ASSETS_ERROR <reason>` |
| `FS_PUT <size> <crc32hex> <path>` | `FS_READY`, then payload, `OK n/size` per 4096 B, then `FS_OK <crc32hex>` or `FS_ERR <reason>` |
| `FS_GET <off> <len> <path>` (`0 0` reads the whole file) | `FS_DATA <len>`, payload, `FS_OK <crc32hex>` (or `FS_TRUNC <got>/<len>`) |
| `FS_LIST <dir>` | rows `FS_LIST: F\|D <size> <name>`, then `FS_END` |
| `FS_STAT <path>` | `FS_STAT <size> F\|D` |
| `FS_ROOTS` | rows `FS_ROOTS: <root> <total> <used>` (`- -` when unmounted), then `FS_END` |
| `FS_MKDIR <path>`, `FS_DELETE <path>` | `OK` or `FS_ERR <reason>` (`FS_ERR mkdir_17` = already exists; `FS_DELETE` removes a file or an empty directory) |
| `SETTINGS_EXPORT [path]`, `SETTINGS_IMPORT <path>` | `OK <path>` at once, the work runs on the LVGL thread; `SETTINGS_STATUS` → `SETTINGS: busy= op= ok= err= path=` is the result. The file is the Settings → Backup JSON (`SettingsIo.cpp`), default `/sdcard/crosspad-settings.json` |

**A command ends at its terminator, nowhere else.** `cdc_dispatch_lines()`
does not treat a short USB packet as the end of a line: a host may split one
write (Web Serial in Chromium hands the kernel a 46-byte write as 36 + 10
bytes), and the first piece would run as a command with a cut-off path while
the second came back as the echo of an unknown one. Every host terminates with
LF, so a partial line waits in the line buffer for the rest. The echo of an unhandled line stays: it is how a stray fragment shows
up in a trace.

**A line holds at most 223 bytes** (`CDC_LINE_MAX`, `main/include/cdc_line.h`),
enough for `FS_GET <off> <len> <path>` with the longest path `path_ok()`
accepts (159 characters; a `static_assert` in `file_transfer.cpp` keeps the
two in step). A longer line is dropped whole and answered `ERR too_long` once,
at its terminator. Until 2026-11-07 the buffer was the 128-byte USB read size
and an overflow only emptied it: the bytes after byte 128 were collected as a
new line, so a long `FS_PUT` path came back as the echo of its tail, glued to
the next reply, and a tail that began with a verb would have run. The HIL verbs
copy a line into 160 bytes and answer `ERR too_long` above that rather than
run a cut-off argument.

Every row of a multi-line reply carries its verb as a `NAME:` prefix, the way
`MEMBLK:` and `APPVER:` do. That is not decoration: `crosspad-hil`'s reader keeps
only lines starting with the prefix or the terminator, so unprefixed rows are
dropped and the listing arrives empty.

The chunk size is 4096 on both sides and must stay that way: the device acks on
4096-byte boundaries, so a short chunk anywhere but the last leaves both ends
waiting. A stream that stalls for 15 s is abandoned with `FS_ERR timeout`
(`fx_abort_timeout()`; OTA: 10 s, `OTA_ERROR timeout`, `ota_poll_timeout()`).
The drain loop runs both checks on their own timestamps on every pass with
`app_queue` empty, not only when its receive times out: companion wakes
(the crosspad-hil MIDI hub's HOST_INFO SysEx every 2 s, BLE, remote) post
empty messages, and before this a broken-off stream with a host attached
never timed out.

**There is no CDC at all in the USB-audio profile**, so none of this works
there; `ota_flash.py`'s MIDI fallback (SysEx `F0 7D 1B 01 F7`) switches the
device back to CDC+MIDI first, and `fs_transfer.py` inherits it. Nothing here
is reachable over SysEx `0x1D` — a bulk path through 7-bit MIDI would be a
different protocol, not this one.
