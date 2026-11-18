# USB transfer mode and file transfer

Contributor notes: the card as a USB drive, assets flashing, files over CDC. The short version of what a change in this area
must not break; the pages it links to have the detail.

## USB Transfer Mode (the SD card as a USB drive)

A third USB profile, **MSC + CDC** (`bcdDevice` profile nibble `0x2`), and a
**boot mode**: Settings → USB → *SD card as USB drive*, or `USB_MSC 1` over CDC, arms a one-shot
`RTC_NOINIT_ATTR` flag and restarts. That boot brings up display, LVGL, USB and
the card raw (`bsp_sdcard_raw_init()`, no FATFS) and starts **no engine, no kit
scan, no kit loader, no BLE, no SD hotplug** — two writers on one FAT volume
corrupt it, so the second one is never created. Leaving is another restart; the
flag never survives a power cycle, and only `ESP_RST_SW` honours it. One
full-screen LVGL screen with the live state and an Exit reachable by touch,
encoder and power click. **The `tud_msc_*` callbacks are ours**
(`main/usb_transfer.cpp`): esp_tinyusb's `tusb_msc_storage.c` `f_mkfs`-es a card
it cannot mount and panics on unplug in every other profile, and `-Wl,--wrap`
cannot reach those callbacks (weak definitions in the TU that calls them), so
`main/CMakeLists.txt` drops that file from the build. PID stays `0x3456` or the
host tools lose the board. No SysEx/BLE entry point, on purpose. Cost: 2112 B of
internal RAM in every profile (`CFG_TUD_MSC_EP_BUFSIZE=2048`). Measured 668 kB/s
read, 495 kB/s write on a card on the 1-bit bus. Mechanism, state machine, traps:
[docs/subsystems/usb-transfer-mode.md](../subsystems/usb-transfer-mode.md).

## Assets Flashing and File Transfer over CDC

Device `main/file_transfer.cpp`, host `tools/fs_transfer.py`
(`assets | roots | ls | push | pull | rm | mkdir`), mounts `/assets`, `/spiflash`,
`/sdcard` only. Framing is a mode switch for an announced byte count, decided in
`cdc_dispatch_lines()` beside OTA. The assets image is staged whole in PSRAM and
CRC-checked before the erase. All filesystem work runs on `fx_worker` (5 kB
**internal** stack — under BLE it cannot be created and verbs answer
`FS_ERR no_worker …`; `BLE_STOP` first). Chunk size 4096 on both ends, fixed.
Test: `crosspad-hil run fs_transfer [--assets build_v2/assets.bin]`.
Protocol table and design: [docs/subsystems/file-transfer.md](../subsystems/file-transfer.md).
