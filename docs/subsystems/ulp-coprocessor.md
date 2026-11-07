# ULP RISC-V coprocessor

The ESP32-S3 has a second, tiny processor in its RTC domain. It is not the "LP
core" of the C6/P4 (that is `CONFIG_ULP_COPROC_TYPE_LP_CORE`); on the S3 it is
**ULP RISC-V** (`CONFIG_ULP_COPROC_TYPE_RISCV`). CrossPad runs one program on it,
`components/bsp/crosspad/ulp/ulp_background.c`, started from `app_main()` by
`bsp_ulp_start()`.

## What the core can do

| | |
|---|---|
| CPU | RV32IMC, 32-bit, no FPU (fixed point only), no nested interrupts |
| Memory | RTC slow memory, code + data + stack in `CONFIG_ULP_COPROC_RESERVE_MEM` (4096 B here, 8176 B max on S3). It is separate from the heap: reserving it costs no internal RAM or PSRAM |
| Scheduling | The RTC timer wakes it every `ulp_set_wakeup_period()`; `main()` runs to the end and the core halts until the next period. It runs **while the main CPU is active** as well as in deep sleep |
| Peripherals | RTC GPIO (GPIO 0–21), SARADC, touch sensors, RTC I2C master (SDA GPIO 1/3, SCL GPIO 0/2 only), bit-banged UART TX (`ulp_riscv_print`), temperature via ADC |
| Not reachable | Anything on the main buses: the board I2C (STM32, IMU, codecs, DRV2605), SPI (LCD), RMT (LEDs), SDMMC, USB, I2S |
| Main CPU link | Every global in the ULP program is exported as `uint32_t ulp_<name>` (generated `ulp_bsp_background.h`). Shared multi-word state needs `ulp_riscv_lock_acquire/release` |
| Waking the main CPU | `ulp_riscv_wakeup_main_processor()` (from deep sleep); a trap wakes it with `ESP_SLEEP_WAKEUP_COCPU_TRAP_TRIG` |
| Debugging | No JTAG, no printf. Shared counters read over CDC are the debugger |

Sources: ESP-IDF v5.5 [ULP RISC-V coprocessor](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/ulp-risc-v.html),
`components/ulp/Kconfig` and `examples/system/ulp/ulp_riscv/*` in the local IDF.

## What runs on it today

A heartbeat counter: every 100 ms the program adds one to `heartbeat`.

Do not give it the v2 back-cover pin (GPIO 8, `BSP_BACK_COVER_PIN`): with
`rtc_gpio_init()` and the RTC pull-up on GPIO 8 the SD card failed to read at
boot on 8 of 8 resets (`sdmmc_read_blocks failed (0x109)`, `Discovered 0
kits`); the ULP running without that pin: 0 of 6. The pin's wiring needs
checking against the schematic before anything takes it.

None of the `bsp_worker` jobs could move as they are: softoff, sd_hotplug and imu
talk over the board I2C/UART, pad_anim drives RMT, perfmon reads FreeRTOS stats.
The RTC I2C pins (GPIO 0–3) are the boot button, I2S data and the touch
interrupt on this board.

## Workflow

- **Build:** nothing extra. `idf.py -B build_v2 -DSDKCONFIG=sdkconfig.v2 build`
  builds the ULP program as a sub-project with `riscv32-esp-elf-gcc` (installed
  with ESP-IDF) and embeds the binary in the app image. `fullclean` is needed
  only when the ULP source list in `CMakeLists.txt` changes.
- **Flash:** the ULP binary travels inside `CrossPad.bin`, so OTA
  (`tools/bench.py flash`) and UART flashing both deliver it.
- **Check:** `ULP` over CDC (`crosspad-hil cdc ULP`, `verbs.ulp()`):
  `ULP: running=1 period_ms=100 heartbeat=617`. Two
  reads 10 s apart must differ by ~100; 17.09 on v2: 9.97 runs/s, 192 B program.
- **Add work:** put the logic in `ulp/ulp_background.c` (globals are the API),
  configure its pins in `bsp_ulp_start()` before `ulp_riscv_run()`, expose the
  counters in `bsp_ulp_get_status()` and the `ULP` verb, and extend
  `knowledge/cdc.yaml` + `verbs.ulp` in crosspad-hil.

## Traps

- `ulp` must be in the component's **REQUIRES**, not PRIV_REQUIRES: the ULP
  sub-project takes its include path from the public dependencies, and without
  it `ulp_riscv_lock.h` is "not found" inside IDF's own ULP sources.
- `BSP_HAS_*` capabilities compare `gpio_num_t` enumerators. In `#if` the
  preprocessor reads unknown identifiers as 0, so `#if BSP_HAS_BACK_COVER_DETECT`
  is always false. Test them with a plain `if`.
- An RTC-muxed pin is taken away from the digital matrix; GPIO 8 on v2 is not
  as free as `pinout_v2.h` says (see above).
- The program restarts from `main()` every period; state that must survive a run
  goes in a global or `static`, which live in RTC slow memory and are not
  re-initialised between runs.
