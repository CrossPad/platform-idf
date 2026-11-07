/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_es8388.h"
#include "crosspad_s3.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp_err_check.h"
#include "esp_codec_dev_defaults.h"
#include <math.h>
#include <string.h>

static const char *TAG = "bsp_es8388";

/* ES8388 Register definitions */
#define ES8388_CONTROL1         0x00
#define ES8388_CONTROL2         0x01
#define ES8388_CHIPPOWER        0x02
#define ES8388_ADCPOWER         0x03
#define ES8388_DACPOWER         0x04
#define ES8388_CHIPLOPOW1       0x05
#define ES8388_CHIPLOPOW2       0x06
#define ES8388_ANAVOLMANAG      0x07
#define ES8388_MASTERMODE       0x08
#define ES8388_ADCCONTROL1      0x09
#define ES8388_ADCCONTROL2      0x0A
#define ES8388_ADCCONTROL3      0x0B
#define ES8388_ADCCONTROL4      0x0C
#define ES8388_ADCCONTROL5      0x0D
#define ES8388_ADCCONTROL7      0x0F  /* ADC mute / noise gate / DC offset ctrl */
#define ES8388_ADCCONTROL8      0x10  /* L ADC digital volume (0x00=0dB .. 0xC0=-96dB, default 0xC0) */
#define ES8388_ADCCONTROL9      0x11  /* R ADC digital volume */
#define ES8388_ADCCONTROL10     0x12
#define ES8388_DACCONTROL1      0x17
#define ES8388_DACCONTROL2      0x18
#define ES8388_DACCONTROL3      0x19
#define ES8388_DACCONTROL4      0x1A  /* Left DAC digital volume (0x00=0dB .. 0xC0=-96dB) */
#define ES8388_DACCONTROL5      0x1B  /* Right DAC digital volume */
#define ES8388_DACCONTROL6      0x1C  /* DAC de-emphasis / click-free control */
#define ES8388_DACCONTROL16     0x26  /* LMIXSEL - left mixer source */
#define ES8388_DACCONTROL17     0x27  /* Left output mixer control */
#define ES8388_DACCONTROL20     0x2A  /* Right output mixer control */
#define ES8388_DACCONTROL21     0x2B  /* bit7: ADCLRCK = DACLRCK */
#define ES8388_DACCONTROL23     0x2D
#define ES8388_DACCONTROL24     0x2E  /* LOUT1 analog volume (6-bit, 0x00-0x21) */
#define ES8388_DACCONTROL25     0x2F  /* ROUT1 analog volume */
#define ES8388_DACCONTROL26     0x30  /* LOUT2 analog volume (6-bit, 0x00-0x21) */
#define ES8388_DACCONTROL27     0x31  /* ROUT2 analog volume */

/* ES8388 state - support for two codecs */
static struct {
    i2c_master_dev_handle_t i2c_dev;
    bsp_es8388_config_t config;
    esp_codec_dev_handle_t codec_dev;
    int current_volume;
    int pair_volume[2];   /* LOUT1/ROUT1 and LOUT2/ROUT2, each set on its own */
    bool initialized;
    bool muted;
    bool warm_boot_skip;  /* set when codec retained valid config across ESP warm reset */
    bool outputs_on;      /* DAC output drivers powered (start() .. stop()) */
    uint8_t dac_outputs;  /* DACPOWER output-enable bits start() powers */
} s_es8388[BSP_I2S_INTERFACE_MAX] = {0};

static bsp_es8388_volume_cb_t s_volume_cb;

/* Read one ES8388 register. */
static esp_err_t es8388_read_reg(bsp_i2s_interface_t interface, uint8_t reg, uint8_t *value)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }
    uint8_t reg_addr = reg;
    return i2c_master_transmit_receive(s_es8388[interface].i2c_dev, &reg_addr, 1, value, 1, -1);
}

/* Debug-only: doubles I2C traffic and floods the UART (~140 lines at boot).
 * Define ES8388_VERIFY_WRITES=1 during codec bring-up. */
#ifndef ES8388_VERIFY_WRITES
#define ES8388_VERIFY_WRITES 0
#endif

/* Write one ES8388 register (read-back verified under ES8388_VERIFY_WRITES). */
static esp_err_t es8388_write_reg(bsp_i2s_interface_t interface, uint8_t reg, uint8_t value)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }
    uint8_t data[2] = {reg, value};
    esp_err_t ret = i2c_master_transmit(s_es8388[interface].i2c_dev, data, 2, -1);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%d] Reg 0x%02X write FAILED: %s", interface, reg, esp_err_to_name(ret));
        return ret;
    }

#if ES8388_VERIFY_WRITES
    uint8_t readback = 0;
    ret = es8388_read_reg(interface, reg, &readback);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "[%d] Reg 0x%02X readback FAILED: %s", interface, reg, esp_err_to_name(ret));
        return ret;
    }
    if (readback != value) {
        ESP_LOGW(TAG, "[%d] Reg 0x%02X MISMATCH: wrote=0x%02X read=0x%02X", interface, reg, value, readback);
    } else {
        ESP_LOGD(TAG, "[%d] Reg 0x%02X OK: 0x%02X", interface, reg, value);
    }
#endif
    return ESP_OK;
}

/* MicAmp stage asked for per codec, 0xFF = the init config's. Kept apart from
 * the codec's state so a gain set before the codec is up -- codec 1 comes up
 * late, from its own task -- is the one its init applies. */
static uint8_t s_mic_gain_set[BSP_I2S_INTERFACE_MAX] = {0xFF, 0xFF};

static esp_err_t es8388_config_adc(bsp_i2s_interface_t interface, uint8_t mic_amp_gain)
{
    if (s_mic_gain_set[interface] != 0xFF) mic_amp_gain = s_mic_gain_set[interface];
    /* ADCCONTROL1: bits[7:4]=L MicAmp gain, [3:0]=R MicAmp gain.
     * Each nibble: 0..8 → 0..+24dB in 3dB steps. Line-level input → 0 (no gain).
     * Mic-level input → 8 (+24dB). */
    if (mic_amp_gain > 8) mic_amp_gain = 8;
    const uint8_t adcctl1 = (uint8_t)((mic_amp_gain << 4) | mic_amp_gain);
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL1, adcctl1), TAG, "ADC control1 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL2, 0xF0), TAG, "ADC control2 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL3, 0x02), TAG, "ADC control3 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL4, 0x0C), TAG, "ADC control4 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL5, 0x02), TAG, "ADC control5 write failed");
    /* Reset defaults are ADC muted (0x20) + LADCVOL/RADCVOL=0xC0 (-96dB) — must override. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL7, 0x09), TAG, "ADC control7 unmute failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL8, 0x00), TAG, "L ADC vol write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL9, 0x00), TAG, "R ADC vol write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL10, 0x00), TAG, "ADC ALC write failed");
    return ESP_OK;
}

/* DACCONTROL1 as this board wants it: bit 7 (DACLRSWAP) set on a codec whose
 * jack has left and right crossed (pinout_v*.h). */
static uint8_t es8388_dac_ctl1(bsp_i2s_interface_t interface, uint8_t format)
{
    return (interface == BSP_I2S_INTERFACE_0 && BSP_CODEC0_DAC_LR_CROSSED) ? (uint8_t)(format | 0x80) : format;
}

/* DACCONTROL6 as this board wants it: bits 5:4 (DACINVL/DACINVR) set on a
 * codec whose jack inverts the signal (pinout_v*.h). */
static uint8_t es8388_dac_ctl6(bsp_i2s_interface_t interface, uint8_t value)
{
    return (interface == BSP_I2S_INTERFACE_1 && BSP_CODEC1_OUT_INVERTED) ? (uint8_t)(value | 0x30) : value;
}

/* Configure the DAC, per User Guide section 11. */
static esp_err_t es8388_config_dac(bsp_i2s_interface_t interface)
{
    /* DACCONTROL1=0x18: 16-bit Philips I2S. DACCONTROL2=0x02: MCLK/Fs=256. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL1, es8388_dac_ctl1(interface, 0x18)), TAG,
                        "DAC control1 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL2, 0x02), TAG, "DAC control2 write failed");
    /* DACCONTROL3=0x36: muted with slowest soft-ramp (UG 10.5). */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL3, 0x36), TAG, "DAC control3 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL4, 0x00), TAG, "DAC digital vol L write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL5, 0x00), TAG, "DAC digital vol R write failed");
    /* DACCONTROL6=0x08: digital click-free power up/down. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL6, es8388_dac_ctl6(interface, 0x08)), TAG,
                        "DAC control6 write failed");
    /* Output mixer: LD2LO=1, LI2LO=0 (DAC-only path). */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL16, 0x00), TAG, "DAC control16 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL17, 0xB8), TAG, "DAC left mixer write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL20, 0xB8), TAG, "DAC right mixer write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL21, 0x80), TAG, "DAC control21 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL23, 0x00), TAG, "DAC control23 write failed");
    /* LOUT/ROUT vol stays 0 in init — start() ramps to 0x1E for anti-pop. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL24, 0x00), TAG, "LOUT1 vol write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL25, 0x00), TAG, "ROUT1 vol write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL26, 0x00), TAG, "LOUT2 vol write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL27, 0x00), TAG, "ROUT2 vol write failed");
    return ESP_OK;
}

esp_err_t bsp_es8388_init(const bsp_es8388_config_t *config)
{
    if (config == NULL) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    bsp_i2s_interface_t interface = config->i2s_interface;

    if (interface >= BSP_I2S_INTERFACE_MAX) {
        ESP_LOGE(TAG, "Invalid I2S interface: %d", interface);
        return ESP_ERR_INVALID_ARG;
    }

    if (s_es8388[interface].initialized) {
        ESP_LOGW(TAG, "ES8388 interface %d already initialized", interface);
        return ESP_OK;
    }

    memcpy(&s_es8388[interface].config, config, sizeof(bsp_es8388_config_t));

    i2c_device_config_t i2c_dev_cfg = {
        .device_address = config->i2c_addr,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(config->i2c_handle, &i2c_dev_cfg, &s_es8388[interface].i2c_dev),
                        TAG, "Failed to add ES8388 to I2C bus");

    s_es8388[interface].initialized = true;
    s_es8388[interface].warm_boot_skip = false;

    /* The codec keeps its registers across an ESP-only reset. Skip the reinit
     * and its anti-pop ramp when the sentinel registers match what init()+start()
     * leave behind; a partial init has a different signature and falls
     * through to the cold path. */
    {
        uint8_t ctl1 = 0xFF, dctl1 = 0xFF, dctl6 = 0xFF, dpwr = 0xFF, lout = 0xFF, dll = 0xFF;
        bool ok = (es8388_read_reg(interface, ES8388_CONTROL1, &ctl1) == ESP_OK)
               && (es8388_read_reg(interface, ES8388_DACCONTROL1, &dctl1) == ESP_OK)
               && (es8388_read_reg(interface, ES8388_DACCONTROL6, &dctl6) == ESP_OK)
               && (es8388_read_reg(interface, ES8388_DACPOWER, &dpwr) == ESP_OK)
               && (es8388_read_reg(interface, ES8388_DACCONTROL24, &lout) == ESP_OK)
               && (es8388_read_reg(interface, 0x35, &dll) == ESP_OK);
        /* DACPOWER: DAC cores up (bits 7:6 clear), outputs whichever were
         * left powered -- a loop pair may be off. 0x35 at its reset value: a
         * chip with the DLL values below from an older firmware is reset.
         * DACCONTROL6 carries the board's output polarity. */
        if (ok && ctl1 == 0x19 && dctl1 == es8388_dac_ctl1(interface, 0x18)
            && dctl6 == es8388_dac_ctl6(interface, 0x08) && (dpwr & 0xC3) == 0 && (dpwr & 0x3C) && lout == 0x1E && dll == 0x00) {
            ESP_LOGW(TAG, "[%d] Warm boot — skipping full init", interface);
            s_es8388[interface].warm_boot_skip = true;
            s_es8388[interface].outputs_on = true;
            s_es8388[interface].dac_outputs = (uint8_t)(dpwr & 0x3C);
            s_es8388[interface].current_volume = 70;
            s_es8388[interface].pair_volume[0] = s_es8388[interface].pair_volume[1] = 70;
            s_es8388[interface].muted = false;
            return ESP_OK;
        }
    }

    /* Pre-init graceful shutdown per User Guide 10.5 — codec may still be
     * running from previous boot (ESP reset doesn't reset ES8388 via I2C). */
    es8388_write_reg(interface, 0x0F, 0x34);
    es8388_write_reg(interface, ES8388_DACCONTROL3, 0x36);
    vTaskDelay(pdMS_TO_TICKS(200));

    es8388_write_reg(interface, ES8388_DACCONTROL24, 0x00);
    es8388_write_reg(interface, ES8388_DACCONTROL25, 0x00);
    es8388_write_reg(interface, ES8388_DACCONTROL26, 0x00);
    es8388_write_reg(interface, ES8388_DACCONTROL27, 0x00);
    vTaskDelay(pdMS_TO_TICKS(80));

    es8388_write_reg(interface, ES8388_CHIPPOWER, 0xF3);
    es8388_write_reg(interface, ES8388_ADCPOWER, 0xFF);
    es8388_write_reg(interface, ES8388_DACPOWER, 0xC0);
    vTaskDelay(pdMS_TO_TICKS(30));
    es8388_write_reg(interface, ES8388_CONTROL2, 0x58);
    es8388_write_reg(interface, ES8388_CHIPPOWER, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Then a soft reset (CONTROL1 SCPReset), outputs already off: the init
     * below assumes a power-on chip, and the codec stays powered through an
     * ESP-only restart. */
    es8388_write_reg(interface, ES8388_CONTROL1, 0x80);
    vTaskDelay(pdMS_TO_TICKS(5));
    es8388_write_reg(interface, ES8388_CONTROL1, 0x00);

    /* User Guide 10.1 startup ordering — VMIDSEL via CONTROL1 must be set
     * BEFORE CONTROL2 analog power-up so VREF ramps slowly (anti-pop). */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_MASTERMODE, 0x00), TAG, "Master mode write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL21, 0x80), TAG, "DAC control21 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CHIPPOWER, 0xF3), TAG, "Stop STM/DLL failed");

    /* CONTROL1 0x16: DACMCLK=0, SameFs=1, EnRef=1, VMIDSEL=500kΩ (slow VREF ramp). */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CONTROL1, 0x16), TAG, "Control1 write failed");
    /* CONTROL2 0x50: bit4 (VRefBufHilo) empirically required — UG 0x40 leaves DAC silent here. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CONTROL2, 0x50), TAG, "Analog power up failed");

    /* 500ms VREF settle through 500kΩ (≥4×τ with ~100nF VREF cap). */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* Runtime: VMIDSEL 500kΩ→50kΩ (better PSRR). */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CONTROL1, 0x19), TAG, "Control1 runtime write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CHIPPOWER, 0xF0), TAG, "VREF power up failed");

    /* ANAVOLMANAG=0x7C enables soft-ramp on LOUT/ROUT vol writes. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CHIPLOPOW1, 0x00), TAG, "Low power1 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CHIPLOPOW2, 0x00), TAG, "Low power2 write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ANAVOLMANAG, 0x7C), TAG, "Analog vol write failed");

    /* Amps stay off — turned on in start() per UG 10.1 final ordering. */
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACPOWER, 0x00), TAG, "DAC power init failed");

    if (config->codec_mode & BSP_ES8388_MODE_DECODE) {
        ESP_RETURN_ON_ERROR(es8388_config_dac(interface), TAG, "DAC config failed");
    }
    if (config->codec_mode & BSP_ES8388_MODE_ENCODE) {
        ESP_RETURN_ON_ERROR(es8388_config_adc(interface, config->mic_amp_gain), TAG, "ADC config failed");
    }

    /* Configure ADC input */
    ESP_RETURN_ON_ERROR(bsp_es8388_config_adc_input(interface, config->adc_input), TAG, "ADC input config failed");

    /* Configure DAC output */
    ESP_RETURN_ON_ERROR(bsp_es8388_config_dac_output(interface, config->dac_output), TAG, "DAC output config failed");

    /* The undocumented DLL registers at their reset values. esp_codec_dev
     * writes 0x35=A0, 0x37=D0, 0x39=D0; with 0x35 and 0x37 set this way the
     * ADC came up as white noise (around -38 dBFS RMS, or full scale on one
     * channel) on 12 of 16 starts, whatever its input, gain or power, until
     * the next start -- at reset values on none of 16 (2026-11-03). Written
     * rather than left: a chip an older firmware set keeps them across an
     * ESP-only restart. */
    es8388_write_reg(interface, 0x35, 0x00);
    es8388_write_reg(interface, 0x37, 0x00);
    es8388_write_reg(interface, 0x39, 0x00);

    s_es8388[interface].current_volume = 70;
    s_es8388[interface].pair_volume[0] = s_es8388[interface].pair_volume[1] = 70;
    s_es8388[interface].muted = true;

    ESP_LOGI(TAG, "ES8388 interface %d initialized (addr=0x%02X)", interface, config->i2c_addr);
    return ESP_OK;
}

esp_err_t bsp_es8388_start(bsp_i2s_interface_t interface, bsp_es8388_mode_t mode)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    /* Warm-boot: codec already running pre-reset, skip DACPOWER/CHIPPOWER
     * retoggle that would emit artifacts. */
    if (s_es8388[interface].warm_boot_skip) {
        ESP_LOGI(TAG, "ES8388 [%d] start skipped (warm boot)", interface);
        return ESP_OK;
    }

    if (mode & BSP_ES8388_MODE_ENCODE) {
        /* User Guide Section 11.1: WriteReg(0x03, 0x09) — power up ADC analog + bias */
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCPOWER, 0x09), TAG, "ADC power on failed");
    }
    if (mode & BSP_ES8388_MODE_DECODE) {
        /* User Guide 10.1 startup ordering, anti-pop:
         *   - LOUT vol forced to 0 before DACPOWER on
         *   - 300ms AC-cap settle, then STM/DLL last
         *   - DAC unmute via slow soft-ramp, then ANAVOLMANAG ramps LOUT to 0dB */
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL3, 0x36), TAG, "Pre-start mute failed");
        es8388_write_reg(interface, ES8388_DACCONTROL24, 0x00);
        es8388_write_reg(interface, ES8388_DACCONTROL25, 0x00);
        es8388_write_reg(interface, ES8388_DACCONTROL26, 0x00);
        es8388_write_reg(interface, ES8388_DACCONTROL27, 0x00);

        /* DACPOWER: DAC cores up + the selected LOUT/ROUT on. bits[7:6]=PdnDACL/R
         * MUST stay 0 — 0xFC powers the DACs DOWN, leaking only MCLK/4 to the output. */
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACPOWER, s_es8388[interface].dac_outputs),
                            TAG, "DAC output power failed");
        s_es8388[interface].outputs_on = true;
        vTaskDelay(pdMS_TO_TICKS(300));

        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_CHIPPOWER, 0x00), TAG, "STM/DLL startup failed");
        vTaskDelay(pdMS_TO_TICKS(50));

        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL3, 0x32), TAG, "Post-start unmute failed");
        s_es8388[interface].muted = false;
        vTaskDelay(pdMS_TO_TICKS(100));

        es8388_write_reg(interface, ES8388_DACCONTROL24, 0x1E);
        es8388_write_reg(interface, ES8388_DACCONTROL25, 0x1E);
        es8388_write_reg(interface, ES8388_DACCONTROL26, 0x1E);
        es8388_write_reg(interface, ES8388_DACCONTROL27, 0x1E);
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "ES8388 [%d] started (mode=%d)", interface, mode);
    return ESP_OK;
}

esp_err_t bsp_es8388_stop(bsp_i2s_interface_t interface, bsp_es8388_mode_t mode)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    /* User Guide 10.5 — Power down to standby. Anti-pop wait between mute
     * and STM/DLL stop so DAC soft-ramp (~21ms @ 48k) finishes silently;
     * LOUT/ROUT vol → 0 via ANAVOLMANAG ramp before cutting amp power. */
    if (mode & BSP_ES8388_MODE_ENCODE) {
        es8388_write_reg(interface, ES8388_ADCCONTROL7, 0x34);
    }
    if (mode & BSP_ES8388_MODE_DECODE) {
        es8388_write_reg(interface, ES8388_DACCONTROL3, 0x36);
        s_es8388[interface].muted = true;
    }
    vTaskDelay(pdMS_TO_TICKS(200));

    if (mode & BSP_ES8388_MODE_DECODE) {
        es8388_write_reg(interface, ES8388_DACCONTROL24, 0x00);
        es8388_write_reg(interface, ES8388_DACCONTROL25, 0x00);
        es8388_write_reg(interface, ES8388_DACCONTROL26, 0x00);
        es8388_write_reg(interface, ES8388_DACCONTROL27, 0x00);
        vTaskDelay(pdMS_TO_TICKS(80));
    }

    es8388_write_reg(interface, ES8388_CHIPPOWER, 0xF3);
    if (mode & BSP_ES8388_MODE_ENCODE) {
        es8388_write_reg(interface, ES8388_ADCPOWER, 0xFC);
    }
    if (mode & BSP_ES8388_MODE_DECODE) {
        es8388_write_reg(interface, ES8388_DACPOWER, 0xC0);
        s_es8388[interface].outputs_on = false;
    }

    ESP_LOGI(TAG, "ES8388 [%d] standby", interface);
    return ESP_OK;
}

esp_err_t bsp_es8388_set_mic_gain(bsp_i2s_interface_t interface, uint8_t stage)
{
    if (interface >= BSP_I2S_INTERFACE_MAX) return ESP_ERR_INVALID_ARG;
    if (stage > 8) stage = 8;
    s_mic_gain_set[interface] = stage;
    if (!s_es8388[interface].initialized) return ESP_OK;     /* its init applies it */
    return es8388_write_reg(interface, ES8388_ADCCONTROL1, (uint8_t)((stage << 4) | stage));
}

/* 0-100 % onto the analog volume's 0x00..0x21; see bsp_es8388_set_output_volume(). */
static uint8_t vol_code(int volume)
{
    return (uint8_t)((volume * 0x21 + 50) / 100);
}

esp_err_t bsp_es8388_set_output_volume(bsp_i2s_interface_t interface, bsp_es8388_dac_output_t outputs, int volume)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }
    if (volume < 0 || volume > 100) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Analog attenuation (LOUT/ROUT), with the DAC at full digital scale:
     * digital attenuation throws away bits and leaves the noise floor where it
     * was, while an analog one after the DAC takes the hiss down with the
     * signal. 0-100% maps to the 6-bit range 0x00..0x21 (0x1E = 0 dB; the
     * three codes above it are gain, see bsp_es8388_out_ceiling()).
     *
     * This register is itself ~1.5 dB/LSB (measured on hardware), so a plain
     * proportional mapping already makes the slider linear in dB end to end —
     * that IS the perceptually-even "audio taper" curve, not something to
     * layer a log/exponential shape on top of. Round to the nearest code
     * instead of truncating: floor() biases every code low and, over 34
     * codes spread across 101 slider positions, turns into a measurable
     * unevenness between adjacent volume decades (some 10-point moves span 3
     * codes, others 4 — truncation clusters the extra code onto the same few
     * decades every time instead of distributing the rounding error). */
    uint8_t vol_reg = vol_code(volume);
    if (outputs & BSP_ES8388_DAC_OUTPUT_LINE1) s_es8388[interface].pair_volume[0] = volume;
    if (outputs & BSP_ES8388_DAC_OUTPUT_LINE2) s_es8388[interface].pair_volume[1] = volume;
    const int pair0 = s_es8388[interface].pair_volume[0];
    const int pair1 = s_es8388[interface].pair_volume[1];

    /* 0 % is off. The analog stage stops at its lowest code (0x00, about
     * -45 dB), which the speaker amp still makes plainly audible -- "Out 1 and
     * Out 2 at zero and the speaker still plays" (09.10.2026). So at 0 the
     * DAC's digital volume goes to its floor (0xC0 = -96 dB) as well, and
     * anywhere above it is back at 0 dB, where init left it -- the analog
     * stage alone still carries the level, with the noise floor going down
     * with it. Both ramp with DACCONTROL3's soft ramp: no click. The DAC
     * feeds both output pairs, which have levels of their own (the speakers
     * and Out 1/2 on codec 0), so it goes to its floor only once both are at
     * 0: one pair turned off must not silence the other. The loop pair
     * (pinout BSP_CODEC_LOOP_*) is not one of them: nobody listens to it --
     * it drives the Line 3/4 jack the ADCs read, at the level init gave it,
     * or is off -- so it must not hold the DAC up once Out 3/4, the codec's
     * one heard pair, is at 0 (it did: Out 3/4 muted stayed at 0 dB digital
     * behind LOUT2/ROUT2 at -45 dB, 04.11.2026). */
    const bool loop_codec = (int)interface == BSP_CODEC_LOOP_CODEC;
    const bool pair0_heard = !(loop_codec && (BSP_CODEC_LOOP_DAC_OUTPUT & BSP_ES8388_DAC_OUTPUT_LINE1));
    const bool pair1_heard = !(loop_codec && (BSP_CODEC_LOOP_DAC_OUTPUT & BSP_ES8388_DAC_OUTPUT_LINE2));
    const uint8_t dig_reg = ((!pair0_heard || pair0 == 0) && (!pair1_heard || pair1 == 0)) ? 0xC0 : 0x00;
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL4, dig_reg), TAG, "DAC digital vol L write failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL5, dig_reg), TAG, "DAC digital vol R write failed");

    if (outputs & BSP_ES8388_DAC_OUTPUT_LINE1) {
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL24, vol_reg), TAG, "LOUT1 vol write failed");
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL25, vol_reg), TAG, "ROUT1 vol write failed");
    }
    if (outputs & BSP_ES8388_DAC_OUTPUT_LINE2) {
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL26, vol_reg), TAG, "LOUT2 vol write failed");
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL27, vol_reg), TAG, "ROUT2 vol write failed");
    }

    s_es8388[interface].current_volume = volume;
    ESP_LOGD(TAG, "ES8388 iface %d outputs 0x%02X volume set to %d%% (reg=0x%02X)",
             interface, (unsigned)outputs, volume, vol_reg);
    /* The pair turned up highest is the one that runs out of swing first. */
    if (s_volume_cb) s_volume_cb(interface, pair0 > pair1 ? pair0 : pair1);
    return ESP_OK;
}

esp_err_t bsp_es8388_set_volume(bsp_i2s_interface_t interface, int volume)
{
    return bsp_es8388_set_output_volume(interface, BSP_ES8388_DAC_OUTPUT_ALL, volume);
}

float bsp_es8388_out_ceiling(int volume)
{
    /* Measured on Out 2 into a Fireface UFX II line input, sines at 1 kHz and
     * 100 Hz (09.10.2026): at the top code a sine is clean at -3 dBFS (THD
     * -85 dB) and clipped at -1 dBFS (THD -36 dB, odd harmonics -- the stage
     * after the volume runs out of swing); at 0x1E it is clean at -1 dBFS
     * (THD -80 dB). A code is ~1.5 dB, so the clean peak is taken as
     * -3.5 dBFS at 0x21 and 1.5 dB more for each code below it. */
    const float db = -3.5f + 1.5f * (float)(0x21 - vol_code(volume));
    return db >= 0.0f ? 1.0f : powf(10.0f, db / 20.0f);
}

void bsp_es8388_set_volume_callback(bsp_es8388_volume_cb_t cb)
{
    s_volume_cb = cb;
}

esp_err_t bsp_es8388_get_volume(bsp_i2s_interface_t interface, int *volume)
{
    if (volume == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    *volume = s_es8388[interface].current_volume;
    return ESP_OK;
}

esp_err_t bsp_es8388_set_mute(bsp_i2s_interface_t interface, bool enable)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    /* User Guide: DACCONTROL3 with soft ramp enabled
     * 0x36 = RampRate=00 (slow), SoftRamp=1, DACMute=1 (muted)
     * 0x32 = RampRate=00 (slow), SoftRamp=1, DACMute=0 (unmuted)
     * Slow ramp avoids click; matches User Guide section 10.5 / 10.6. */
    uint8_t reg_val = enable ? 0x36 : 0x32;
    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACCONTROL3, reg_val), TAG, "Mute write failed");

    s_es8388[interface].muted = enable;
    ESP_LOGD(TAG, "ES8388 iface %d mute=%d", interface, enable);
    return ESP_OK;
}



esp_err_t bsp_es8388_config_adc_input(bsp_i2s_interface_t interface, bsp_es8388_adc_input_t input)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_ADCCONTROL2, (uint8_t)input), TAG, "ADC input config failed");
    return ESP_OK;
}

esp_err_t bsp_es8388_config_dac_output(bsp_i2s_interface_t interface, bsp_es8388_dac_output_t output)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    /* The output drivers, not the output mixer (0x27/0x2A stay on the DAC).
     * A driver powered down still holds its pins on VREF through VROI's
     * resistor: bsp_es8388_set_off_outputs_hiz(). */
    const uint8_t outputs = (uint8_t)output & 0x3C;
    if (s_es8388[interface].outputs_on) {
        ESP_RETURN_ON_ERROR(es8388_write_reg(interface, ES8388_DACPOWER, outputs), TAG, "DAC output power failed");
    }
    s_es8388[interface].dac_outputs = outputs;
    return ESP_OK;
}

esp_err_t bsp_es8388_set_off_outputs_hiz(bsp_i2s_interface_t interface, bool hiz)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }
    /* DACCONTROL23 bit 4 VROI: 0 = 1.5 kOhm, 1 = 40 kOhm from VREF to an
     * output that is off. */
    return es8388_write_reg(interface, ES8388_DACCONTROL23, hiz ? 0x10 : 0x00);
}

esp_err_t bsp_es8388_read_register(bsp_i2s_interface_t interface, uint8_t reg, uint8_t *value)
{
    if (value == NULL) return ESP_ERR_INVALID_ARG;
    return es8388_read_reg(interface, reg, value);
}

esp_err_t bsp_es8388_write_register(bsp_i2s_interface_t interface, uint8_t reg, uint8_t value)
{
    return es8388_write_reg(interface, reg, value);
}

esp_codec_dev_handle_t bsp_es8388_get_codec_dev(bsp_i2s_interface_t interface)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return NULL;
    }
    return s_es8388[interface].codec_dev;
}

esp_err_t bsp_es8388_deinit_interface(bsp_i2s_interface_t interface)
{
    if (interface >= BSP_I2S_INTERFACE_MAX || !s_es8388[interface].initialized) {
        return ESP_FAIL;
    }

    /* Graceful power-down following User Guide Section 11.5 */

    /* 1. Mute with soft ramp */
    es8388_write_reg(interface, ES8388_DACCONTROL3, 0xE6);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* 2. Disable outputs + DAC */
    es8388_write_reg(interface, ES8388_DACPOWER, 0x00);

    /* 3. Power down ADC */
    es8388_write_reg(interface, ES8388_ADCPOWER, 0xFF);

    /* 4. Stop STM/DLL */
    es8388_write_reg(interface, ES8388_CHIPPOWER, 0xC0);

    /* 5. DAC control 21 standby */
    es8388_write_reg(interface, ES8388_DACCONTROL21, 0x9C);

    /* 6. Power down analog (CONTROL2) */
    es8388_write_reg(interface, ES8388_CONTROL2, 0x58);

    /* 7. Full chip power down */
    es8388_write_reg(interface, ES8388_CHIPPOWER, 0xFF);

    /* Remove I2C device */
    if (s_es8388[interface].i2c_dev) {
        i2c_master_bus_rm_device(s_es8388[interface].i2c_dev);
        s_es8388[interface].i2c_dev = NULL;
    }

    s_es8388[interface].initialized = false;
    s_es8388[interface].outputs_on = false;
    s_es8388[interface].codec_dev = NULL;

    ESP_LOGI(TAG, "ES8388 interface %d deinitialized", interface);
    return ESP_OK;
}

esp_err_t bsp_es8388_deinit(void)
{
    for (int i = 0; i < BSP_I2S_INTERFACE_MAX; i++) {
        if (s_es8388[i].initialized) {
            bsp_es8388_deinit_interface((bsp_i2s_interface_t)i);
        }
    }
    return ESP_OK;
}
