#pragma once
#ifdef __cplusplus
extern "C" {
#endif
/* The board's serial for crosspad.app: the PCB serial the factory wrote into the
 * STM's OTP ("CP1234"), or the ESP MAC as 12 hex digits when there is none (rev1,
 * an unprovisioned board, the STM not answering). The PCB serial is read once and
 * kept; the MAC fallback is asked again on the next call. Reads the STM over I2C:
 * not from an ISR. */
const char *device_serial(void);
#ifdef __cplusplus
}
#endif
