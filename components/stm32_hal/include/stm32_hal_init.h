/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "esp_err.h"
#include "stm32_charger_present.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize STM32 communication (UART only)
 *
 * Initializes UART HAL for STM32 co-processor communication.
 * Does NOT start any framework logic — that is handled by
 * crosspad-platform-idf's CrosspadPlatform_StartStm32().
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t stm32_hal_init(void);

/**
 * @brief Deinitialize STM32 communication
 *
 * @return ESP_OK on success, ESP_FAIL on error
 */
esp_err_t stm32_hal_deinit(void);

/**
 * @brief Set display brightness via STM32 (raw SysEx)
 *
 * Sends brightness command directly over UART without requiring
 * crosspad-core. Works in BSP-only mode.
 *
 * @param brightness Brightness level (0-100)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if brightness > 100,
 *         ESP_ERR_INVALID_STATE if HAL not initialized
 */
esp_err_t stm32_hal_set_brightness(uint8_t brightness);

/**
 * @brief Last non-zero brightness requested via stm32_hal_set_brightness().
 *
 * Used by the app-level power-state handler to relight the panel after a
 * low-power transition (STM hands brightness ownership to ESP on wake).
 *
 * @return Cached brightness 1..100 (defaults to 90 before any set).
 */
uint8_t stm32_hal_get_last_brightness(void);

/**
 * @brief Stop the UART heartbeat for a while, then resume it.
 *
 * Test hook only (CDC RESET_TEST stmwd): the STM link watchdog resets the ESP
 * after ~5 s of heartbeat silence, and this is how that path is exercised on
 * purpose. Longer than the watchdog threshold means the ESP reboots.
 */
void stm32_hal_heartbeat_pause_ms(uint32_t ms);

/**
 * @brief Hold the heartbeat off for as long as the link is owned by someone
 *        else (the STM firmware updater). Unlike the pause, this has no
 *        timer behind it — the caller resumes it.
 */
void stm32_hal_heartbeat_suspend(bool suspend);

/**
 * @brief Re-probe the STM32 identity over I2C and refresh the cached link
 *        info — after the STM rebooted into new firmware, for instance.
 */
esp_err_t stm32_hal_refresh_link_info(void);

/**
 * @brief Check if STM32 HAL is initialized
 *
 * @return true if initialized, false otherwise
 */
bool stm32_hal_is_initialized(void);

/** Cached STM32 link state (I2C probe result + handshake info, or a rev 1.x
 *  STM's version reply over UART). */
typedef struct {
    bool     linked;          /* true after I2C probe + magic match, or the UART version reply */
    uint16_t proto_version;   /* STM32_PROTO_VERSION reported by firmware */
    uint8_t  fw_major;        /* 0.0 from a UART-only STM: its reply carries no firmware version */
    uint8_t  fw_minor;
    uint8_t  pcb_rev;         /* the board (OTP; STM's compile-time default when blank) */
    uint8_t  fw_pcb;          /* the PCB the running STM firmware was built for; 0 = unknown */
    bool     uart_only;       /* a rev 1.x STM: no registers, no in-field update */
} stm32_link_info_t;

/**
 * @brief Read cached link info from the last successful probe.
 *
 * Populated by stm32_hal_init() when the I2C handshake succeeds, and on a
 * board without the I2C link by stm32_hal_set_uart_version() when the STM
 * answers the version request the HAL sends over UART with its heartbeat. Call after
 * stm32_hal_init(); for live re-probe use stm32_i2c_probe() directly.
 *
 * @param[out] out Populated when stm32_hal_is_linked() is true.
 * @return ESP_OK or ESP_ERR_INVALID_STATE when never linked.
 */
esp_err_t stm32_hal_get_link_info(stm32_link_info_t *out);

/** Convenience: true when I2C probe handshake succeeded since boot. */
bool stm32_hal_is_linked(void);

/**
 * @brief The version a UART-only STM (rev 1.x) reported (CROSSPAD_CMD_VERSION_REPLY).
 *
 * Fills the link info the I2C probe fills on rev 2, so its readers see one
 * shape. Ignored on a board with the I2C link, whose probe owns it.
 */
void stm32_hal_set_uart_version(uint8_t pcb_rev, uint8_t api_version);

/**
 * @brief BQ25895 telemetry snapshot (REG_PWR_CHG_* block).
 *
 * Mirrors the firmware snapshot built once per reg_snapshot_refresh() tick.
 * Populated atomically via a single 22-byte I2C read — pairs with
 * stm32_hal_get_charger_telemetry().
 */
typedef struct {
    uint16_t vbus_mv;          /* REG_PWR_CHG_VBUS    */
    uint16_t vsys_mv;          /* REG_PWR_CHG_VSYS    */
    uint16_t vbat_mv;          /* REG_PWR_CHG_VBAT — BQ on-die BATV (only valid while the BQ ADC converts / charging) */
    uint16_t vbat_adc_mv;      /* REG_PWR_VBAT — STM direct-ADC battery (always live; the forward-compatible source) */
    uint16_t ichg_ma;          /* REG_PWR_CHG_ICHG    */
    uint8_t  ts_pct;           /* REG_PWR_CHG_TS_PCT  */
    uint8_t  vbus_stat;        /* REG_PWR_CHG_VBUS_ST (enum 0..7) */
    uint8_t  chrg_stat;        /* REG_PWR_CHG_CHRG_ST (enum 0..3) */
    uint16_t iinlim_ma;        /* REG_PWR_CHG_IINLIM  */
    uint16_t idpm_lim_ma;      /* REG_PWR_CHG_IDPM    */
    uint16_t vindpm_mv;        /* REG_PWR_CHG_VINDPM  */
    uint8_t  ntc_fault;        /* REG_PWR_CHG_NTC bitfield */
    uint8_t  flags;            /* REG_CHG_FLAG_*      */
    uint8_t  src_active;       /* CHG_ACT_*           */
    uint16_t target_iinlim_ma; /* REG_PWR_CHG_TGT_LIM */
    /* SoC estimator (STM EKF, firmware reg ver >= 0x000A; 0 / invalid on older). */
    uint8_t  soc_pct;          /* REG_PWR_SOC_PCT   — 0..100 */
    uint16_t soc_permil;       /* REG_PWR_SOC_PMIL  — SoC x1000 (0.1%) */
    uint8_t  soc_flags;        /* REG_PWR_SOC_FLAGS — REG_SOC_FLAG_* */
} stm32_charger_telemetry_t;

/**
 * @brief Read the full BQ25895 telemetry block in one I2C transfer.
 *
 * Equivalent to nine separate single-register reads — saves bus time and
 * gives a coherent snapshot (no tearing between fields within a tick).
 *
 * @param[out] out  Populated on ESP_OK.
 * @return ESP_OK, ESP_ERR_INVALID_ARG (NULL), ESP_ERR_INVALID_STATE (link down),
 *         or propagated I2C bus error.
 */
esp_err_t stm32_hal_get_charger_telemetry(stm32_charger_telemetry_t *out);

/** A charger is plugged in — see stm32_charger_present.h for why VBUS_STAT alone is not. */
static inline bool stm32_charger_present(const stm32_charger_telemetry_t *t)
{
    return stm32_charger_present_raw(t->vbus_stat, t->chrg_stat, t->vbus_mv);
}

/**
 * @brief Set dim-after-inactivity timeout (REG_CTL_DIM_TO_S_HI/LO).
 *
 * Wraps the HI/LO byte-pair write so callers don't have to think about
 * endianness or commit order. 0 disables dim.
 */
esp_err_t stm32_hal_set_dim_timeout_s(uint16_t seconds);

/**
 * @brief Set sleep (STOP1) timeout (REG_CTL_SLEEP_TO_S_HI/LO).
 *
 * 0 disables auto-sleep.
 */
esp_err_t stm32_hal_set_sleep_timeout_s(uint16_t seconds);

#ifdef __cplusplus
}
#endif
