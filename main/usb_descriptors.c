#include "include/usb_descriptors.h"
#include "tusb.h"
#include "class/cdc/cdc_device.h"
#include "class/midi/midi_device.h"
#include "esp_mac.h"
#include <stdio.h>
#include <string.h>


const char* s_str_desc[5] = {
    (char[]){0x09, 0x04},  // 0: language ID (English, 0x0409)
    "TinyUSB",             // 1: Manufacturer
    "Crosspad",      // 2: Product
    NULL, // 3: Serial — usb_serial_string() at install
    "Example MIDI device", // 4: MIDI
};

const tusb_desc_device_t desc_device = {
    .bLength = sizeof(desc_device),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,
    .idProduct = 0x3456,
    // Windows matches the driver stack on ...&REV_xxxx, so the revision nibble
    // gives each board revision its own REV_xxxx.
    .bcdDevice = USB_BCD_DEVICE(USB_BCD_PROFILE_CDC),
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 1
  };

// Interface numbers
enum {
  ITF_NUM_CDC = 0,
  ITF_NUM_CDC_DATA,
  ITF_NUM_MIDI,
  ITF_NUM_MIDI_STREAMING,
  ITF_NUM_TOTAL
};

// Endpoint numbers
#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define EPNUM_MIDI_OUT    0x04
#define EPNUM_MIDI_IN     0x84

// TUD_MIDI_DESCRIPTOR emits no IAD, but this device declares MISC_PROTOCOL_IAD
// and Windows groups interfaces by IAD — without one, usbaudio binds the AC
// interface alone and opening the port fails (MMSYSERR_NOMEM). Hence the
// explicit 8-byte IAD below.
#define MIDI_IAD_LEN 8
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + MIDI_IAD_LEN + TUD_MIDI_DESC_LEN)

// Configuration descriptor
const uint8_t desc_fs_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0, 100),
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 0, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
  MIDI_IAD_LEN, TUSB_DESC_INTERFACE_ASSOCIATION, ITF_NUM_MIDI, 2,
      TUSB_CLASS_AUDIO, AUDIO_SUBCLASS_CONTROL, AUDIO_FUNC_PROTOCOL_CODE_UNDEF, 0,
  TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 0, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64)
};

#define USB_SERIAL_LEN sizeof("00:00:00:00:00:00")

const char *usb_serial_string(void)
{
    static char serial[USB_SERIAL_LEN];
    if (!serial[0]) {
        uint8_t mac[6] = {0};
        esp_efuse_mac_get_default(mac);
        snprintf(serial, sizeof(serial), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    return serial;
}
