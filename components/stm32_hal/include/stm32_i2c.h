/*
 * STM32 I2C master link — ESP32-S3 talks to the STM32 co-processor as a
 * master on the shared BSP I2C bus. STM32 sits at STM32_I2C_ADDR_7B (0x42)
 * with a 16-bit register address space (see stm32_reg_map.h).
 *
 * Wire format:
 *   Read  :  S | ADDR+W | RegHi | RegLo | Sr | ADDR+R | D0..Dn | P
 *   Write :  S | ADDR+W | RegHi | RegLo | D0..Dn | P
 *
 * Only available when the active BSP pinout enables BSP_HAS_STM32_I2C
 * (rev2 boards). Calls return ESP_ERR_NOT_SUPPORTED on rev1.
 */

#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the I2C master link to STM32.
 *
 * Reuses the BSP I2C bus (bsp_i2c_init / bsp_i2c_get_handle), so it is
 * safe to call after other I2C devices (es8388, FT6x36, IMU) have been
 * brought up. Idempotent.
 *
 * @return ESP_OK, ESP_ERR_NOT_SUPPORTED on boards without STM32 I2C link,
 *         or a propagated bus / device error.
 */
esp_err_t stm32_i2c_init(void);

/** Release the device handle. The shared BSP bus is left running. */
esp_err_t stm32_i2c_deinit(void);

/** True once stm32_i2c_init() has succeeded. */
bool stm32_i2c_is_initialized(void);

/**
 * @brief Read @p len bytes starting at register @p reg.
 *
 * Length cap (1..255) bounds the transfer payload, NOT the register address
 * space — STM32 reg map spans 0x0000..0x1000 but typical reads are < 32 B.
 * For larger contiguous blocks issue multiple calls with stepped @p reg.
 *
 * Retried up to 3x with a 1 ms gap on transient bus errors.
 *
 * @param reg  16-bit register address (see stm32_reg_map.h).
 * @param dst  Destination buffer (must hold @p len bytes).
 * @param len  Number of bytes to read per transfer (1..255).
 */
esp_err_t stm32_i2c_read(uint16_t reg, uint8_t *dst, size_t len);

/**
 * @brief Write @p len bytes starting at register @p reg.
 *
 * Length cap (1..255) is per-transfer; see stm32_i2c_read note. Retried up
 * to 3x with a 1 ms gap on transient bus errors.
 *
 * @param reg  16-bit register address.
 * @param src  Source buffer.
 * @param len  Number of bytes to write per transfer (1..255).
 */
esp_err_t stm32_i2c_write(uint16_t reg, const uint8_t *src, size_t len);

/** Convenience: read one byte. */
esp_err_t stm32_i2c_read_u8(uint16_t reg, uint8_t *out);

/** Convenience: write one byte. */
esp_err_t stm32_i2c_write_u8(uint16_t reg, uint8_t value);

/**
 * @brief Probe the STM32 by reading the identity block.
 *
 * Validates the 'CPAD' magic and stores firmware version + PCB rev. Run
 * after stm32_i2c_init() to confirm the link is up and the firmware speaks
 * a compatible protocol.
 *
 * @param[out] proto_version 16-bit protocol version (may be NULL).
 * @param[out] fw_major      Firmware major version (may be NULL).
 * @param[out] fw_minor      Firmware minor version (may be NULL).
 * @param[out] pcb_rev       PCB revision byte (may be NULL).
 * @return ESP_OK on a valid handshake, ESP_ERR_INVALID_RESPONSE if the
 *         magic does not match, or a propagated bus error.
 */
esp_err_t stm32_i2c_probe(uint16_t *proto_version,
                          uint8_t *fw_major,
                          uint8_t *fw_minor,
                          uint8_t *pcb_rev);

/**
 * @brief Read cached identity from the last successful probe.
 *
 * Cheaper than stm32_i2c_probe() — no I2C transfer. Use this when a downstream
 * module needs to fan out proto/fw/pcb after init has already done the
 * handshake (callers don't need to re-probe).
 *
 * @return ESP_OK or ESP_ERR_INVALID_STATE if no probe has succeeded yet.
 */
esp_err_t stm32_i2c_get_cached_ident(uint16_t *proto_version,
                                     uint8_t *fw_major,
                                     uint8_t *fw_minor,
                                     uint8_t *pcb_rev);

/** PCB revision the running STM firmware was built for (REG_IDENT_FW_PCB),
 *  from the last successful probe; 0 when unknown (pre-0x000F firmware). */
uint8_t stm32_i2c_cached_fw_pcb(void);

#ifdef __cplusplus
}
#endif
