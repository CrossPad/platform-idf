#include "fw_image_desc.h"
#include <stdio.h>
#include <string.h>
#include "esp_app_format.h"
#include "crosspad_s3.h"
#include "crosspad/stm32/reg_map.h"
#include "assets_id.h"

#ifndef CROSSPAD_FW_KIT_ID
#define CROSSPAD_FW_KIT_ID "full"
#endif

#define APP_DESC_FILE_OFFSET (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

_Static_assert(APP_DESC_FILE_OFFSET + sizeof(esp_app_desc_t) == FW_DESC_FILE_OFFSET,
               "CPFI must follow esp_app_desc_t");

/* The linker script places .rodata_custom_desc directly after .rodata_desc. */
const __attribute__((section(".rodata_custom_desc"), used)) fw_image_desc_t s_fw_image_desc = {
    .magic         = FW_DESC_MAGIC,
    .desc_version  = FW_DESC_VERSION,
    .board_rev     = BSP_BOARD_REV_NUM,
    .stm_proto_min = STM32_PROTO_VERSION,
    .kit_id        = CROSSPAD_FW_KIT_ID,
    .assets_id     = FW_ASSETS_ID_BYTES,
};

const fw_image_desc_t *fw_image_desc_running(void) { return &s_fw_image_desc; }

bool fw_image_desc_read_file(const char *path, esp_app_desc_t *app,
                             fw_image_desc_t *desc, bool *has_desc)
{
    FILE *f = fopen(path, "rb");
    if (!f) { *has_desc = false; return false; }
    bool ok = fseek(f, APP_DESC_FILE_OFFSET, SEEK_SET) == 0 &&
              fread(app, sizeof(*app), 1, f) == 1 &&
              app->magic_word == ESP_APP_DESC_MAGIC_WORD;
    *has_desc = ok && fread(desc, sizeof(*desc), 1, f) == 1 &&
                desc->magic == FW_DESC_MAGIC && desc->desc_version == FW_DESC_VERSION;
    fclose(f);
    return ok;
}
