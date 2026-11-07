# USB Transfer mode — the SD card as a USB drive

Date: 2026-09-21. Goal: pick "Transfer mode" on the device, the computer sees
the CrossPad's SD card as an ordinary USB drive, drag kit folders onto it,
eject, leave the mode, new kits are there.

## Problem

Kits reach the card two ways today, and both are bad for a user: over CDC with
`tools/fs_transfer.py` (a host tool, one file per round trip, 4096-byte chunks)
or by taking the card out. The device has the hardware for a third way — the
SDMMC card and a USB device controller — and TinyUSB has an MSC class driver;
what has been missing is somewhere to put it. The descriptor sets in use
(MIDI+CDC, MIDI+UAC2) have no endpoints left, and the firmware holds the card
open: three threads share the prebuilt sample player through
`SamplePlayerAccess.hpp`, the kit loader streams off the same card, and FATFS
is mounted at `/sdcard` for the whole run. Two writers on one FAT volume
corrupt it.

## Decisions

**1. A separate boot mode, not a live switch.** Entering it arms a one-shot
flag and restarts. On that boot the firmware brings up the STM link, the
display, LVGL, USB and the card — and starts nothing that opens a file on the
card or takes the engine arena: no sample engine, no kit scan, no kit loader,
no pitched engine, no SD hotplug probe, no BLE. Leaving the mode is another
restart, into whatever profile the settings say. This sidesteps the sharing
problem entirely rather than solving it.

**2. The flag is RTC slow memory, not NVS.** `RTC_NOINIT_ATTR uint32_t`
survives `esp_restart()` and is uninitialised after a power cycle, which is
exactly the lifetime wanted: the mode never persists across a cold boot. It is
read and cleared once, first thing in `app_main()`, so a reboot *out* of the
mode is normal even if it was a panic. Uninitialised RTC RAM could match the
magic by chance, so the test is `magic && esp_reset_reason() == ESP_RST_SW`.
NVS was rejected for the opposite failure: a stale key after a crash strands
the board in transfer mode, and it costs a flash write per entry.

**3. Exclusive ownership of the card.** In transfer mode FATFS is never
mounted. `bsp_sdcard_raw_init()` runs the same host/slot configuration and the
same 4-bit → 1-bit retry as `bsp_sdcard_mount()`, stopping after
`sdmmc_card_init()` — no VFS, no `f_mount`. Sector size and count come from the
card's CSD; READ10/WRITE10 are `sdmmc_read_sectors` / `sdmmc_write_sectors`.
`bsp_sdcard_mount()` and the `sd_hotplug` worker job are both skipped for the
whole boot. **Each retry must restart the host**: a card that fails halfway
leaves the peripheral unable to update its clock (`0x107`), and the mounted path
only gets away without a deinit because `esp_vfs_fat_sdmmc_mount()` performs one
on its way out of a failure. Without it the 1-bit fallback failed on every boot
of a board whose card needs that fallback.

**4. Descriptor: MSC + CDC, no MIDI.** Profile nibble `0x2` in `bcdDevice`
(`0x0500 | rev<<4 | profile`), beside CDC `0x1` and audio `0x0`.

| Interface | Class | Endpoints |
|---|---|---|
| 0, 1 | CDC ACM (with its own IAD) | `0x81` notif, `0x02` out, `0x82` in |
| 2 | MSC BOT / SCSI | `0x03` out, `0x83` in |

Three IN endpoints plus EP0, inside the S3 DWC2 budget of EP0 + 5 (the audio
profile is the one that runs out). CDC stays so the bench, `crosspad-hil`, the
`USB_MSC 0` exit verb and OTA keep working while the card is on the host; MIDI
goes, because nothing plays in this mode. `crosspad-hil` infers `usb_mode`
from which ports enumerate rather than from the profile nibble, so it reports
a transfer board as `default` and every CDC verb keeps working — nothing in
either repo misreads the new nibble.

**5. The `tud_msc_*` callbacks are ours; esp_tinyusb's storage layer is dropped
from the build.** `CONFIG_TINYUSB_MSC_ENABLED=y` also compiles
`tusb_msc_storage.c`, a FATFS-backed layer with two behaviours this firmware
must not have: its `tud_msc_start_stop_cb` answers the host's eject by mounting
the card, and that mount calls **`f_mkfs`** when `f_mount` reports
`FR_NO_FILESYSTEM` or `FR_INT_ERR`; and its `tud_umount_cb` — called on bus
reset as well as unplug, in *every* profile — dereferences a storage handle
that only exists in transfer mode.

The first attempt was `-Wl,--wrap`, the technique already used for
`lv_malloc_core`. **It does not work here**, and the build says so only if you
look: `tud_umount_cb`, `tud_msc_inquiry_cb` and `tud_msc_start_stop_cb` have
weak definitions in the very translation units that call them (`usbd.c`,
`msc_device.c`), and ld wraps only *undefined* references. The wrappers linked,
were never referenced, and `--gc-sections` dropped them — they appeared in the
map's discarded-sections list and nowhere else. Only `read10`/`write10`, which
have no in-TU definition, were actually redirected.

So `main/CMakeLists.txt` filters `tusb_msc_storage.c` out of the component's
`SOURCES` and `main/usb_transfer.cpp` defines all seven callbacks against the
raw card. A strong definition in `libmain` does beat the weak default in
`msc_device.c` — `objdump` confirms the class driver's call sites land in
`libmain`. A future rename of that file fails the build on duplicate symbols,
which is the right failure mode. Nothing in the path ever mounts or writes a
filesystem.

**6. Memory.** `CONFIG_TINYUSB_MSC_BUFSIZE=2048` (4 sectors).
`CFG_TUD_MSC_EP_BUFSIZE` is a static `TUD_EPBUF_DEF` inside the MSC class
driver, so it costs **2048 B + 64 B (`_mscd_itf`) of internal DRAM in every
profile**, transfer or not. Owning the callbacks means there is no second
buffer: the only per-mode allocation is one `sdmmc_card_t` (~200 B of heap).
2048 rather than 8192 because internal RAM is the scarce one here (with BLE up
the launcher sits at ~8 kB free) and USB full-speed bulk caps the link at about
1.1 MB/s regardless — four sectors per SCSI data chunk is already more than the
wire can take. The engines never start in this mode, so the 5424 kB PSRAM arena
is simply never allocated.

**7. Screen state machine.** One full-screen LVGL screen, no launcher, no
status bar, no apps. The MSC callbacks run on the TinyUSB task and touch only
atomics; a 250 ms `lv_timer` on the LVGL thread reads them and rewrites the
labels. Nothing touches the card from the LVGL thread.

```
no card ─────────────────────────► NO CARD      (MSC is not enumerated at all)
                 tud_mounted() ─┐
WAITING ◄────────────────────────┴─► MOUNTED ──► BUSY (a write within 2 s)
   ▲                                   │  ▲        │
   └────────── host gone ──────────────┘  └────────┘
                      host eject (START STOP UNIT, load_eject, start=0)
                                   └──────────────► EJECTED (safe to exit)
```

Exit is reachable three ways — the touch button, the encoder (the button joins
`encoderTop_group`) and the power-button click (`ui_back_push`, so an open
confirm dialog is cancelled first). Exit asks for confirmation only in BUSY:
"the computer may still be writing".

**8. Entry points.** Settings → **USB** → *SD card as USB drive*: a row in
crosspad-gui's USB page, drawn only where `IGuiPlatform::usbTransferSupported()`
is true (Arduino and the PC simulator keep the default `false`), with a confirm
dialog that says the device restarts and audio stops; confirming calls
`IGuiPlatform::requestUsbTransfer()`; and the CDC verb `USB_MSC 0|1` (bare
`USB_MSC` reports). **No SysEx or BLE entry point**: exposing storage to a
host must not be possible from the MIDI+UAC2 profile without the user at the
device, which is the same reasoning as the USB serial guard.

## Not in scope

- Live switching without a reboot.
- Anything over BLE or SysEx.
- Writing to the card from the firmware while the host owns it.
- Formatting, partitioning or repairing the card; the firmware never writes a
  filesystem structure, only the sectors the host sends.
- A kit index refresh short of the reboot that leaving the mode already does.

## Verification (done, 2026-09-21, a v2 board)

Both revisions build clean. On the board: `USB_MSC 1` enumerated `bcdDevice
5.22`, three interfaces, endpoints exactly as tabled; the card appeared as
`/dev/sdd`, 29.1 GiB, `CrossPad` / `SD Card`, one vfat partition with all 81
kits. 668 kB/s read (a 9.26 MB kit, cold), 495 kB/s write (`dd` 4 MB,
`conv=fsync`) — that card ran on the 1-bit bus. A file and a kit folder written on the
host were on `/sdcard` after `USB_MSC 0`. A UDisks2 `Drive.Eject` moved the
state to `ejected` with the board still on USB. `bench.py test smoke` passed
afterwards, `bench.py ready` left the board in the CDC profile with a kit.

`bench.py check` and `bench.py ready` learned the mode: `check` reports it as a
failure with the command that leaves it, `ready` leaves it on its own.
