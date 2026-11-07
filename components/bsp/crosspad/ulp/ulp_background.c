/*
 * Runs on the ESP32-S3's ULP RISC-V coprocessor, woken by the RTC timer every
 * period bsp_ulp_start() sets, whatever the main CPU is doing. Every global
 * here is exported to the main CPU as ulp_<name> and lives in RTC slow memory,
 * so it keeps its value from one run to the next.
 */
#include <stdint.h>

uint32_t heartbeat;

int main(void)
{
    heartbeat++;
    return 0;
}
