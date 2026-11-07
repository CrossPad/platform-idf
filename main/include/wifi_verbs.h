#pragma once
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* WIFI_* verbs (CDC, BLE companion, SysEx 0x1F alike). True when `cmd` was one,
 * answered through `reply`. Runs on the CDC drain loop or the companion's main
 * task: nothing here waits for the radio. */
bool wifi_verbs_handle(const char *cmd, void (*reply)(const char *text));
#ifdef __cplusplus
}
#endif
