/**
 * @file stm32_hal_init.c
 * @brief STM32 HAL init — UART init + raw brightness command
 *
 * Pure C, no crosspad-core dependency. Framework logic (Stm32Manager
 * maintenance task, routing, DFU) is wired separately by
 * crosspad-platform-idf.
 */

#include "stm32_hal_init.h"
#include "stm32_uart.h"
#include "stm32_i2c.h"
#include "stm32_reg_map.h"
#include "stm32_pwr_sync.h"
#include "esp_log.h"
#include "esp_timer.h"

#ifdef CONFIG_IDF_TARGET_ESP32S3
#include "bsp/esp-bsp.h"   /* BSP_HAS_STM32_I2C */
#endif
/* Defined (0 or 1) by every CrossPad pinout: undefined here means the header
 * is missing, and each test below would quietly take the rev1 path. */
#if defined(BSP_BOARD_CROSSPAD_S3) && !defined(BSP_HAS_STM32_I2C)
#error "BSP_HAS_STM32_I2C is undefined: include bsp/esp-bsp.h before testing it"
#endif

static const char* TAG = "STM32_HAL";

/* SysEx protocol constants, duplicated from SysExProtocol.h to keep this file
 * free of a crosspad-core dependency. */
#define SYSEX_START        0xF0
#define SYSEX_END          0xF7
#define SYSEX_MANUFACTURER 0x7D
#define CMD_BRIGHTNESS     0x16
/* A rev 1.x STM's control register write (the code a rev 2 STM uses for the
 * USB mode) and its version register, whose write it answers with
 * CROSSPAD_CMD_VERSION_REPLY [pcb_rev, api_version]. */
#define CMD_UART_CONTROL   0x1B
#define UART_CR_VERSION    0x00

static bool s_initialized = false;
static stm32_link_info_t s_link_info;   /* I2C probe handshake, or the UART version reply */

/* Last non-zero brightness the app requested, replayed by the app-level power
 * handler to relight the panel: the STM wakes with the backlight off and hands
 * brightness ownership to the ESP. */
static uint8_t s_last_brightness = 90;

/* MIDI Active Sensing heartbeat: the STM treats 0xFE on its UART RX as
 * proof-of-life (2000 ms timeout), so send every 500 ms for 4x margin.
 * Real-time bytes are transparent to MIDI parsers and to the STM's SysEx FSM,
 * so injecting one mid-stream is safe. */
#define STM32_HEARTBEAT_PERIOD_US   (500 * 1000)
static esp_timer_handle_t s_heartbeat_timer = NULL;
static bool s_heartbeat_suspended;

/* Without the I2C link the version is asked over UART, with the heartbeat
 * until the STM answers: a request sent before the ESP's MIDI parser runs
 * is answered into nothing. An STM older than the request never answers, so
 * the asking stops after this many beats. */
#define UART_VERSION_REQUESTS  20

static void heartbeat_timer_cb(void *arg)
{
    (void)arg;
    static const uint8_t hb = 0xFE;   /* MIDI Active Sensing */
    stm32_uart_send(&hb, 1);
#if !(defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C)
    static unsigned s_version_requests;
    if (!s_link_info.linked && s_version_requests < UART_VERSION_REQUESTS) {
        static const uint8_t ver_req[] = {
            SYSEX_START, SYSEX_MANUFACTURER,
            CMD_UART_CONTROL, UART_CR_VERSION, 0x00,
            SYSEX_END
        };
        stm32_uart_send(ver_req, sizeof(ver_req));
        ++s_version_requests;
    }
#endif
}

static void heartbeat_resume_cb(void *arg)
{
    (void)arg;
    if (s_heartbeat_timer && !s_heartbeat_suspended) {
        esp_timer_start_periodic(s_heartbeat_timer, STM32_HEARTBEAT_PERIOD_US);
    }
}

void stm32_hal_heartbeat_suspend(bool suspend)
{
    s_heartbeat_suspended = suspend;
    if (!s_heartbeat_timer) return;
    if (suspend) {
        esp_timer_stop(s_heartbeat_timer);
    } else if (!esp_timer_is_active(s_heartbeat_timer)) {
        esp_timer_start_periodic(s_heartbeat_timer, STM32_HEARTBEAT_PERIOD_US);
    }
}

void stm32_hal_heartbeat_pause_ms(uint32_t ms)
{
    if (!s_heartbeat_timer || s_heartbeat_suspended) return;
    esp_timer_stop(s_heartbeat_timer);
    static esp_timer_handle_t s_resume_timer = NULL;
    if (!s_resume_timer) {
        const esp_timer_create_args_t args = {
            .callback = heartbeat_resume_cb,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "stm32_hb_resume",
            .skip_unhandled_events = true,
        };
        if (esp_timer_create(&args, &s_resume_timer) != ESP_OK) {
            heartbeat_resume_cb(NULL);
            return;
        }
    }
    esp_timer_stop(s_resume_timer);
    esp_timer_start_once(s_resume_timer, (uint64_t)ms * 1000ULL);
}

/* Default power-state callback: log + auto-ACK. An application overrides it
 * with stm32_pwr_sync_set_callback() to flush NVS, mute audio or clean up
 * before calling stm32_pwr_sync_ack(). The auto-ACK lets the STM continue its
 * teardown even with no application handler — at worst the ESP loses
 * unfinished buffers, rather than leaving the STM in its wait loop. */
static void default_pwr_state_cb(uint8_t state, void *user_data)
{
    (void)user_data;
    switch (state) {
        case PWR_STATE_SLEEP_PENDING:
            ESP_LOGI(TAG, "PWR: SLEEP_PENDING — ack");
            stm32_pwr_sync_ack();
            break;
        case PWR_STATE_SHIP_PENDING:
            ESP_LOGW(TAG, "PWR: SHIP_PENDING — ack, expect power loss");
            stm32_pwr_sync_ack();
            break;
        case PWR_STATE_WOKE:
            /* The STM woke with the backlight off and a stale LCD frame. The
             * ACK stops its 250 ms WOKE retry loop; relighting is left to the
             * app-level override, which must redraw before raising
             * brightness or the stale frame flashes. */
            ESP_LOGI(TAG, "PWR: STM woke (app should redraw + set brightness)");
            stm32_pwr_sync_ack();
            break;
        case PWR_STATE_LIGHT:
            ESP_LOGI(TAG, "PWR: LIGHT sleep (STM backlight off; app dims visuals)");
            break;
        case PWR_STATE_AWAKE:
            ESP_LOGI(TAG, "PWR: AWAKE (light-exit)");
            break;
        case PWR_STATE_CHARGE:
            ESP_LOGI(TAG, "PWR: CHARGE idle");
            break;
        default:
            ESP_LOGW(TAG, "PWR: unknown state 0x%02X", state);
            break;
    }
}

esp_err_t stm32_hal_init(void)
{
    if (s_initialized) {
        ESP_LOGW(TAG, "STM32 HAL already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing STM32 HAL");

    esp_err_t ret = stm32_uart_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize STM32 UART: %s", esp_err_to_name(ret));
        return ret;
    }

    /* Default power-state handler; an application overrides it after
     * stm32_hal_init() with stm32_pwr_sync_set_callback(). */
    stm32_pwr_sync_set_callback(default_pwr_state_cb, NULL);

    /* Periodic heartbeat to STM (MIDI Active Sensing). Created after UART is
     * up so the first tick can actually transmit. */
    const esp_timer_create_args_t hb_args = {
        .callback        = heartbeat_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "stm32_hb",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&hb_args, &s_heartbeat_timer) == ESP_OK) {
        esp_timer_start_periodic(s_heartbeat_timer, STM32_HEARTBEAT_PERIOD_US);
    } else {
        ESP_LOGW(TAG, "Failed to create STM32 heartbeat timer");
    }

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    /* I2C control/status link (rev2). The UART stays the high-bandwidth MIDI
     * pipe; I2C carries the slower register traffic. */
    ret = stm32_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "STM32 I2C init failed: %s — continuing UART-only",
                 esp_err_to_name(ret));
    } else {
        uint16_t proto = 0; uint8_t fma = 0, fmi = 0, pcb = 0;
        if (stm32_i2c_probe(&proto, &fma, &fmi, &pcb) != ESP_OK) {
            ESP_LOGW(TAG, "STM32 I2C probe failed — link will retry on first use");
        } else {
            s_link_info.linked        = true;
            s_link_info.proto_version = proto;
            s_link_info.fw_major      = fma;
            s_link_info.fw_minor      = fmi;
            s_link_info.pcb_rev       = pcb;
            s_link_info.fw_pcb        = stm32_i2c_cached_fw_pcb();
            if (proto != STM32_PROTO_VERSION) {
                ESP_LOGE(TAG, "STM proto version mismatch: STM=0x%04X, IDF=0x%04X "
                              "(register map may be partially out of sync)",
                         proto, STM32_PROTO_VERSION);
            }
        }
    }
#endif

    s_initialized = true;
    ESP_LOGI(TAG, "STM32 HAL initialized");
    return ESP_OK;
}

esp_err_t stm32_hal_refresh_link_info(void)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    if (!stm32_i2c_is_initialized()) return ESP_ERR_INVALID_STATE;
    uint16_t proto = 0; uint8_t fma = 0, fmi = 0, pcb = 0;
    esp_err_t err = stm32_i2c_probe(&proto, &fma, &fmi, &pcb);
    if (err != ESP_OK) return err;
    s_link_info.linked        = true;
    s_link_info.proto_version = proto;
    s_link_info.fw_major      = fma;
    s_link_info.fw_minor      = fmi;
    s_link_info.pcb_rev       = pcb;
    s_link_info.fw_pcb        = stm32_i2c_cached_fw_pcb();
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t stm32_hal_deinit(void)
{
    if (!s_initialized) {
        return ESP_OK;
    }

    if (s_heartbeat_timer) {
        esp_timer_stop(s_heartbeat_timer);
        esp_timer_delete(s_heartbeat_timer);
        s_heartbeat_timer = NULL;
    }

    esp_err_t ret = stm32_uart_deinit();
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    stm32_i2c_deinit();
#endif
    s_initialized = false;
    return ret;
}

esp_err_t stm32_hal_set_brightness(uint8_t brightness)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (brightness > 100) {
        return ESP_ERR_INVALID_ARG;
    }
    if (brightness > 0) s_last_brightness = brightness;   /* cache for post-wake relight */

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    /* rev2 exposes brightness as REG_CTL_BRIGHTNESS on the I2C slave and no
     * longer handles the legacy SysEx; falls back to UART if I2C is not up. */
    if (stm32_i2c_is_initialized()) {
        /* STM32 backlight_set() expects 0..100 — same scale as this API. */
        return stm32_i2c_write_u8(REG_CTL_BRIGHTNESS, brightness);
    }
#endif

    const uint8_t msg[] = {
        SYSEX_START, SYSEX_MANUFACTURER,
        CMD_BRIGHTNESS, brightness,
        SYSEX_END
    };
    int written = stm32_uart_send(msg, sizeof(msg));
    return (written == sizeof(msg)) ? ESP_OK : ESP_FAIL;
}

uint8_t stm32_hal_get_last_brightness(void)
{
    return s_last_brightness;
}

bool stm32_hal_is_initialized(void)
{
    return s_initialized;
}

bool stm32_hal_is_linked(void)
{
    return s_link_info.linked;
}

void stm32_hal_set_uart_version(uint8_t pcb_rev, uint8_t api_version)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C
    (void)pcb_rev;
    (void)api_version;
#else
    s_link_info.proto_version = api_version;
    s_link_info.fw_major      = 0;
    s_link_info.fw_minor      = 0;
    s_link_info.pcb_rev       = pcb_rev;
    s_link_info.fw_pcb        = 0;
    s_link_info.uart_only     = true;
    s_link_info.linked        = true;
    ESP_LOGI(TAG, "STM32 over UART: PCB r%u, protocol 0x%04X", pcb_rev, api_version);
#endif
}

esp_err_t stm32_hal_get_link_info(stm32_link_info_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!s_link_info.linked) return ESP_ERR_INVALID_STATE;
    *out = s_link_info;
    return ESP_OK;
}

#if defined(CONFIG_IDF_TARGET_ESP32S3) && defined(BSP_HAS_STM32_I2C) && BSP_HAS_STM32_I2C

/* Block size from REG_PWR_CHG_VBUS_HI (REG_POWER_BASE + 0x0D) through
 * REG_PWR_CHG_TGT_LIM_LO (REG_POWER_BASE + 0x22) inclusive. */
#define STM32_HAL_CHG_BLOCK_LEN  (REG_PWR_CHG_TGT_LIM_LO - REG_PWR_CHG_VBUS_HI + 1)

esp_err_t stm32_hal_get_charger_telemetry(stm32_charger_telemetry_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    if (!stm32_i2c_is_initialized()) return ESP_ERR_INVALID_STATE;

    uint8_t buf[STM32_HAL_CHG_BLOCK_LEN];
    esp_err_t err = stm32_i2c_read(REG_PWR_CHG_VBUS_HI, buf, sizeof(buf));
    if (err != ESP_OK) return err;

    /* STM direct-ADC battery, separate from the BQ block and always live —
     * the BQ BATV only reads while the charger ADC converts. Preferred source
     * for the SoC gauge. Non-fatal: leave 0 and let the caller fall back. */
    out->vbat_adc_mv = 0;
    uint8_t vb[2];
    if (stm32_i2c_read(REG_PWR_VBAT_HI, vb, sizeof(vb)) == ESP_OK) {
        out->vbat_adc_mv = ((uint16_t)vb[0] << 8) | vb[1];
    }

    /* SoC estimator block, firmware reg ver >= 0x000A. Older firmware returns
     * 0xFF, and the caller gates on REG_SOC_FLAG_VALID + soc_pct <= 100. */
    out->soc_pct = 0; out->soc_permil = 0; out->soc_flags = 0;
    uint8_t sb[4];
    if (stm32_i2c_read(REG_PWR_SOC_PCT, sb, sizeof(sb)) == ESP_OK) {
        out->soc_pct    = sb[0];
        out->soc_permil = ((uint16_t)sb[1] << 8) | sb[2];
        out->soc_flags  = sb[3];
    }

    out->vbus_mv          = ((uint16_t)buf[0]  << 8) | buf[1];
    out->vsys_mv          = ((uint16_t)buf[2]  << 8) | buf[3];
    out->vbat_mv          = ((uint16_t)buf[4]  << 8) | buf[5];
    out->ichg_ma          = ((uint16_t)buf[6]  << 8) | buf[7];
    out->ts_pct           = buf[8];
    out->vbus_stat        = buf[9];
    out->chrg_stat        = buf[10];
    out->iinlim_ma        = ((uint16_t)buf[11] << 8) | buf[12];
    out->idpm_lim_ma      = ((uint16_t)buf[13] << 8) | buf[14];
    out->vindpm_mv        = ((uint16_t)buf[15] << 8) | buf[16];
    out->ntc_fault        = buf[17];
    out->flags            = buf[18];
    out->src_active       = buf[19];
    out->target_iinlim_ma = ((uint16_t)buf[20] << 8) | buf[21];
    return ESP_OK;
}

/* HI then LO: the STM commits the 16-bit value on the LO write, so the pair
 * must arrive in order. One multi-byte transfer keeps the pointer monotonic. */
static esp_err_t stm32_hal_write_u16_be(uint16_t reg_hi_addr, uint16_t value)
{
    const uint8_t pair[2] = {
        (uint8_t)((value >> 8) & 0xFFu),
        (uint8_t)(value & 0xFFu),
    };
    return stm32_i2c_write(reg_hi_addr, pair, sizeof(pair));
}

esp_err_t stm32_hal_set_dim_timeout_s(uint16_t seconds)
{
    if (!stm32_i2c_is_initialized()) return ESP_ERR_INVALID_STATE;
    return stm32_hal_write_u16_be(REG_CTL_DIM_TO_S_HI, seconds);
}

esp_err_t stm32_hal_set_sleep_timeout_s(uint16_t seconds)
{
    if (!stm32_i2c_is_initialized()) return ESP_ERR_INVALID_STATE;
    return stm32_hal_write_u16_be(REG_CTL_SLEEP_TO_S_HI, seconds);
}

#else  /* link not available */

esp_err_t stm32_hal_get_charger_telemetry(stm32_charger_telemetry_t *out)
{
    (void)out; return ESP_ERR_NOT_SUPPORTED;
}
esp_err_t stm32_hal_set_dim_timeout_s(uint16_t s)   { (void)s; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t stm32_hal_set_sleep_timeout_s(uint16_t s) { (void)s; return ESP_ERR_NOT_SUPPORTED; }

#endif
