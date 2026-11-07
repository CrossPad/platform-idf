#pragma once

/* Reset black box.
 *
 * A spontaneous reboot on the launcher leaves nothing behind: the panic text
 * goes to a UART nobody is reading and the STM's link watchdog resets the ESP
 * with no panic at all. This module records, in NVS, what every boot can
 * still see about the reset that preceded it — the ESP's own reset reason,
 * what the STM counted on the UART banner and whether ITS watchdog forced
 * the reset, how long the previous run lasted and which app was up — plus
 * whether a core dump (panic / task WDT) landed in the coredump partition.
 *
 * Read it back later, over the CrossPad's own CDC, with RESET_LOG; the crash
 * summary (PC, exception cause, backtrace) with COREDUMP_INFO.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record the reset that led to this boot. Call once the STM I2C link is up
 * (after bsp_stm32_init) and after CrosspadPlatform_Init (event bus). */
void reset_log_init(void);

/* Newest-first human-readable dump of the ring. Returns bytes written. */
size_t reset_log_format(char *out, size_t cap);

/* Live read of the STM's counters (banner-parsed resets, watchdog resets). */
size_t reset_log_stm_format(char *out, size_t cap);

/* Core dump summary from the coredump partition, or a one-line "none". */
size_t reset_log_coredump_format(char *out, size_t cap);

/* Forget the ring and erase the stored core dump. */
void reset_log_clear(void);

#ifdef __cplusplus
}
#endif
