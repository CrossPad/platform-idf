#pragma once
#include <stdint.h>
#include "tusb.h"
#include "crosspad_s3.h"

/* bcdDevice: 0x05 major, board revision nibble, profile nibble — the host reads
 * the revision the image was built for without opening any port. */
#define USB_BCD_BASE            0x0500
#define USB_BCD_REV_SHIFT       4
#define USB_BCD_PROFILE_CDC     0x1
#define USB_BCD_PROFILE_AUDIO   0x0
#define USB_BCD_PROFILE_TRANSFER 0x2
#define USB_BCD_DEVICE(profile) (USB_BCD_BASE | (BSP_BOARD_REV_NUM << USB_BCD_REV_SHIFT) | (profile))

/* Base MAC as "44:1B:F6:8B:22:38", the ROM bootloader's serial format. */
#ifdef __cplusplus
extern "C" {
#endif
const char *usb_serial_string(void);
#ifdef __cplusplus
}
#endif

extern const char* s_str_desc[5];
extern const tusb_desc_device_t desc_device;
extern const uint8_t desc_fs_configuration[];