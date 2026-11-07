# stm32_hal — TODO

Ecosystem review 2026-05-12. Strona ESP-IDF kontraktu STM↔ESP. Counterpart: `TODO.md` repo firmware STM (CrossPad_STM32_r2) § REVIEW 2026-05-12.

## CRITICAL — sync z firmware STM r20

- [x] **I1. `STM32_PROTO_VERSION` rozjeżdża się z firmware.** **DONE 2026-05-12** — bumped to `0x0003`, firmware wire bytes derived from `CROSSPAD_REG_MAP_VERSION` w `reg_map.c`.
- [x] **I2. `REG_POWER_LEN` BQ telemetria.** **DONE 2026-05-12** — dosypane wszystkie `REG_PWR_CHG_*` (18 bajtów), `REG_POWER_LEN = 0x23`. Plus helpery bitmask `REG_CHG_FLAG_*`, `REG_CHG_STATUS_*` + enumy `CHG_SRC_*` / `CHG_ACT_*`.
- [x] **I3. `REG_CTL_CHG_*` brak w IDF.** **DONE 2026-05-12** — wszystkie 16 stałych w IDF.
- [x] **I4. `REG_CMD_CHG_*` brak w IDF.** **DONE 2026-05-12** — wszystkie 6 cmd stałych w IDF.
- [x] **I5. `REG_MAPPING_FIELD_*` semantyka pól pad-mapping.** **DONE 2026-05-12** — `REG_MAPPING_FIELD_MUX_A/MUX_B/CH_A/CH_B/SWAP` po obu stronach.

## HIGH — UX / observability

- [x] **I6. Expose STM link state.** **DONE 2026-05-12** — `stm32_hal_is_linked()` + `stm32_hal_get_link_info(stm32_link_info_t*)` zwracają cached proto/fw/pcb po probe. Proto mismatch loguje warning. GUI badge TODO osobno (gui-side, nie hal).
- [x] **I7. Brak helperów do nowej BQ telemetry.** **DONE 2026-05-12** — `stm32_hal_get_charger_telemetry(stm32_charger_telemetry_t*)` reads 22 B w jednym xfer i parsuje na typed struct (vbus/vsys/vbat/ichg/ts/iinlim/idpm/vindpm/ntc/flags/src_active/target_iinlim).
- [x] **I8. Brak high-level wrapperów dla sleep timeouts.** **DONE 2026-05-12** — `stm32_hal_set_dim_timeout_s(uint16_t)` + `stm32_hal_set_sleep_timeout_s(uint16_t)` pakują BE u16 w jeden 2-byte write (HI→LO commit order zachowany).

## MED — ekosystem

- [ ] **I9. Single source of truth dla reg_map.** Aktualnie ręczna duplikacja `stm32_reg_map.h` ↔ STM `reg_map.h`. Opcje:
  - (a) Shared header w `crosspad-core/include/crosspad/protocol/stm32_regs.h` jako submodule po obu stronach.
  - (b) Generator: `tools/sync_stm32_regs.py` runs pre-commit, fail na diff.
  - Wybrać i wdrożyć. STM TODO R14.
- [ ] **I10. `stm32_pwr_sync` auto-ACK zbyt agresywny.** `stm32_hal_init.c:36-58` auto-ACK natychmiast dla SLEEP_PENDING + SHIP_PENDING. Aplikacja powinna mieć szansę zaorać NVS / mute audio / drop ESP-NOW peers PRZED ACK. Sugestia: ack tylko jeśli app override callback nie zarejestrowane, dać aplikacji ~2 s window.
- [ ] **I11. Brak reverse PWR_STATE (ESP→STM).** ESP nie ma jak zapowiedzieć STM-owi że idzie w light-sleep. Dodać `stm32_hal_notify_esp_sleep_pending()` + odpowiednik REG po stronie STM. Powiązane STM TODO R27.
- [x] **I12. Brak heartbeat send-side z IDF.** **DONE 2026-05-12** — `esp_timer` 500 ms periodic w `stm32_hal_init` wysyła `0xFE` MIDI Active Sensing single-byte. STM `ESP_HEARTBEAT_TIMEOUT_MS=2000`, więc 4× margines. Real-time byte transparentny dla SysEx FSM, safe do wstrzyknięcia mid-stream.

## LOW

- [x] **I13. `stm32_i2c_read/write` length cap 255.** **DONE 2026-05-12** — docstring uzupełniony: cap dotyczy per-transfer payload, nie register space. Dla większych bloków stepped @reg.
- [x] **I14. Retry policy w `stm32_i2c_*`.** **DONE 2026-05-12** — `STM32_I2C_RETRY_COUNT=3` z `esp_rom_delay_us(1000)` między próbami w read+write. Busy-wait nie yield-uje rdzenia.
- [x] **I15. Cached probe accessor.** **DONE 2026-05-12** — `stm32_i2c_get_cached_ident()` zwraca cached proto/fw/pcb bez I2C transferu. Populowane przez `stm32_i2c_probe` po sukcesie magic match.

## STRATEGICZNE

- [ ] **I17. Brak end-to-end test scenarios.** Hardware-in-the-loop: STM ↔ ESP-IDF probe handshake, register write+read sanity, MIDI pipe loopback, sleep wake cycle. Kandydat: `components/stm32_hal/test/` z fixture przez `crosspad_test_run`.

---

## DONE 2026-05-12 (batch 2 — heartbeat + i2c hardening)

- I12: heartbeat send-side — esp_timer 500 ms periodic w `stm32_hal_init.c`, single-byte `0xFE` przez `stm32_uart_send`. STM widzi heartbeat od chwili init UART. Timer dispatched przez `ESP_TIMER_TASK`, `skip_unhandled_events=true` żeby nie kumulować zaległości gdy callback opóźniony.
- I13: docstring uzupełniony w `stm32_i2c.h` — length cap 255 jest per-transfer, nie register space (sięga 0x1000). Retry policy też udokumentowana.
- I14: retry x3 z `esp_rom_delay_us(1000)` w `stm32_i2c_read` + `stm32_i2c_write`. Wraca ostatni err gdy wszystkie próby fail. Busy-wait (no yield) bo 1 ms < jeden FreeRTOS tick.
- I15: `s_ident_cache` w `stm32_i2c.c` populowany w `stm32_i2c_probe` po magic match. `stm32_i2c_get_cached_ident()` zwraca cached fields lub `ESP_ERR_INVALID_STATE`.

Build verified: `idf.py -B build_v2` clean pass, `CrossPad.bin 0x188d90` (56 % free).

## DONE 2026-05-12 (batch 1 — proto + telemetry sync)

- I7: `stm32_hal_get_charger_telemetry()` — 22-byte single-xfer read całego BQ block, typed struct `stm32_charger_telemetry_t`. Konsumenci dostają coherent snapshot bez tearing.
- I8: `stm32_hal_set_dim_timeout_s/_sleep_timeout_s` — pakują BE u16 w jeden 2-byte i2c write, HI→LO commit order zachowany przez STM-side handler.
- I1: proto version bump 0x0001 → 0x0003 zsynchronizowany z STM (`include/stm32_reg_map.h:23`).
- I2: dosypane BQ25895 telemetry block (`REG_PWR_CHG_*`, +18 bajtów) — IDF widzi VBUS/VSYS/VBAT/ICHG/TS/IINLIM/IDPM/VINDPM/NTC/FLAGS/SRC_ACT/TGT_LIM. `REG_POWER_LEN` bumped 0x0D → 0x23.
- I3: dosypane `REG_CTL_CHG_*` (16 stałych — ICHG/VREG/IINLIM/VINDPM/ITERM/IPRE/BOOST/SYSMIN/ENABLE/OTG/HIZ/TERMINATE/STAT_LED/SRC_MODE/AUTO_LIM).
- I4: dosypane `REG_CMD_CHG_*` (6 stałych).
- I5: dodane `REG_MAPPING_FIELD_*` po obu stronach kontraktu.
- I6: link-state getter (`stm32_hal_is_linked`, `stm32_hal_get_link_info` z `stm32_link_info_t` cache: proto/fw/pcb).
- Dodatkowo: helpery bitmask (`REG_CHG_FLAG_*`, `REG_CHG_STATUS_*`) + enumy (`CHG_SRC_*`, `CHG_ACT_*`) dla typowania nowych pól.

Counterpart fixy w STM r20 (commit needed):
- `reg_map.h`: `CROSSPAD_REG_MAP_VERSION 0x0002 → 0x0003`, dodane `REG_MAPPING_FIELD_*`, `REG_IDENT_LEN`.
- `reg_map.c`: wire bytes PROTO_HI/LO derived from version constant; defaults `s_ctl_audio_en=0`, `s_ctl_brightness=50`; charger CTL readback paths zwracają `s_pending_*` zamiast live ADC; `REG_CMD_SOFT_RESET` ma `return`.
- `i2c_slave.c`: `s_ptr=0` reset w `ListenCpltCallback` + `ErrorCallback`.
- `sleep.c`: `battery_present = (VBAT > BATTERY_LOW_MV) && !VSYS_STAT`; usunięte zduplikowane CHG_INT pullup.
- `charger.c`: bq2589x_Init retry x3 z 50 ms odstępem, degraded mode gdy `!s_chip_present`, CHG_INT pullup w `charger_init` (owner pinu). BKP3R upper nibble = init result code dla SWD debug. Public `charger_is_present()`.
- `esp_sync.c`: nowy `esp_sync_watchdog_loop` — link silent 5 s → reset_esp, 3 retry, potem `esp_sync_link_dead()`; PWR_STATE_WOKE 8x retry × 250 ms.
- `esp_link.c`: inline SysEx FSM, F0 7D 30 F7 → `g_dfu_request=1`.
- `crosspad_app.c`: deferred DFU service w main loop; `esp_sync_watchdog_loop()` wpięte.
- `leds.c`: LED1 auto pokazuje 50 ms pulse co 1 s gdy `esp_sync_link_dead()` (top priority).

STM final build: 79724 B FLASH (60.82 %), 17464 B RAM (11.84 %).
