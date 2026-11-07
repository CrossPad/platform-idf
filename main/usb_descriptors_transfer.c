#include "tusb.h"
#include "usb_descriptors.h"

/*
 * Transfer mode: MSC + CDC, no MIDI — the SD card is the point and nothing
 * plays here. CDC stays so the bench, crosspad-hil, the USB_MSC 0 exit verb
 * and OTA keep working while the host owns the card.
 *
 * IN  EP0 + 0x81 CDC notif + 0x82 CDC + 0x83 MSC   (the DWC2 allows 5 + EP0)
 * OUT 0x02 CDC + 0x03 MSC
 */

enum {
    ITF_NUM_CDC_T = 0,
    ITF_NUM_CDC_DATA_T,
    ITF_NUM_MSC_T,
    ITF_NUM_TOTAL_T
};

#define EPNUM_CDC_NOTIF_T   0x81
#define EPNUM_CDC_OUT_T     0x02
#define EPNUM_CDC_IN_T      0x82
#define EPNUM_MSC_OUT_T     0x03
#define EPNUM_MSC_IN_T      0x83

#define CONFIG_TRANSFER_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MSC_DESC_LEN)

/* An SD card under sustained write is the load this profile exists for. */
#define USB_TRANSFER_MAX_POWER_MA 500

/* Same PID as the CDC profile on purpose: every host tool finds the command
 * port by 0x303A:0x3456, and losing it here would leave no way back out of the
 * mode from the host. The profile nibble in bcdDevice is what tells them (and
 * Windows, which matches its driver stack on ...&REV_xxxx) apart. */
const tusb_desc_device_t desc_device_transfer = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = 64,
    .idVendor = 0x303A,
    .idProduct = 0x3456,
    .bcdDevice = USB_BCD_DEVICE(USB_BCD_PROFILE_TRANSFER),
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 1
};

const uint8_t desc_cfg_transfer[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL_T, 0, CONFIG_TRANSFER_TOTAL_LEN, 0, USB_TRANSFER_MAX_POWER_MA),
    TUD_CDC_DESCRIPTOR(ITF_NUM_CDC_T, 0, EPNUM_CDC_NOTIF_T, 8, EPNUM_CDC_OUT_T, EPNUM_CDC_IN_T, 64),
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC_T, 4, EPNUM_MSC_OUT_T, EPNUM_MSC_IN_T, 64),
};

const char* s_str_desc_transfer[] = {
    (char[]){0x09, 0x04},   // 0: language ID (English, 0x0409)
    "TinyUSB",              // 1: Manufacturer
    "Crosspad",             // 2: Product
    NULL,                   // 3: Serial — usb_serial_string() at install
    "Crosspad SD Card",     // 4: MSC interface
};
