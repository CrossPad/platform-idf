# STM32 In-Field Update

> Source of truth for this subsystem. `CLAUDE.md` carries only the summary; when behaviour changes, update this file and the summary together.

The ESP reflashes the STM32 co-processor itself, over the UART link, from an
image it carries — so a firmware that needs a newer STM (a new register, a
protocol bump) ships the STM with it instead of asking for an ST-Link.

- **Image**: `main/stm32_fw/stm32_fw.bin` is the CrossPad_STM32_r2 build this
  firmware was written against. `collect_assets.cmake` stages it into LittleFS
  as `/assets/stm32_fw.bin` (configure time —
  `idf.py -B build_<rev> -DSDKCONFIG=sdkconfig.<rev> reconfigure` after
  replacing it, and it travels in the `assets` partition, which OTA does not
  touch). Every image carries a descriptor at offset `0x100` (`CPFW`, major,
  minor, protocol); nothing is parsed from filenames.
- **Board gate**: the descriptor carries the PCB revision the image was
  built for (`fw_desc.pcb`, offset 0x108, 0 = unknown on pre-1.3 images);
  the STM reports the *board* in `REG_IDENT_PCB_REV` (OTP, compile-time
  default when blank) and its own build target in `REG_IDENT_FW_PCB`
  (proto 0x000F). A bundle for another board is never `due`, the boot
  prompt says so and offers nothing, Settings shows "v1.3 for r21, board
  r20" without a button, and `STM_FW_UPDATE` answers
  `ERR STM_FW ESP_ERR_NOT_SUPPORTED` unless the path is followed by `FORCE`.
  Verified with a crafted r21 image in LittleFS.
- **Policy**: `stm32_fw_update_check()` runs right after `bsp_stm32_init()`
  and marks an update `due` when the bundle is newer (major.minor) or speaks
  another protocol. Nothing installs on its own on a board with a display:
  once the launcher is up, `stm_fw_overlay` asks the user (Update/Later; under
  20 % battery with no charger it only says so and offers no update). Only a
  headless board runs `stm32_fw_update_auto()`, late in boot, which defers
  under the same battery rule. A bundle
  *older* than the running STM firmware is never `due`, even when its
  protocol differs — the field-firmware library (`docs/design/
  2026-09-21-firmware-library-ab-switch-design.md`) lets a device switch
  between installed firmware packages, and a switch to an older package
  bundling an older STM image must not drag the STM32 backwards with it.
  `STM_FW_UPDATE … FORCE` bypasses `due` entirely and still installs an
  older bundle on request. ~10 s, logged under `STM32_FW`. Off-device:
  `STM_FW_INFO` (running vs bundled) and `STM_FW_UPDATE [path]` — any path,
  so an SD card file works too.
- **Firmware-switch warning**: `fw_install_start()` (`main/fw_slots.cpp`)
  compares an installing package's declared `stmProtoMin` against the
  STM32's currently running protocol (`stm32_hal_get_link_info()`); when the
  package expects an older protocol than what is running, `FW_INSTALL`
  answers `OK warn=stm_proto_older` instead of a plain `OK` and
  `fw_install_stm_proto_older()` (`main/include/fw_slots.h`) exposes the same
  fact for the GUI confirm dialog to show a line ("Built for an older pad
  controller protocol — it may not work."). This is a heads-up at install
  time only — it never refuses the install, and it says nothing on its own
  about whether that package's bundled STM image would actually be applied:
  that is still decided by `due` above, which the auto-update path already
  refuses for an older bundle.
- **Mechanism** (`components/stm32_hal/stm32_fw_update.c` ↔
  `CrossPad_STM32_r2/app/src/fw_update.c`): the G0B1's ROM bootloader is
  unreachable from the ESP (its USART/I2C pins are on ADC, SPI2 and the
  encoder), so the STM app carries a RAM-resident flasher. `REG_CMD_FW_UPDATE`
  (or SysEx `F0 7D 31 F7`) arms it; it masks IRQs, switches I2C1 off (a
  slave left ACKing with IRQs masked would clock-stretch the shared bus),
  polls LPUART1 and programs 2 KiB pages. Page 0 is erased first and written
  last, after the whole-image CRC32 matched, so an interrupted update leaves
  an empty vector table and the G0 empty-check boots ROM DFU on the next
  power-up — recoverable over USB, not a brick. `'X'` aborts (reset).
- **Flasher rules**: the flasher must be `noinline` and everything it calls
  `__RAM_FUNC` — inlined into its caller it runs from the flash it is
  erasing, through veneers. `hiwdg.Instance = IWDG` must be set before the
  first `HAL_IWDG_Refresh()`: a refresh through a NULL `Instance` writes its
  key to address 0, the boot alias of flash, and wedges the flash controller
  (`PGSERR|PGAERR`, CFGBSY stuck). The flasher still clears the flags and
  applies the CFGBSY unstick (dummy store while locked — never a CR write,
  that bus-faults) before unlocking.
- **UI**: `main/gui/stm_fw_overlay.cpp` — the decision is the user's. The
  pads show the same progress bar ESP OTA uses (`PadAnimator::startProgress`
  / `playSuccess` / `playError`), so a face-down unit still tells you where
  it is; headless boards get the same bar around `stm32_fw_update_auto()`.
  Settings → Programming shows running vs bundled and offers **Update**
  (newer) or **Reflash** (same version — the recovery for a co-processor
  that misbehaves); it goes through `ISettingsUI::getCoprocFwInfo()` /
  `requestCoprocFwUpdate()` (core 0.5.0) so crosspad-gui stays portable, and
  the button lands in the same overlay flow. A whole update takes ~10 s on
  the STM link (the on-device prompt promises "about 15 s"). Once the
  launcher is up (`LoadMainScreen` → `stm_fw_overlay_on_main_screen()`), a
  `ui_modal_confirm` shows running vs bundled version with Update/Later;
  progress is a panel on `lv_layer_top()`, the update runs in its own task;
  failure shows the error and the recovery (retry, or USB DFU +
  `tools/dfu_flash.py` from the CrossPad_STM32_r2 repo when the STM stays
  dead) with Retry/Close. A modal
  raised from inside another modal's callback loses focus — queue it with
  `lv_async_call`. Headless boards update without asking.
- **Browser flasher**: `tools/webflasher/index.html` (serve it statically —
  any host, GitHub Pages). Chrome on a PC or Android phone, no computer
  needed: ESP OTA over CDC (Web Serial / WebUSB polyfill), ESP recovery via
  the ROM bootloader through the STM bridge (esptool-js), STM32 over USB DFU
  (vendored webdfu, DfuSe, page 0 erased first / written last). Only the
  page load and the protocol code are testable without a phone — the
  serial/USB choosers cannot be driven from DevTools.
- **Bench**: the STM's USB (console + DFU) is dead while it flashes, so a
  console tail loses the update phase; judge by `STM_FW_INFO` afterwards.
  `pyocd commander -t cortex_m --connect attach` reads `FLASH_SR`
  (`0x40022010`) and the PC without resetting. `EN_3V3` defaults high in the
  STM's GPIO init so an STM reset does not power-cycle the ESP.

## ROM DFU over USB from a browser

The G0B1 ROM bootloader's DFU functional descriptor says `wTransferSize`
1024. A 2048-byte DNLOAD is accepted on the wire, then every GETSTATUS
answers garbage and the bootloader is wedged until an SWD reset
(`st-flash --reset write main/stm32_fw/stm32_fw.bin 0x8000000`); dfu-util
honours 1024 and works. `tools/webflasher` and the browser companion write
1024-byte pieces, page 0 erased first and written last, and can send the DfuSe leave
so the STM boots without a replug — the ESP re-syncs the link by itself.
Chrome on Linux exposes no `interfaceName` for the alternates, and reading the
DfuSe map through GET_DESCRIPTOR wedges the ROM the same way; the companion
uses the known map for alternate 0 instead.

**Leaving the ROM is clean only when the STM entered it itself.** `STM_DFU`
goes through `boot_request_dfu()`, which leaves `BOOT_DFU_MAGIC` in
`TAMP->BKP0R`; after the DfuSe leave the app finds the return marker and does
one clean `NVIC_SystemReset()`. A bootloader entered cold — empty flash (the
G0 empty check), or the boot gesture — has no marker, so the leave is a jump
into the app with the ROM's clock tree and USB state, and the app can end in
STOP1 with debug off (no VCP, no SWD). The way out is a reset:
a power cycle, or `STM_RESET` over CDC, which pulses the ESP's reset line
(GPIO46, the pulse `CrosspadPlatform` gives `Stm32Manager`'s reset callback),
or NRST from an ST-LINK that has that wire. The proper fix belongs in the STM firmware:
detect a start from the ROM without a reset (e.g. `SYSCFG` memory remap still
on system memory) and self-reset the way the return marker path does. Note
also that the ESP probes the STM link once at boot: an ESP booted while the
STM sleeps reports `running=0.0 (down)` until the ESP itself is reset.

