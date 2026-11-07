/**
 * @file bsp_ulp.h
 * @brief Background work on the ULP RISC-V coprocessor.
 *
 * The coprocessor is an RV32IMC core in the RTC domain (no FPU, 4 kB of RTC
 * slow memory for code and data here, 8176 B at most). Its timer wakes it every
 * BSP_ULP_PERIOD_MS while the main CPU runs or sleeps; it reaches RTC GPIO,
 * SARADC, touch and RTC I2C (GPIO 0-3 only), nothing on the main buses. It
 * counts a heartbeat. Program: ulp/ulp_background.c.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BSP_ULP_PERIOD_MS 100

typedef struct {
    bool running;
    uint32_t heartbeat;      ///< ULP runs since start
} bsp_ulp_status_t;

/** Load the ULP program and start its timer. */
esp_err_t bsp_ulp_start(void);

/** Snapshot of what the ULP program has counted. */
void bsp_ulp_get_status(bsp_ulp_status_t *out);

#ifdef __cplusplus
}
#endif
