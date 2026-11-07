#include "bsp_ulp.h"

#include "esp_log.h"
#include "ulp_riscv.h"
#include "ulp_bsp_background.h"

static const char *TAG = "BSP_ULP";

extern const uint8_t ulp_bin_start[] asm("_binary_ulp_bsp_background_bin_start");
extern const uint8_t ulp_bin_end[] asm("_binary_ulp_bsp_background_bin_end");

static bool s_running = false;

esp_err_t bsp_ulp_start(void)
{
    if (s_running) return ESP_OK;

    esp_err_t err = ulp_riscv_load_binary(ulp_bin_start, ulp_bin_end - ulp_bin_start);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "load failed: %s", esp_err_to_name(err));
        return err;
    }

    ulp_set_wakeup_period(0, BSP_ULP_PERIOD_MS * 1000u);
    err = ulp_riscv_run();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "run failed: %s", esp_err_to_name(err));
        return err;
    }
    s_running = true;
    ESP_LOGI(TAG, "ULP RISC-V started, period %d ms, %u B program",
             BSP_ULP_PERIOD_MS, (unsigned)(ulp_bin_end - ulp_bin_start));
    return ESP_OK;
}

void bsp_ulp_get_status(bsp_ulp_status_t *out)
{
    out->running = s_running;
    out->heartbeat = s_running ? ulp_heartbeat : 0;
}
