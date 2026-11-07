/*
 * STM32 power-state SysEx parser + ACK helper.
 * Protocol contract: stm32_pwr_sync.h.
 */

#include "stm32_pwr_sync.h"
#include "stm32_reg_map.h"
#include "stm32_i2c.h"
#include "esp_log.h"

static const char *TAG = "PWR_SYNC";

/* SysEx state machine. Only F0 7D <cmd> <arg> F7 (our vendor 0x7D) is acted
 * on; any other SysEx passes through untouched to the MIDI parser above. */
typedef enum {
    PS_IDLE = 0,         /* waiting for 0xF0 */
    PS_AFTER_F0,         /* widziałem 0xF0, oczekuję manufacturer */
    PS_AFTER_MFG,        /* 0xF0 0x7D, oczekuję cmd */
    PS_AFTER_CMD,        /* 0xF0 0x7D <cmd>, oczekuję pierwszy arg */
    PS_COLLECT_TAIL,     /* arg consumed, waiting for 0xF7 */
    PS_SKIP_FOREIGN,     /* another vendor's SysEx, waiting for 0xF7 */
} parser_state_t;

static parser_state_t s_state = PS_IDLE;
static uint8_t        s_cmd;
static stm32_pwr_state_cb_t s_cb;
static void               *s_cb_user;

void stm32_pwr_sync_set_callback(stm32_pwr_state_cb_t cb, void *user_data) {
    s_cb = cb;
    s_cb_user = user_data;
}

static void handle_pwr_state(uint8_t state) {
    ESP_LOGI(TAG, "PWR IRQ: state=0x%02X", state);
    if (s_cb) s_cb(state, s_cb_user);
}

void stm32_pwr_sync_feed_byte(uint8_t b) {
    /* MIDI real-time bytes (0xF8..0xFF) do not reset a SysEx stream — that is
     * the MIDI 1.0 real-time interrupt rule. Pass them through. */
    if (b >= 0xF8) return;

    switch (s_state) {
        case PS_IDLE:
            if (b == 0xF0) s_state = PS_AFTER_F0;
            break;

        case PS_AFTER_F0:
            if (b == 0x7D)       s_state = PS_AFTER_MFG;
            else if (b == 0xF7)  s_state = PS_IDLE;
            else                 s_state = PS_SKIP_FOREIGN;
            break;

        case PS_AFTER_MFG:
            s_cmd = b;
            s_state = PS_AFTER_CMD;
            break;

        case PS_AFTER_CMD:
            if (s_cmd == CROSSPAD_SYSEX_PWR_IRQ) {
                handle_pwr_state(b);
            }
            /* After the first arg only F7 matters; our commands carry a
             * single argument, so further bytes are ignored. */
            s_state = PS_COLLECT_TAIL;
            break;

        case PS_COLLECT_TAIL:
        case PS_SKIP_FOREIGN:
            if (b == 0xF7) s_state = PS_IDLE;
            break;
    }
}

void stm32_pwr_sync_feed(const uint8_t *buf, size_t len) {
    if (!buf) return;
    for (size_t i = 0; i < len; i++) {
        stm32_pwr_sync_feed_byte(buf[i]);
    }
}

esp_err_t stm32_pwr_sync_ack(void) {
    if (!stm32_i2c_is_initialized()) {
        ESP_LOGW(TAG, "ACK skipped — I2C not initialized");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = stm32_i2c_write_u8(REG_CMD_PWR_ACK, PWR_ACK_MAGIC);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ACK write failed: %s", esp_err_to_name(err));
    }
    return err;
}
