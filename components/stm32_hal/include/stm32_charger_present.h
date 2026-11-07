#pragma once

#include <stdbool.h>
#include <stdint.h>

/* "A charger is plugged in", from the BQ25895 telemetry. VBUS_STAT alone is
 * not enough: with AUTO_DPDM/HVDCP off (D+/D- float on r20) the BQ never runs
 * input-source detection, so VBUS_STAT reads 000 even on a charger. CHRG_STAT
 * (pre/fast/done) and the VBUS voltage both still see it. Kept free of
 * ESP-IDF headers so the host test can compile it. */
#define STM32_CHARGER_VBUS_MIN_MV 4300u

static inline bool stm32_charger_present_raw(uint8_t vbus_stat, uint8_t chrg_stat, uint16_t vbus_mv)
{
    return vbus_stat != 0 || chrg_stat != 0 || vbus_mv >= STM32_CHARGER_VBUS_MIN_MV;
}
