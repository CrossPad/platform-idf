# USB Transfer mode — the SD card as a USB drive

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

Pick Transfer mode on the device, the computer sees the CrossPad's SD card as an
ordinary USB drive, drag kit folders onto it, eject, leave the mode. No host
tool, and nothing to learn: the alternative was `tools/fs_transfer.py` over CDC
(one file per round trip) or pulling the card out of the socket.

Design and the decisions behind it:
[docs/design/2026-09-21-usb-transfer-mode-design.md](../design/2026-09-21-usb-transfer-mode-design.md).

## It is a boot mode

Entering it arms a one-shot flag and restarts. On that boot the firmware brings
up the STM link, the display, LVGL, USB and the card — and **starts nothing that
opens a file on the card or takes the engine arena**: no sample engine, no kit
scan, no kit loader, no pitched engine, no SD hotplug probe, no BLE, no OTA/STM
overlays. That is the whole reason it is a boot mode: three threads share the
prebuilt sample player (`SamplePlayerAccess.hpp`), the kit loader streams off
the same card, and FATFS is mounted at `/sdcard` for a normal run. Two writers
on one FAT volume corrupt it, so the mode simply never creates the second one.

Leaving is another restart, back into whatever USB profile the settings say.

The flag is an `RTC_NOINIT_ATTR` word in `main/usb_transfer.cpp`: it survives
`esp_restart()` and is uninitialised after a power cycle, so **a cold boot is
always normal**. `usb_transfer_boot_claim()` reads and clears it as the first
statement of `app_main()`, and only honours it when `esp_reset_reason()` is
`ESP_RST_SW` — uninitialised RTC RAM can match the magic by chance, and a board
stranded in transfer mode would have no obvious way out. A panic, a watchdog or
an external reset therefore always comes back normal too.

## Entering and leaving

| Where | How |
|---|---|
| Device | Settings → **USB** → *SD card as USB drive · Start*, then confirm |
| Device | The **Exit and restart** button on the transfer screen — touch, encoder, or a power-button click |
| Host | `USB_MSC 1` over CDC; `USB_MSC 0` to leave; bare `USB_MSC` reports |
| Bench | `tools/bench.py ready` leaves the mode on its own |

There is deliberately **no SysEx and no BLE entry point**. Handing a host the
storage must need the user at the device, which is the same reasoning as the
USB serial guard: the MIDI+UAC2 profile must not be able to open the card.

```
USB_MSC: active=1 card=1 state=mounted size_mb=29820 read_kb=2837 write_kb=0
```

`state` is `no_card | waiting | mounted | busy | ejected`.

Firmware packages for the A/B switch go in `crosspad/firmware/` on the card
(alongside `kits/`) — see [docs/subsystems/firmware-library.md](firmware-library.md).

## The descriptor

Profile nibble `0x2` in `bcdDevice` (`0x0500 | rev<<4 | profile`), beside CDC
`0x1` and audio `0x0` — a v2 board in transfer mode enumerates as `5.22`.

| Interface | Class | Endpoints |
|---|---|---|
| 0, 1 | CDC ACM (its own IAD) | `0x81` notif, `0x02` out, `0x82` in |
| 2 | MSC BOT / SCSI | `0x03` out, `0x83` in |

Three IN endpoints plus EP0, inside the S3 DWC2 budget of EP0 + 5. MIDI is
dropped — nothing plays here. CDC stays, so the bench, `crosspad-hil`, the
`USB_MSC 0` exit verb and OTA all keep working while the card is on the host.

**The PID stays `0x303A:0x3456`.** `crosspad-hil` finds the command port by
VID:PID and would lose the board otherwise, leaving no way out from the host.
It also infers `usb_mode` from which ports enumerate, not from the profile
nibble, so it reports a transfer board as `default` — `tools/bench.py check`
asks `USB_MSC` instead and names the mode itself.

The drive identifies as vendor `CrossPad`, model `SD Card`
(`/dev/disk/by-id/usb-CrossPad_SD_Card_<mac>-0:0`).

## The MSC callbacks are ours, not esp_tinyusb's

`CONFIG_TINYUSB_MSC_ENABLED=y` also compiles esp_tinyusb's
`tusb_msc_storage.c`, a FATFS-backed storage layer this firmware must not have:

- its `tud_msc_start_stop_cb` answers the host's eject by mounting the card on
  FATFS, and that mount calls **`f_mkfs`** when `f_mount` reports no filesystem
  or an internal error. Formatting someone's kit card after an eject is not a
  risk worth taking;
- its `tud_umount_cb` dereferences a storage handle that is null in every
  profile but this one, and `tud_umount_cb` fires on **bus reset** as well as
  unplug. Left linked, an ordinary unplug on a normal boot is a panic.

**`-Wl,--wrap` cannot fix this.** Those callbacks have weak definitions in the
very translation units that call them (`usbd.c`, `msc_device.c`), and ld only
redirects *undefined* references — the wrappers link but are never called, and
`--gc-sections` drops them without a word. The proof is in the map file: a
`--wrap` build had `__wrap_tud_umount_cb` in the discarded-sections list.

So `main/CMakeLists.txt` drops `tusb_msc_storage.c` from the component's
sources and `main/usb_transfer.cpp` defines the `tud_msc_*` callbacks itself,
against the raw card. A strong definition in `libmain` does beat the weak
default in `msc_device.c`; `objdump` shows the class driver's call sites
landing in `libmain`. If a future esp_tinyusb renames that file, the filter
matches nothing and the build fails on duplicate symbols, which is the failure
mode to want.

Nothing in this path ever mounts a filesystem or writes one: the firmware only
moves the sectors the host asks for.

## Exclusive ownership of the card

`bsp_sdcard_raw_init()` (`components/bsp/crosspad/bsp_sdmmc.c`) runs the same
host and slot configuration as `bsp_sdcard_mount()`, including the 4-bit → 1-bit
retry, and stops after `sdmmc_card_init()` — no VFS, no `f_mount`. It is called
from the transfer profile's TinyUSB install, **before** `tinyusb_driver_install()`,
because the MSC callbacks are answered from the first enumeration.

**Every attempt starts from a fresh host.** A card that fails halfway leaves the
peripheral in a state where the next `sdmmc_host_init_slot()` cannot even update
the clock (`0x107`, `ESP_ERR_TIMEOUT`), so the 1-bit retry must
`sdmmc_host_deinit()` first. The mounted path gets this for free because
`esp_vfs_fat_sdmmc_mount()` deinits on its way out of a failure; the raw path
without it failed every time on a board whose card needs the
1-bit fallback.

If the card does not come up at all, the transfer profile is not installed: the
board enumerates plain CDC + MIDI, and the screen says "No SD card" with Exit as
the only control.

## The screen

One full-screen LVGL screen, no launcher, no status bar, no apps
(`main/gui/usb_transfer_screen.cpp`, branched in `LVGLTask`): title, one line of
instruction, the card size, a live state line and the megabytes moved.

```
no card ─────────────────────────► NO CARD      (MSC is not enumerated at all)
                 tud_mounted() ─┐
WAITING ◄────────────────────────┴─► MOUNTED ──► BUSY (a write within 2 s)
   ▲                                   │  ▲        │
   └────────── host gone ──────────────┘  └────────┘
                      host eject (START STOP UNIT, load_eject, start=0)
                                   └──────────────► EJECTED (safe to exit)
```

The MSC callbacks run on the TinyUSB task and touch only atomics; a 250 ms
`lv_timer` on the LVGL thread is the single reader. Nothing goes near the card
from the LVGL thread.

Exit is reachable by touch, by the encoder (the button joins `encoderTop_group`,
free because there is no launcher) and by the power-button click
(`ui_back_push`, so an open confirm dialog is cancelled first). It asks for
confirmation only in BUSY: "the computer may still be writing".

## Memory

| What | Where | When |
|---|---|---|
| `_mscd_epbuf` 2048 B + `_mscd_itf` 64 B | internal DRAM, static | **every profile**, transfer or not |
| `sdmmc_card_t` ~200 B | internal heap | transfer mode only |

`CONFIG_TINYUSB_MSC_BUFSIZE=2048` (four sectors) is `CFG_TUD_MSC_EP_BUFSIZE`, a
static allocation inside the class driver — it costs its size in internal RAM
in every build, which is why it is not 8192. USB full-speed bulk caps the link
at about 1.1 MB/s regardless, so four sectors per SCSI data chunk is already
more than the wire can keep busy. The engines never start in this mode, so the
engine arena, which holds only what the engines ask for, takes no PSRAM.

## Measured (v2, 29.8 GB card in 1-bit)

| | |
|---|---|
| Enumeration | `/dev/sdd`, 29.1 GiB, `CrossPad` / `SD Card`, one vfat partition |
| Read (`cp -r` of a 9.26 MB kit, cold) | 668 kB/s |
| Write (`dd` 4 MB, `conv=fsync`) | 495 kB/s |
| Round trip | a file and a kit folder written on the host were on `/sdcard` after `USB_MSC 0` |

A card on the 4-bit bus is untested here. `USB_MSC` reports the bus the card came up on
(`width= freq=`, kept at the mount, 0 0 without a card).

## Traps

| Trap | What you see | Why |
|---|---|---|
| `udisksctl power-off` takes the whole board off USB | every CDC verb "no reply", `usb_mode=unknown`, the STM bridge VCP still there | that call removes the *USB device*, not just the drive — the CDC port goes with it. `crosspad-hil console --reset` (DTR/RTS on the STM bridge) reboots the ESP and it re-enumerates. Eject properly instead: `gdbus call --system --dest org.freedesktop.UDisks2 --object-path /org/freedesktop/UDisks2/drives/CrossPad_SD_Card_<mac> --method org.freedesktop.UDisks2.Drive.Eject "{}"` — that is a SCSI START STOP UNIT and the board stays on USB |
| `bench.py check` says the profile is `default` while the card is on the host | crosspad-hil names the profile after the ports that enumerate, and this one keeps its CDC | `check` asks `USB_MSC` as well and reports `usb transfer mode` separately |
| `crosspad-hil cdc USB_MSC` refuses with `UNKNOWN_VERB` | `USB_MSC is not in knowledge/cdc.yaml` | the verb is not in the crosspad-hil catalog yet; pass `--expect USB_MSC:` (or `--expect OK` for the setters) |
| Scenarios fail as bench problems | no kit, no engine, `KIT_STATUS current=-1` | that is the mode working as designed; `bench.py ready` leaves it |

## Not in scope

Live switching without a reboot; any path over BLE or SysEx; writing to the card
from the firmware while the host owns it; formatting, partitioning or repairing
the card; a kit-index refresh short of the reboot that leaving the mode already
performs. Windows and macOS are untested.
