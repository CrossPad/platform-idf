#pragma once
#include <cstdint>

/* WiFi remote management (docs/design/2026-10-08-wifi-remote-design.md):
 * esp-mqtt to wss://<account base>/api/device/mqtt/<serial>, the companion
 * allowlist runs the commands. Holds a WiFi lease "remote" while the board is
 * paired, WiFi is allowed and Settings -> WiFi -> Remote management is on; BLE is
 * off meanwhile (the radios are exclusive). */
void        cp_remote_init(bool run);                   // after cp_account_init(); run=false (transfer / network mode boot): nothing connects
bool        cp_remote_wanted(void);                     // paired, WiFi allowed and Remote management on: remote holds its lease
void        cp_remote_retry_now(void);                  // WiFi page opened: the next pass tries at once, link backoff back to 60 s
void        cp_remote_set_wake(void (*wake)(void));     // wakes the main loop for cp_remote_poll()
void        cp_remote_poll(void);                       // main loop: runs one queued command
bool        cp_remote_handle(const char *cmd, void (*reply)(const char *));   // REMOTE_STATUS
uint8_t     cp_remote_peers(void);                      // browsers connected (status-bar icon)
const char *cp_remote_state_name(void);
