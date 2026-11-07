/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Example: Using ES8388 codec with dual I2S interfaces
 * 
 * This example demonstrates how to initialize and use ES8388 audio codec
 * with the BSP I2S driver for audio input/output.
 */

#include "crosspad_s3.h"
#include "i2s_driver.h"
#include "bsp_es8388.h"
#include "esp_log.h"
#include "xtensa/hal.h"
#include "xtensa_perfmon_access.h"
#include "xtensa_perfmon_apis.h"

static const char *TAG = "es8388_example";

/**
 * @brief Initialize audio system with ES8388 codec
 */
esp_err_t audio_init_example(void)
{
    /* Initialize I2C first (required for ES8388 communication) */
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed");
        return ret;
    }
    
    /* Get I2C handle */
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    
    /* Initialize I2S with ES8388 codec
     * - Uses default I2S config (48kHz, 32-bit, stereo)
     * - Single codec mode (dual_codec = false)
     */
    ret = bsp_i2s_es8388_init(i2c_handle, NULL, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S + ES8388 initialization failed");
        return ret;
    }
    
    /* Set initial volume (0-100) for codec on I2S0 */
    bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, 70);
    
    /* Unmute output on codec 0 */
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false);
    
    ESP_LOGI(TAG, "Audio system initialized successfully");
    return ESP_OK;
}

/**
 * @brief Example: Record audio from microphone
 */
esp_err_t audio_record_example(void)
{
    /* Get I2S RX handle for receiving audio data */
    i2s_chan_handle_t rx_handle = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_0);
    if (rx_handle == NULL) {
        ESP_LOGE(TAG, "I2S RX handle not available");
        return ESP_FAIL;
    }
    
    /* Buffer for audio samples */
    int32_t audio_buffer[480]; // 240 frames * 2 channels
    size_t bytes_read = 0;
    
    /* Read audio data from I2S (blocking call) */
    esp_err_t ret = i2s_channel_read(rx_handle, audio_buffer, sizeof(audio_buffer), 
                                     &bytes_read, pdMS_TO_TICKS(1000));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Read %d bytes of audio data", bytes_read);
        
        /* Process audio data here... */
        /* audio_buffer contains stereo 32-bit samples */
    }
    
    return ret;
}

/**
 * @brief Example: Playback audio through speaker
 */
esp_err_t audio_playback_example(void)
{
    /* Get I2S TX handle for sending audio data */
    i2s_chan_handle_t tx_handle = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_0);
    if (tx_handle == NULL) {
        ESP_LOGE(TAG, "I2S TX handle not available");
        return ESP_FAIL;
    }
    
    /* Generate or load audio samples */
    int32_t audio_buffer[480]; // 240 frames * 2 channels
    
    /* Fill buffer with audio samples... */
    /* For test: Generate a simple sine wave */
    for (int i = 0; i < 480; i += 2) {
        int32_t sample = 0; // Add your audio data here
        audio_buffer[i] = sample;     // Left channel
        audio_buffer[i + 1] = sample; // Right channel
    }
    
    size_t bytes_written = 0;
    
    /* Write audio data to I2S (blocking call) */
    esp_err_t ret = i2s_channel_write(tx_handle, audio_buffer, sizeof(audio_buffer),
                                      &bytes_written, pdMS_TO_TICKS(1000));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Wrote %d bytes of audio data", bytes_written);
    }
    
    return ret;
}

/**
 * @brief Example: Audio loopback (mic -> speaker)
 */
esp_err_t audio_loopback_example(void)
{
    i2s_chan_handle_t rx_handle = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_0);
    i2s_chan_handle_t tx_handle = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_0);

    if (rx_handle == NULL || tx_handle == NULL) {
        ESP_LOGE(TAG, "I2S handles not available");
        return ESP_FAIL;
    }

    int32_t audio_buffer[480];
    size_t bytes_read = 0, bytes_written = 0;

    /* Loopback for 10 seconds */
    for (int i = 0; i < 100; i++) {
        /* Read from microphone */
        esp_err_t ret = i2s_channel_read(rx_handle, audio_buffer, sizeof(audio_buffer),
                                         &bytes_read, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            continue;
        }

        /* Write to speaker */
        i2s_channel_write(tx_handle, audio_buffer, bytes_read,
                         &bytes_written, pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Loopback test completed");
    return ESP_OK;
}

/**
 * @brief Example: Audio loopback with performance monitoring
 *
 * This example demonstrates how to use Xtensa Performance Monitor (PERFMON)
 * to measure CPU cycles, instruction counts, and cache statistics during
 * I2S audio processing loops.
 */
esp_err_t audio_loopback_perfmon_example(void)
{
    i2s_chan_handle_t rx_handle = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_0);
    i2s_chan_handle_t tx_handle = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_0);

    if (rx_handle == NULL || tx_handle == NULL) {
        ESP_LOGE(TAG, "I2S handles not available");
        return ESP_FAIL;
    }

    /* Initialize performance monitor */
    xtensa_perfmon_init(0,               /* CPU 0 */
                        0,               /* tracelevel (not used) */
                        XTPERF_CNT_CYCLES,   /* Counter 0: CPU cycles */
                        XTPERF_CNT_INSN,     /* Counter 1: Instructions executed */
                        XTPERF_CNT_D_LOAD_U1,/* Counter 2: D-cache load misses */
                        XTPERF_CNT_D_STORE_U1,/* Counter 3: D-cache store misses */
                        XTPERF_CNT_I_MISS,   /* Counter 4: I-cache misses */
                        XTPERF_CNT_I_HIT);   /* Counter 5: I-cache hits */

    ESP_LOGI(TAG, "Performance monitoring enabled for I2S loopback");
    ESP_LOGI(TAG, "Counters: [0]=Cycles [1]=Instructions [2]=D-Load-Miss [3]=D-Store-Miss [4]=I-Miss [5]=I-Hit");

    int32_t audio_buffer[480];
    size_t bytes_read = 0, bytes_written = 0;

    /* Statistics accumulators */
    uint64_t total_cycles = 0;
    uint64_t total_insn = 0;
    uint64_t total_d_load_miss = 0;
    uint64_t total_d_store_miss = 0;
    uint64_t total_i_miss = 0;
    uint64_t total_i_hit = 0;
    int successful_iterations = 0;

    /* Loopback with perfmon for 10 seconds (100 iterations @ ~100ms each) */
    for (int i = 0; i < 100; i++) {
        /* Reset and start performance counters */
        xtensa_perfmon_reset(0);
        xtensa_perfmon_start();

        /* === MEASURED SECTION START === */

        /* Read from microphone */
        esp_err_t ret = i2s_channel_read(rx_handle, audio_buffer, sizeof(audio_buffer),
                                         &bytes_read, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            xtensa_perfmon_stop();
            continue;
        }

        /* Optional: Process audio data here */
        /* Example: Simple volume scaling */
        for (int j = 0; j < 480; j++) {
            audio_buffer[j] = (audio_buffer[j] * 8) / 10;  // 80% volume
        }

        /* Write to speaker */
        i2s_channel_write(tx_handle, audio_buffer, bytes_read,
                         &bytes_written, pdMS_TO_TICKS(100));

        /* === MEASURED SECTION END === */

        /* Stop counters and read values */
        xtensa_perfmon_stop();

        uint32_t cycles = xtensa_perfmon_value(0);
        uint32_t insn = xtensa_perfmon_value(1);
        uint32_t d_load_miss = xtensa_perfmon_value(2);
        uint32_t d_store_miss = xtensa_perfmon_value(3);
        uint32_t i_miss = xtensa_perfmon_value(4);
        uint32_t i_hit = xtensa_perfmon_value(5);

        /* Accumulate statistics */
        total_cycles += cycles;
        total_insn += insn;
        total_d_load_miss += d_load_miss;
        total_d_store_miss += d_store_miss;
        total_i_miss += i_miss;
        total_i_hit += i_hit;
        successful_iterations++;

        /* Log every 10th iteration */
        if (i % 10 == 0) {
            float cpi = (float)cycles / (float)insn;  // Cycles per instruction
            float i_hit_rate = 100.0f * (float)i_hit / (float)(i_hit + i_miss);

            ESP_LOGI(TAG, "[%3d] Cycles=%u Insn=%u CPI=%.2f D-L-Miss=%u D-S-Miss=%u I-Hit=%.1f%%",
                     i, cycles, insn, cpi, d_load_miss, d_store_miss, i_hit_rate);
        }
    }

    /* Calculate and display final statistics */
    if (successful_iterations > 0) {
        uint32_t avg_cycles = total_cycles / successful_iterations;
        uint32_t avg_insn = total_insn / successful_iterations;
        uint32_t avg_d_load_miss = total_d_load_miss / successful_iterations;
        uint32_t avg_d_store_miss = total_d_store_miss / successful_iterations;
        uint32_t avg_i_miss = total_i_miss / successful_iterations;
        uint32_t avg_i_hit = total_i_hit / successful_iterations;

        float avg_cpi = (float)total_cycles / (float)total_insn;
        float avg_i_hit_rate = 100.0f * (float)total_i_hit / (float)(total_i_hit + total_i_miss);

        ESP_LOGI(TAG, "=== Performance Statistics (avg over %d iterations) ===", successful_iterations);
        ESP_LOGI(TAG, "  CPU Cycles:        %u", avg_cycles);
        ESP_LOGI(TAG, "  Instructions:      %u", avg_insn);
        ESP_LOGI(TAG, "  CPI:               %.3f", avg_cpi);
        ESP_LOGI(TAG, "  D-Cache Load Miss: %u", avg_d_load_miss);
        ESP_LOGI(TAG, "  D-Cache Store Miss:%u", avg_d_store_miss);
        ESP_LOGI(TAG, "  I-Cache Misses:    %u", avg_i_miss);
        ESP_LOGI(TAG, "  I-Cache Hits:      %u", avg_i_hit);
        ESP_LOGI(TAG, "  I-Cache Hit Rate:  %.2f%%", avg_i_hit_rate);
    }

    /* Deinitialize performance monitor */
    xtensa_perfmon_dump();

    ESP_LOGI(TAG, "Loopback with perfmon test completed");
    return ESP_OK;
}

/**
 * @brief Example: Volume control
 */
void audio_volume_control_example(void)
{
    /* Gradually increase volume from 0 to 100 on codec 0 */
    for (int vol = 0; vol <= 100; vol += 10) {
        bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, vol);
        ESP_LOGI(TAG, "Volume set to %d%%", vol);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    /* Mute codec 0 */
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, true);
    ESP_LOGI(TAG, "Audio muted");
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* Unmute codec 0 */
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false);
    ESP_LOGI(TAG, "Audio unmuted");
    
    /* Get current volume */
    int current_vol;
    bsp_es8388_get_volume(BSP_I2S_INTERFACE_0, &current_vol);
    ESP_LOGI(TAG, "Current volume: %d%%", current_vol);
}

/**
 * @brief Example: Custom I2S configuration
 */
esp_err_t audio_custom_config_example(void)
{
    /* Initialize I2C */
    bsp_i2c_init();
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    
    /* Custom I2S configuration: 44.1kHz, 16-bit, stereo */
    i2s_std_config_t i2s_config = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din = BSP_I2S_DSIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    
    i2s_config.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    
    /* Initialize with custom configuration */
    esp_err_t ret = bsp_i2s_es8388_init(i2c_handle, &i2s_config, false);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Custom I2S initialization failed");
        return ret;
    }
    
    ESP_LOGI(TAG, "Audio initialized with custom config (44.1kHz, 16-bit)");
    return ESP_OK;
}

/**
 * @brief Example: Using with ESP-ADF codec device
 */
esp_err_t audio_esp_adf_integration_example(void)
{
    /* Initialize audio system */
    bsp_i2c_init();
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    bsp_i2s_es8388_init(i2c_handle, NULL, false);
    
    /* Get ESP-ADF codec data interface */
    const audio_codec_data_if_t *codec_if = bsp_i2s_get_codec_itf(BSP_I2S_INTERFACE_0);
    if (codec_if == NULL) {
        ESP_LOGE(TAG, "Codec interface not available");
        return ESP_FAIL;
    }
    
    /* You can now use codec_if with other ESP-ADF components */
    /* For example, with audio pipelines, audio elements, etc. */
    
    ESP_LOGI(TAG, "Codec interface ready for ESP-ADF integration");
    return ESP_OK;
}

/**
 * @brief Example: Dual ES8388 codec operation
 * 
 * This example demonstrates independent control of two ES8388 codecs:
 * - Codec 0 on I2S0 (Master mode)
 * - Codec 1 on I2S1 (Slave mode, synchronized clocks)
 */
esp_err_t audio_dual_codec_example(void)
{
    ESP_LOGI(TAG, "=== Dual Codec Example ===");
    
    /* Initialize I2C */
    esp_err_t ret = bsp_i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C initialization failed");
        return ret;
    }
    
    /* Get I2C handle */
    i2c_master_bus_handle_t i2c_handle = bsp_i2c_get_handle();
    
    /* Initialize I2S with DUAL ES8388 codecs */
    ESP_LOGI(TAG, "Initializing dual codec system...");
    ret = bsp_i2s_es8388_init(i2c_handle, NULL, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Dual codec initialization failed");
        return ret;
    }
    
    /* Configure independent volumes for each codec */
    ESP_LOGI(TAG, "Setting independent volumes...");
    bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, 80);  // Codec 0: 80%
    bsp_es8388_set_volume(BSP_I2S_INTERFACE_1, 50);  // Codec 1: 50%
    
    /* Unmute both codecs */
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false);
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_1, false);
    
    /* Get handles for both interfaces */
    i2s_chan_handle_t i2s0_rx = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_0);
    i2s_chan_handle_t i2s0_tx = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_0);
    i2s_chan_handle_t i2s1_rx = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_1);
    i2s_chan_handle_t i2s1_tx = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_1);
    
    /* Buffers for both codecs */
    int32_t codec0_buffer[480];
    int32_t codec1_buffer[480];
    size_t bytes_read0, bytes_read1;
    
    /* Example: Record from both codecs simultaneously for 5 seconds */
    ESP_LOGI(TAG, "Recording from both codecs simultaneously...");
    for (int i = 0; i < 50; i++) {
        /* Read from codec 0 */
        esp_err_t ret0 = i2s_channel_read(i2s0_rx, codec0_buffer, sizeof(codec0_buffer),
                                          &bytes_read0, pdMS_TO_TICKS(100));
        
        /* Read from codec 1 */
        esp_err_t ret1 = i2s_channel_read(i2s1_rx, codec1_buffer, sizeof(codec1_buffer),
                                          &bytes_read1, pdMS_TO_TICKS(100));
        
        if (ret0 == ESP_OK && ret1 == ESP_OK) {
            /* Process audio from both codecs here */
            /* codec0_buffer and codec1_buffer contain synchronized audio */
            if (i % 10 == 0) {
                ESP_LOGI(TAG, "Codec0: %d bytes, Codec1: %d bytes", bytes_read0, bytes_read1);
            }
        }
    }
    
    /* Example: Demonstrate independent mute control */
    ESP_LOGI(TAG, "Testing independent mute control...");
    
    ESP_LOGI(TAG, "Muting codec 0, codec 1 still active");
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, true);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "Muting codec 1, unmuting codec 0");
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false);
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_1, true);
    vTaskDelay(pdMS_TO_TICKS(2000));
    
    ESP_LOGI(TAG, "Unmuting both codecs");
    bsp_es8388_set_mute(BSP_I2S_INTERFACE_1, false);
    vTaskDelay(pdMS_TO_TICKS(1000));
    
    /* Example: Independent volume ramping */
    ESP_LOGI(TAG, "Testing independent volume control...");
    for (int vol = 0; vol <= 100; vol += 20) {
        bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, vol);          // Codec 0 increasing
        bsp_es8388_set_volume(BSP_I2S_INTERFACE_1, 100 - vol);    // Codec 1 decreasing
        ESP_LOGI(TAG, "Codec0 vol: %d%%, Codec1 vol: %d%%", vol, 100 - vol);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    /* Read current volumes */
    int vol0, vol1;
    bsp_es8388_get_volume(BSP_I2S_INTERFACE_0, &vol0);
    bsp_es8388_get_volume(BSP_I2S_INTERFACE_1, &vol1);
    ESP_LOGI(TAG, "Final volumes - Codec0: %d%%, Codec1: %d%%", vol0, vol1);
    
    ESP_LOGI(TAG, "Dual codec example completed successfully");
    return ESP_OK;
}

/**
 * @brief Main example entry point
 *
 * Set DUAL_CODEC_MODE to 1 to run dual codec example
 * Set PERFMON_MODE to 1 to run performance monitoring example
 * Otherwise runs standard single codec examples
 */
#define DUAL_CODEC_MODE 0
#define PERFMON_MODE 1

void app_main(void)
{
    ESP_LOGI(TAG, "ES8388 + I2S Example Starting...");

#if DUAL_CODEC_MODE
    /* Run dual codec example */
    if (audio_dual_codec_example() != ESP_OK) {
        ESP_LOGE(TAG, "Dual codec example failed");
        return;
    }
#elif PERFMON_MODE
    /* Initialize audio system (single codec) */
    if (audio_init_example() != ESP_OK) {
        ESP_LOGE(TAG, "Audio initialization failed");
        return;
    }

    /* Run performance monitoring example */
    ESP_LOGI(TAG, "Running audio loopback with performance monitoring...");
    audio_loopback_perfmon_example();
#else
    /* Initialize audio system (single codec) */
    if (audio_init_example() != ESP_OK) {
        ESP_LOGE(TAG, "Audio initialization failed");
        return;
    }

    /* Run single codec examples */
    ESP_LOGI(TAG, "Running volume control example...");
    audio_volume_control_example();

    ESP_LOGI(TAG, "Running audio loopback example...");
    audio_loopback_example();
#endif

    ESP_LOGI(TAG, "Example completed successfully");

    /* Cleanup */
    bsp_i2s_es8388_deinit();
}
