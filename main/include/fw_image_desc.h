#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_app_desc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_DESC_MAGIC       0x49465043u
#define FW_DESC_VERSION     1
#define FW_DESC_FILE_OFFSET 0x120
#define FW_KIT_ID_LEN       16
#define FW_ASSETS_ID_LEN    32

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t  desc_version;
    uint8_t  board_rev;
    uint16_t stm_proto_min;
    char     kit_id[FW_KIT_ID_LEN];
    uint8_t  assets_id[FW_ASSETS_ID_LEN];
} fw_image_desc_t;

const fw_image_desc_t *fw_image_desc_running(void);

/* Reads both descriptions from an image file. Returns false when the file is
 * not an app image; *has_desc is false for an image built before CPFI. */
bool fw_image_desc_read_file(const char *path, esp_app_desc_t *app,
                             fw_image_desc_t *desc, bool *has_desc);

#ifdef __cplusplus
}
#endif
