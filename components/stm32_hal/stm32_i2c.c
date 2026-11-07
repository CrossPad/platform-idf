/*
 * STM32 I2C master link — see stm32_i2c.h.
 *
 * Reuses the BSP I2C bus (bsp_i2c_get_handle) so the codecs / touch / IMU
 * keep working on the same SCL / SDA. STM32 firmware exposes a 16-bit
 * register address space starting at 0x42 (see stm32_reg_map.h).
 */

#include "stm32_i2c.h"
#include "stm32_reg_map.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_rom_sys.h"           /* esp_rom_delay_us — busy-wait between retries */

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "crosspad_s3.h"           /* bsp_i2c_get_handle() */
#include "bsp/esp-bsp.h"           /* BSP_HAS_STM32_I2C, BSP_I2C_STM32_ADDR */
#include "driver/i2c_master.h"
#endif
/* Defined (0 or 1) by every CrossPad pinout: undefined here means the header
 * is missing, and each test below would quietly take the rev1 path. */
#if defined(BSP_BOARD_CROSSPAD_S3) && !defined(BSP_HAS_STM32_I2C)
#error "BSP_HAS_STM32_I2C is undefined: include bsp/esp-bsp.h before testing it"
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C

static const char *TAG = "STM32_I2C";

#ifndef BSP_I2C_STM32_ADDR
#define BSP_I2C_STM32_ADDR          STM32_I2C_ADDR_7B
#endif

/* 400 kHz — matches STM32 I2C1 Fast-mode (0x00C12166 timing). The STM32 is a
 * software slave (HAL Sequential IT) so we keep clock-stretching enabled on
 * the master side via the default driver behavior. */
#define STM32_I2C_SCL_HZ            400000
#define STM32_I2C_XFER_TIMEOUT_MS   50

/* Retry policy for transient bus glitches. The BQ25895 shares the bus over the
 * STM proxy and can briefly stall ACKs during charge-state transitions; 3
 * tries with a 1 ms busy-wait between is cheap and catches the common cases
 * without masking a truly dead link. */
#define STM32_I2C_RETRY_COUNT       3
#define STM32_I2C_RETRY_GAP_US      1000

static i2c_master_dev_handle_t s_dev = NULL;
static bool                    s_initialized = false;

/* Cached identity (CPAD magic confirmed) — populated on first successful
 * stm32_i2c_probe() call. Lets downstream modules read proto/fw/pcb without
 * re-running the handshake. */
static struct {
    bool     valid;
    uint16_t proto_version;
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  pcb_rev;
    uint8_t  fw_pcb;
} s_ident_cache;

static inline uint8_t reg_hi(uint16_t reg) { return (uint8_t)((reg >> 8) & 0xFFu); }
static inline uint8_t reg_lo(uint16_t reg) { return (uint8_t)(reg & 0xFFu); }

esp_err_t stm32_i2c_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    ESP_RETURN_ON_FALSE(bus != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "BSP I2C bus not initialized");

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = BSP_I2C_STM32_ADDR,
        .scl_speed_hz    = STM32_I2C_SCL_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &s_dev),
                        TAG, "failed to add STM32 device @0x%02X",
                        BSP_I2C_STM32_ADDR);

    s_initialized = true;
    ESP_LOGI(TAG, "STM32 I2C link up @0x%02X, %d Hz",
             BSP_I2C_STM32_ADDR, STM32_I2C_SCL_HZ);
    return ESP_OK;
}

esp_err_t stm32_i2c_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }
    esp_err_t err = i2c_master_bus_rm_device(s_dev);
    s_dev = NULL;
    s_initialized = false;
    return err;
}

bool stm32_i2c_is_initialized(void)
{
    return s_initialized;
}

esp_err_t stm32_i2c_read(uint16_t reg, uint8_t *dst, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(dst && len > 0 && len <= 255, ESP_ERR_INVALID_ARG,
                        TAG, "bad args");

    const uint8_t addr_be[2] = { reg_hi(reg), reg_lo(reg) };
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < STM32_I2C_RETRY_COUNT; ++attempt) {
        err = i2c_master_transmit_receive(s_dev,
                                          addr_be, sizeof(addr_be),
                                          dst, len,
                                          STM32_I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        esp_rom_delay_us(STM32_I2C_RETRY_GAP_US);
    }
    return err;
}

esp_err_t stm32_i2c_write(uint16_t reg, const uint8_t *src, size_t len)
{
    ESP_RETURN_ON_FALSE(s_initialized, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    ESP_RETURN_ON_FALSE(src && len > 0 && len <= 255, ESP_ERR_INVALID_ARG,
                        TAG, "bad args");

    /* Pack [RegHi | RegLo | Data...] into a single transmit so the slave
     * sees one continuous frame. 2-byte addr + up to 255 data bytes fits in
     * a 257-byte stack buffer. */
    uint8_t buf[257];
    buf[0] = reg_hi(reg);
    buf[1] = reg_lo(reg);
    for (size_t i = 0; i < len; ++i) {
        buf[2 + i] = src[i];
    }
    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < STM32_I2C_RETRY_COUNT; ++attempt) {
        err = i2c_master_transmit(s_dev, buf, len + 2u, STM32_I2C_XFER_TIMEOUT_MS);
        if (err == ESP_OK) return ESP_OK;
        esp_rom_delay_us(STM32_I2C_RETRY_GAP_US);
    }
    return err;
}

esp_err_t stm32_i2c_read_u8(uint16_t reg, uint8_t *out)
{
    return stm32_i2c_read(reg, out, 1);
}

esp_err_t stm32_i2c_write_u8(uint16_t reg, uint8_t value)
{
    return stm32_i2c_write(reg, &value, 1);
}

esp_err_t stm32_i2c_probe(uint16_t *proto_version,
                          uint8_t *fw_major,
                          uint8_t *fw_minor,
                          uint8_t *pcb_rev)
{
    uint8_t ident[REG_IDENT_LEN] = {0};
    esp_err_t err = stm32_i2c_read(REG_IDENTITY_BASE, ident, sizeof(ident));
    if (err != ESP_OK) {
        return err;
    }
    if (ident[0] != STM32_IDENT_MAGIC_C ||
        ident[1] != STM32_IDENT_MAGIC_P ||
        ident[2] != STM32_IDENT_MAGIC_A ||
        ident[3] != STM32_IDENT_MAGIC_D) {
        ESP_LOGE(TAG, "magic mismatch: %02X %02X %02X %02X",
                 ident[0], ident[1], ident[2], ident[3]);
        return ESP_ERR_INVALID_RESPONSE;
    }
    uint16_t proto = ((uint16_t)ident[4] << 8) | ident[5];
    if (proto_version) *proto_version = proto;
    if (fw_major)      *fw_major      = ident[6];
    if (fw_minor)      *fw_minor      = ident[7];
    if (pcb_rev)       *pcb_rev       = ident[8];

    s_ident_cache.valid         = true;
    s_ident_cache.proto_version = proto;
    s_ident_cache.fw_major      = ident[6];
    s_ident_cache.fw_minor      = ident[7];
    s_ident_cache.pcb_rev       = ident[8];
    /* Pre-0x000F firmware answers 0xFF for the unknown address: "unknown". */
    s_ident_cache.fw_pcb        = ident[10] == 0xFF ? 0 : ident[10];

    ESP_LOGI(TAG, "STM32 ident: proto=%u.%u fw=%u.%u pcb=%u fw_pcb=%u",
             ident[4], ident[5], ident[6], ident[7], ident[8], s_ident_cache.fw_pcb);
    return ESP_OK;
}

uint8_t stm32_i2c_cached_fw_pcb(void)
{
    return s_ident_cache.valid ? s_ident_cache.fw_pcb : 0;
}

esp_err_t stm32_i2c_get_cached_ident(uint16_t *proto_version,
                                     uint8_t *fw_major,
                                     uint8_t *fw_minor,
                                     uint8_t *pcb_rev)
{
    if (!s_ident_cache.valid) return ESP_ERR_INVALID_STATE;
    if (proto_version) *proto_version = s_ident_cache.proto_version;
    if (fw_major)      *fw_major      = s_ident_cache.fw_major;
    if (fw_minor)      *fw_minor      = s_ident_cache.fw_minor;
    if (pcb_rev)       *pcb_rev       = s_ident_cache.pcb_rev;
    return ESP_OK;
}

#else  /* link not available on this board / target */

esp_err_t stm32_i2c_init(void)                      { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t stm32_i2c_deinit(void)                    { return ESP_OK; }
bool      stm32_i2c_is_initialized(void)            { return false; }
esp_err_t stm32_i2c_read(uint16_t r, uint8_t *d, size_t l)        { (void)r; (void)d; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t stm32_i2c_write(uint16_t r, const uint8_t *s, size_t l) { (void)r; (void)s; (void)l; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t stm32_i2c_read_u8(uint16_t r, uint8_t *o)               { (void)r; (void)o; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t stm32_i2c_write_u8(uint16_t r, uint8_t v)               { (void)r; (void)v; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t stm32_i2c_probe(uint16_t *p, uint8_t *fma, uint8_t *fmi, uint8_t *pcb)
{
    (void)p; (void)fma; (void)fmi; (void)pcb;
    return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t stm32_i2c_get_cached_ident(uint16_t *p, uint8_t *fma, uint8_t *fmi, uint8_t *pcb)
{
    (void)p; (void)fma; (void)fmi; (void)pcb;
    return ESP_ERR_INVALID_STATE;
}
uint8_t stm32_i2c_cached_fw_pcb(void) { return 0; }

#endif
