#pragma once
#include <stdint.h>
#include "tusb.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const tusb_desc_device_t desc_device_audio;
extern const uint8_t desc_cfg_audio[];
extern const char* s_str_desc_audio[];

#ifdef __cplusplus
}
#endif
