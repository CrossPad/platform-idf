# Hardware and board revisions

Contributor notes: the chips, the pinout headers, v1 and v2, CI for each, the ULP core. The short version of what a change in this area
must not break; the pages it links to have the detail.

## Hardware Quick Reference

| Component | Chip/Interface | Notes |
|-----------|---------------|-------|
| SoC | ESP32-S3, dual Xtensa, 240MHz | 8MB PSRAM (OCT), 16MB Flash |
| Display | ILI9341, 320x240, SPI | ST7789 controller variant |
| Touch | FT6x36, I2C | Capacitive |
| Audio | 2x ES8388, I2S0 (master) + I2S1 (slave) | 44.1 kHz stereo (engine rate from Settings → Audio; kits are 44.1 k and the engine does not resample), anti-pop sequences |
| IMU | ST LSM6DS3TR-C (WHO_AM_I 0x6A), I2C, own driver (`bsp_imu.cpp`, bits from ST's `lsm6ds3tr-c_reg.h`) | gyro 52 Hz, accel 416 Hz with LPF2 ODR/50, BDU; single tap/free fall/wake-up/6D recognised in the part (`bsp_imu_set_event_callback`, `IMU_THS`, raw `IMU_REG`/`IMU_REGW`), **double taps paired in the driver** (two knocks within 500 ms, 150 ms to settle — the part's own double-tap machine answered a hand on this case unpredictably, whatever DUR/QUIET were); **the wake-up threshold must stay above the tap threshold (125 mg default) and the sources must not be latched, or no tap arrives at all**; a pad hit shakes the case like a knock, so `main.cpp` drops tap gestures for 400 ms after every `PadPressed`; gestures reach apps as `EventType::MotionGesture` on the event bus, not through any IMU call; gyro sleep only in LIGHT sleep, where motion wakes the board (`REG_CMD_USER_ACTIVITY`); the `imu` task wakes on the gyro data-ready pulse on INT2 (v2 GPIO 48, attached after `bsp_display_start()`), reads 0x1B..0x2D in one burst, Kalman on the ESP; polls where `BSP_HAS_IMU_INT2` is 0 |
| LEDs | WS2812B, 16 addressable | One per pad, via RMT |
| Pads | 16 capacitive, pressure-sensitive | Via STM32 co-processor (UART SYSEX) |
| Encoder | Rotary + momentary switch | GPIO, LVGL input device |
| SD Card | SDMMC | 4-bit bus |
| Haptics | DRV2605 | Vibration motor driver, probed and started by `CrosspadPlatform_StartVibration()` |
| Wireless | ESP-NOW, WiFi, BLE | The S3 has no Bluetooth Classic — no A2DP/SPP, BLE only |
| USB | MIDI + CDC + UAC2 (TinyUSB) | CDC profile VID 0x303A, PID 0x3456; MIDI+UAC2 profile PID 0x3457 (no CDC) |
| MIDI | USB, BLE, UART (via the STM32) | |
| Storage | SD card (FAT), LittleFS `assets`, FAT `storage` | Partitions: `partitions_16mb.csv` |
| Co-processor | STM32 | UART (+ I2C at 0x42 on v2), pad scanning + charger management |
| Battery | Li-Ion, 4000 mAh | From the old README's spec table; not readable from code |

BSP API reference: https://crosspad.app/docs/reference/bsp/ (generated from `components/bsp/crosspad/include/`)
Audio codec details: `components/bsp/crosspad/AUDIO_ES8388.md`
Performance monitoring: `components/bsp/crosspad/PERFMON.md`
Hardware revisions: `components/bsp/crosspad/README.md#hardware-revisions`
Schematic: in the Arduino repo (`CrossPad/ESP32-S3`) under `docs/sch_cp.pdf`

## Hardware Revisions

CrossPad has multiple PCB revisions. The active one is selected at **build time**
via Kconfig (`CONFIG_BSP_BOARD_REV_V1` / `CONFIG_BSP_BOARD_REV_V2`). One firmware
binary per revision — no runtime probe.

**Which revision to build/flash is decided by `tools/crosspad_board.py`, not by
memory.** Order: `--board v1|v2` explicit → the connected board (hardware
revision from the STM bridge `bcdDevice` = `0x3000 | BCD(PCB)`, else firmware
revision from the ESP `bcdDevice` = `0x0500 | rev<<4 | profile`) → `"board"` in
`crosspad.local.json` → none (non-interactive callers exit 2). `idf.py board`
shows the decision and its source, `idf.py board --set v2` remembers one. CP
Tools, bench.py, ota_flash.py, factory_provision.py and crosspad-mcp all go
through this resolver, and `ota_flash.py` refuses an image built for another
revision than a connected board. Full design:
[docs/design/2026-09-14-board-selection-design.md](../design/2026-09-14-board-selection-design.md).

- **Pinout headers:** `components/bsp/crosspad/include/pinout/pinout_v*.h`. Each
  revision has its own header with all GPIOs and capability flags
  (`BSP_HAS_STM32_I2C`, `BSP_HAS_EEPROM`, `BSP_HAS_JACK_ADC`, `BSP_HAS_BACK_COVER_DETECT`, …).
- **Selector:** `components/bsp/crosspad/include/crosspad_s3.h` — single
  `#if defined(CONFIG_BSP_BOARD_REV_V*)` chain picks the right pinout header.
  **Never add `#ifdef CONFIG_BSP_BOARD_REV_*` anywhere else in the codebase.**
  Branch on `BSP_HAS_*` capability macros instead so consumers stay
  rev-agnostic.
- **Pins missing on a rev** are defined as `GPIO_NUM_NC`, not `#undef`-ed —
  capability flags derive automatically (`BSP_HAS_JACK_ADC = (ADC_PIN != GPIO_NUM_NC)`).
- **Per-revision build dirs** `build_v1` / `build_v2` (each with its
  `sdkconfig.v*`, see Build Environment) avoid a full reconfigure when
  switching; `build_*/` is gitignored so they cohabit. PCB r16/r19 = v1,
  PCB r20 = v2.
- **Adding a new rev:** copy `pinout_v2.h` → `pinout_v3.h`, add Kconfig choice
  entry + matching `BSP_BOARD_REV_STR` default, add `#elif` branch in
  `crosspad_s3.h`. Full procedure: `components/bsp/crosspad/README.md#adding-a-new-revision`.
- **`sdkconfig.v1`/`sdkconfig.v2` are regenerated with the local IDF (5.5.4),
  not CI's** — CI builds the matrix inside `ghcr.io/crosspad/crosspad-ci:v5.5.5-1`
  (ESP-IDF v5.5.5, `tools/ci/`), a patch-level difference. `v1` also builds without a flash core dump
  (`ESP_COREDUMP_ENABLE_TO_FLASH` off); `reset_log.cpp` guards the
  `esp_core_dump_get_summary()` path behind `RSTLOG_COREDUMP` so it still
  builds — `COREDUMP_INFO` just answers "none" on v1. CI builds **v2 only**
  (`-B build_$rev -DSDKCONFIG=sdkconfig.$rev`; v1 cells are off by default --
  a manual run with input `revs: v1,v2` still builds it, `tools/ci_matrix.py --revs`) on
  `main`. **Optional self-hosted runner:** `tools/ci_runner.py install`
  + `service` registers a machine as a self-hosted runner; `on`/`off` flips
  the repo variable `CI_RUNNER` the workflow reads (`off` before that machine
  goes down -- GitHub never falls back to a hosted runner). **CI image:** every
  job runs in `ghcr.io/crosspad/crosspad-ci:<IDF>-<rev>` (`tools/ci/Dockerfile`,
  published from main by `ci-image.yml`), the image the other CrossPad repos'
  CI uses too; a change to `tools/ci/` bumps `CI_IMAGE_REV`, then each consumer
  moves its pin (tag@digest, from the publish job's summary) in a commit of
  its own. The `release` and Pages jobs stay on GitHub's runner (secrets) --
  unless the repo variable `CI_SECRETS_RUNNER=self-hosted` is set, only while
  GitHub's hosted runners cannot start jobs.

## ULP RISC-V Coprocessor

The S3's RTC-domain RV32IMC core (not a C6/P4 "LP core") runs
`components/bsp/crosspad/ulp/ulp_background.c` every 100 ms beside the main CPU,
started by `bsp_ulp_start()` in `app_main()`: a heartbeat. It reaches only RTC
GPIO/ADC/touch/RTC I2C (GPIO 0–3), never the board buses. Builds and flashes with
the app image; check with `ULP` over CDC. `ulp` must be in REQUIRES, and
`BSP_HAS_*` needs a runtime `if`, not `#if`. **Do not take GPIO 8 into the RTC
mux**: as the "back cover" pin it breaks SD card reads (8 of 8 boots). Capabilities, workflow, traps: [docs/subsystems/ulp-coprocessor.md](../subsystems/ulp-coprocessor.md).
