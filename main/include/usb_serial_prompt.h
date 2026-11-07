/**
 * @file usb_serial_prompt.h
 * @brief The USB serial guard: a host asked for the CDC profile over SysEx
 *        (0x1B 0x01) and CrosspadSettings::usbSerialGuard says the user decides.
 */
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Ask on the device. Any task; the dialog is raised on the LVGL thread.
 * Allow switches to USB_MODE_DEFAULT; Deny, the power click and 30 s do
 * nothing. One prompt at a time — a repeat while it is up is covered by the
 * answer. Before the launcher is up the request is dropped (logged).
 */
void usb_serial_prompt_request(void);

/** True while the prompt is up. The companion fronts refuse ENC_* then: the
 *  host that asked must not be the one that presses Allow. */
bool usb_serial_prompt_active(void);

#ifdef __cplusplus
}
#endif
