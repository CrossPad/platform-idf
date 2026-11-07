#include "include/usb_host_profile.h"
#include "NvsSettingsStore.hpp"
#include "usb_audio_port.h"
#include "usb_device_uac.h"
#include "tusb.h"
#include "device/dcd.h"
#include "esp_attr.h"
#include <cstdio>
#include <cstring>

static const char *kNs = "cfg_usb";
static const char *kKey = "mode";
static usb_host_mode_t s_mode = USB_HOST_MODE_AUTO;
static volatile uint8_t s_seen = USB_HOST_SEEN_NONE;
static volatile uint8_t s_profile = USB_HOST_PROFILE_LINUX;
/* The host was placed in this enumeration: until the next bus reset nothing
 * changes it -- a program on the host may ask for a string its own way. */
static volatile bool s_placed;

/* The setup packets since the last bus reset, as they came (USB_ENUM). */
static constexpr uint32_t kTrace = 32;
static uint8_t s_trace[kTrace][8];
static volatile uint32_t s_traceCount;

static void IRAM_ATTR apply(usb_host_profile_t profile)
{
    s_profile = (uint8_t)profile;
    if (profile == USB_HOST_PROFILE_MACOS) {
        uac_device_set_host(UAC_HOST_MACOS);
        usb_audio_port_set_asrc(5000.0f, 6.0f);
    } else {
        uac_device_set_host(UAC_HOST_LINUX);
        usb_audio_port_set_asrc(1000.0f, 2.0f);
    }
}

static void IRAM_ATTR apply_mode()
{
    switch (s_mode) {
    case USB_HOST_MODE_MACOS: apply(USB_HOST_PROFILE_MACOS); break;
    case USB_HOST_MODE_LINUX: apply(USB_HOST_PROFILE_LINUX); break;
    default:
        apply(s_seen == USB_HOST_SEEN_MACOS ? USB_HOST_PROFILE_MACOS : USB_HOST_PROFILE_LINUX);
        break;
    }
}

/* The first GET_DESCRIPTOR for a string other than the language table after
 * a bus reset tells the hosts apart. Linux asks for every string at its
 * largest, 255 bytes, in one request (usb_string() in
 * drivers/usb/core/message.c); macOS asks for the 2-byte header first and
 * then for exactly the length it read (USB_ENUM on a Mac, 2026-10-24:
 * SET_ADDRESS, device descriptor 8 then 18, string 2 for 2 then 18 bytes).
 * Windows asks for 255 too and gets the Linux shape, main's. */
static void IRAM_ATTR classify(const tusb_control_request_t &r)
{
    if (s_placed) return;
    if (r.bmRequestType != 0x80 || r.bRequest != TUSB_REQ_GET_DESCRIPTOR) return;
    if ((r.wValue >> 8) != TUSB_DESC_STRING || (r.wValue & 0xFF) == 0) return;
    s_placed = true;
    s_seen = r.wLength == 255 ? USB_HOST_SEEN_LINUX : USB_HOST_SEEN_MACOS;
    if (s_mode == USB_HOST_MODE_AUTO) apply_mode();
}

extern "C" void __real_dcd_event_handler(dcd_event_t const *event, bool in_isr);

/* Every DCD event passes here first, from the USB interrupt: the bus resets
 * and setup packets the host sent, before TinyUSB's own handler queues them. */
extern "C" void IRAM_ATTR __wrap_dcd_event_handler(dcd_event_t const *event, bool in_isr)
{
    if (event->event_id == DCD_EVENT_BUS_RESET) {
        s_traceCount = 0;
        s_placed = false;
    } else if (event->event_id == DCD_EVENT_SETUP_RECEIVED) {
        const uint32_t n = s_traceCount;
        if (n < kTrace) memcpy(s_trace[n], &event->setup_received, 8);
        s_traceCount = n + 1;
        classify(event->setup_received);
    }
    __real_dcd_event_handler(event, in_isr);
}

extern "C" void usb_host_profile_init(void)
{
    const uint8_t v = NvsSettingsStore::getInstance().readU8(kNs, kKey, USB_HOST_MODE_AUTO);
    s_mode = v <= USB_HOST_MODE_LINUX ? (usb_host_mode_t)v : USB_HOST_MODE_AUTO;
    apply_mode();
}

extern "C" void usb_host_profile_set_mode(usb_host_mode_t mode)
{
    s_mode = mode;
    apply_mode();
    NvsSettingsStore::getInstance().saveU8(kNs, kKey, (uint8_t)mode);
}

extern "C" usb_host_mode_t usb_host_profile_mode(void) { return s_mode; }
extern "C" usb_host_profile_t usb_host_profile_get(void) { return (usb_host_profile_t)s_profile; }
extern "C" usb_host_seen_t usb_host_profile_detected(void) { return (usb_host_seen_t)s_seen; }

extern "C" const char *usb_host_profile_name(usb_host_profile_t profile)
{
    return profile == USB_HOST_PROFILE_MACOS ? "macOS" : "Linux";
}

extern "C" const char *usb_host_mode_name(usb_host_mode_t mode)
{
    return mode == USB_HOST_MODE_MACOS ? "macOS" : mode == USB_HOST_MODE_LINUX ? "Linux" : "auto";
}

extern "C" const char *usb_host_seen_name(usb_host_seen_t seen)
{
    return seen == USB_HOST_SEEN_MACOS ? "macOS" : seen == USB_HOST_SEEN_LINUX ? "Linux" : "none";
}

extern "C" uint32_t usb_host_enum_trace(uint32_t first, uint32_t max, char *out, size_t len)
{
    const uint32_t count = s_traceCount;
    const uint32_t have = count < kTrace ? count : kTrace;
    size_t n = 0;
    if (len) out[0] = '\0';
    for (uint32_t i = first; i < have && i < first + max && n + 17 < len; ++i) {
        const uint8_t *p = s_trace[i];
        n += snprintf(out + n, len - n, "%02x%02x%02x%02x%02x%02x%02x%02x ",
                      p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
    }
    return count;
}
