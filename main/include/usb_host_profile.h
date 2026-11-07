/**
 * @file usb_host_profile.h
 * @brief Which computer USB audio is tuned for: macOS or Linux, found by itself.
 *
 * The two hosts want the feedback endpoint and the playback resampler shaped
 * differently (usb_device_uac.c, UsbAudioPort.cpp): Linux a feedback counted
 * over 128 SOFs and a resampler of +-1000 ppm, macOS a feedback counted per
 * interval and +-5000 ppm. In Auto (the default) the board tells the hosts
 * apart by how they enumerate it -- the control requests they send, seen in
 * TinyUSB's event handler (--wrap=dcd_event_handler) -- before any audio
 * stream opens; a host it cannot place gets the Linux shape, the one main
 * played everywhere. The mode persists in NVS (`cfg_usb/mode`) and applies at
 * once. Settings -> USB host, `USB_HOST [AUTO|MAC|LINUX]` and `USB_ENUM` over
 * CDC and the companion SysEx.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The shape in force. */
typedef enum {
    USB_HOST_PROFILE_MACOS = 0,
    USB_HOST_PROFILE_LINUX = 1,
} usb_host_profile_t;

/** What the user picked. The order is the Settings dropdown's. */
typedef enum {
    USB_HOST_MODE_AUTO  = 0,
    USB_HOST_MODE_MACOS = 1,
    USB_HOST_MODE_LINUX = 2,
} usb_host_mode_t;

/** What enumeration said about the host. */
typedef enum {
    USB_HOST_SEEN_NONE  = 0,   // no string descriptor asked for yet
    USB_HOST_SEEN_MACOS = 1,
    USB_HOST_SEEN_LINUX = 2,
} usb_host_seen_t;

/** Boot, after NVS and before USB: load the saved mode and apply it. */
void usb_host_profile_init(void);
/** Apply @p mode now and persist it. */
void usb_host_profile_set_mode(usb_host_mode_t mode);
usb_host_mode_t usb_host_profile_mode(void);
/** The shape in force: the mode's, or in Auto what was detected (Linux until then). */
usb_host_profile_t usb_host_profile_get(void);
usb_host_seen_t usb_host_profile_detected(void);
const char *usb_host_profile_name(usb_host_profile_t profile);   // "macOS" | "Linux"
const char *usb_host_mode_name(usb_host_mode_t mode);            // "auto" | "macOS" | "Linux"
const char *usb_host_seen_name(usb_host_seen_t seen);            // "none" | "macOS" | "Linux"
/** The control requests since the last bus reset, from @p first, as 16 hex
 *  digits each (the setup packet as sent) separated by spaces; at most @p max.
 *  Returns how many there were in all. */
uint32_t usb_host_enum_trace(uint32_t first, uint32_t max, char *out, size_t len);

#ifdef __cplusplus
}
#endif
