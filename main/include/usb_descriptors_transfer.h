#pragma once
#include <stdint.h>
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const tusb_desc_device_t desc_device_transfer;
extern const uint8_t desc_cfg_transfer[];
extern const char* s_str_desc_transfer[];

#define USB_TRANSFER_STR_DESC_COUNT 5

#ifdef __cplusplus
}
#endif
