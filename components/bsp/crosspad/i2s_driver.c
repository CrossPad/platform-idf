#include "i2s_driver.h"
#include "bsp_es8388.h"
#include "crosspad_s3.h"
#include "esp_log.h"
#include "esp_check.h"
#include "bsp_err_check.h"
#include "esp_codec_dev_defaults.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_attr.h"
#include "hal/i2s_ll.h"

static const char *TAG = "bsp_i2s_dual";

/* I2S channel handles for dual microphone setup */
static i2s_chan_handle_t i2s0_tx_chan = NULL;
static i2s_chan_handle_t i2s0_rx_chan = NULL;
static i2s_chan_handle_t i2s1_tx_chan = NULL;
static i2s_chan_handle_t i2s1_rx_chan = NULL;

/* ESP-ADF codec data interfaces */
static const audio_codec_data_if_t *i2s0_data_if = NULL;
static const audio_codec_data_if_t *i2s1_data_if = NULL;

/* Written by the TX DMA interrupt only (bsp_i2s_tx_clock_t). The two
 * callbacks are IRAM, like the GDMA handler that calls them: left in flash
 * (fetched through the PSRAM cache on this image), they cost the USB-audio
 * profile 6-8 speaker-ring underruns and audible noise on the captured loop
 * per 4-minute latency run; in IRAM, none. */
typedef struct {
    volatile uint32_t sent;
    volatile uint32_t last_us;
    volatile uint32_t unfilled;
    SemaphoreHandle_t volatile waiter;
} tx_clock_state_t;
static tx_clock_state_t s_tx_clock[BSP_I2S_INTERFACE_MAX];

static bool IRAM_ATTR tx_sent_isr(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle; (void)event;
    tx_clock_state_t *c = (tx_clock_state_t *)ctx;
    /* Time first: a reader takes `sent`, then the time, then `sent` again. */
    c->last_us = (uint32_t)esp_timer_get_time();
    c->sent = c->sent + 1;
    BaseType_t woken = pdFALSE;
    SemaphoreHandle_t w = c->waiter;
    if (w) xSemaphoreGiveFromISR(w, &woken);
    return woken == pdTRUE;
}

/* The send queue was full of refillable buffers when this one finished: the
 * buffer the DMA starts now is one of them, never written since its last turn. */
static bool IRAM_ATTR tx_unfilled_isr(i2s_chan_handle_t handle, i2s_event_data_t *event, void *ctx)
{
    (void)handle; (void)event;
    tx_clock_state_t *c = (tx_clock_state_t *)ctx;
    c->unfilled = c->unfilled + 1;
    return false;
}

/* Before the channel is enabled: the driver refuses callbacks on a running one. */
static esp_err_t tx_clock_attach(i2s_chan_handle_t tx, bsp_i2s_interface_t iface)
{
    const i2s_event_callbacks_t cbs = {
        .on_sent = tx_sent_isr,
        .on_send_q_ovf = tx_unfilled_isr,
    };
    return i2s_channel_register_event_callback(tx, &cbs, &s_tx_clock[iface]);
}

void bsp_i2s_tx_set_waiter(bsp_i2s_interface_t interface, void *sem)
{
    if (interface < BSP_I2S_INTERFACE_MAX) s_tx_clock[interface].waiter = (SemaphoreHandle_t)sem;
}

void bsp_i2s_tx_clock(bsp_i2s_interface_t interface, bsp_i2s_tx_clock_t *out)
{
    if (!out) return;
    if (interface >= BSP_I2S_INTERFACE_MAX) {
        *out = (bsp_i2s_tx_clock_t){0};
        return;
    }
    const tx_clock_state_t *c = &s_tx_clock[interface];
    uint32_t sent;
    do {
        sent = c->sent;
        out->last_us = c->last_us;
    } while (sent != c->sent);
    out->sent = sent;
    out->unfilled = c->unfilled;
}

/* Default I2S GPIO configuration for I2S0 (Master) */
#define BSP_I2S0_GPIO_CFG       \
    {                           \
        .mclk = BSP_I2S_MCLK,   \
        .bclk = BSP_I2S_SCLK,   \
        .ws = BSP_I2S_LCLK,     \
        .dout = BSP_I2S_DOUT,   \
        .din = BSP_I2S_DSIN,    \
        .invert_flags = {       \
            .mclk_inv = false,  \
            .bclk_inv = false,  \
            .ws_inv = false,    \
        },                      \
    }

/* Default I2S GPIO configuration for I2S1 (Slave) */
#define BSP_I2S1_GPIO_CFG       \
    {                           \
        .mclk = I2S_GPIO_UNUSED, \
        .bclk = BSP_I2S_SCLK,   \
        .ws = BSP_I2S_LCLK,     \
        .dout = BSP_I2S_DOUT2,  \
        .din = BSP_I2S_DSIN2,   \
        .invert_flags = {       \
            .mclk_inv = false,  \
            .bclk_inv = false,  \
            .ws_inv = false,    \
        },                      \
    }

/* Sample rate both interfaces are clocked at. Set before bsp_i2s_es8388_init()
 * (the platform passes the engine rate from settings); the codecs are I2S
 * slaves on a x256 MCLK, so this is the only place the rate lives. */
static uint32_t s_sample_rate = BSP_I2S_DEFAULT_SAMPLE_RATE;

void bsp_i2s_set_sample_rate(uint32_t hz)
{
    s_sample_rate = hz;
}

uint32_t bsp_i2s_get_sample_rate(void)
{
    return s_sample_rate;
}

/* Default I2S configuration: Stereo, 16-bit, s_sample_rate */
#define BSP_I2S_DUAL_MIC_CFG(_sample_rate)                                                           \
    {                                                                                                \
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(_sample_rate),                                         \
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO), \
        .gpio_cfg = BSP_I2S0_GPIO_CFG,                                                               \
    }

/**
 * @brief Initialize I2S0 (Master) for primary interface
 */
static esp_err_t bsp_i2s_init_interface_0(const i2s_std_config_t *i2s_config)
{
    if (i2s0_tx_chan && i2s0_rx_chan) {
        ESP_LOGW(TAG, "I2S0 already initialized");
        return ESP_OK;
    }

    /* Setup I2S0 channel - Master mode.
     * DMA: 6 desc × 64 frames = 384 frames = ~8.7 ms of queue at 44.1k.
     *
     * Four descriptors (5.8 ms) were not enough in the USB-audio profile.
     * Measured on hardware, same kit and the same 8 hit/s pad storm: the
     * sample engine's own render -- SampleStreamPly_Process() alone, timed in
     * MarcelSampleNode -- peaks at 2.1 ms in the CDC profile and 7.7 ms in the
     * USB-audio one. There is no blocking call inside it; what changes is that
     * the UAC task stacks and rings live in PSRAM and SPIRAM_FETCH_INSTRUCTIONS
     * puts the engine's own code on that same bus, so USB traffic slows every
     * access it makes. A 7.7 ms spike against 5.8 ms of queue is an underrun by
     * arithmetic, and that is what took the sequencer's pulse with it
     * (MetronomeNode counts the beat in frames rendered).
     *
     * Six covers the measured spike with ~1 ms to spare. Eight was the
     * comfortable number and was not taken: each descriptor is 256 B of
     * *internal* RAM on each of four channels, so 4->8 is 4 kB of the 9.1 kB
     * that was free at the worst moment (MEM int_min), and fx_worker's 5 kB
     * internal stack has to keep fitting beside it. The price of six is ~2 kB
     * and ~2.9 ms of added output latency, the queue being full in steady
     * state. Raising it further needs the engine's render to get cheaper
     * first, not a deeper queue. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;  // Auto clear legacy data in DMA buffer
    chan_cfg.dma_desc_num = BSP_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BSP_I2S_DMA_FRAME_NUM;
    
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &i2s0_tx_chan, &i2s0_rx_chan), TAG, 
                        "Failed to create I2S0 channel");
    ESP_RETURN_ON_ERROR(tx_clock_attach(i2s0_tx_chan, BSP_I2S_INTERFACE_0), TAG,
                        "I2S0 TX callbacks failed");
    
    /* Apply I2S configuration */
    i2s_std_config_t default_cfg;
    const i2s_std_config_t *p_i2s_cfg = i2s_config;
    if (p_i2s_cfg == NULL) {
        /* Use default configuration if not provided */
        default_cfg = (i2s_std_config_t)BSP_I2S_DUAL_MIC_CFG(s_sample_rate);
        default_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
        p_i2s_cfg = &default_cfg;
    }
    
    /* RX first. In full-duplex mode the driver makes the second channel the
     * slave, clocked from its own fractional divider; with RX in that role the
     * ADC stream at 44.1 kHz carried ~165 held frames/s (one frame repeating
     * its predecessor, L and R together) while 48 kHz was clean -- measured
     * over the board's own USB capture. RX as the master and TX as the slave
     * gives 0 held frames at both rates. */
    if (i2s0_rx_chan != NULL) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s0_rx_chan, p_i2s_cfg), TAG, 
                            "I2S0 RX init failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s0_rx_chan), TAG, 
                            "I2S0 RX enable failed");
    }
    if (i2s0_tx_chan != NULL) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s0_tx_chan, p_i2s_cfg), TAG, 
                            "I2S0 TX init failed");
        /* The codecs' MCLK from the RX master's divider, the one their BCLK
         * and LRCK come from. Setting the TX clock (i2s_hal_set_tx_clock())
         * binds MCLK to the TX divider, slave or not: two dividers of the same
         * 76/441 fraction, in a phase decided by when each started, so on
         * about half the boots the codecs' MCLK count per LRCK frame wandered
         * over 441 frames -- a 200 Hz smear on both outputs, THD+N -38 dB at
         * 1 kHz and -28 dB at 3 kHz where a clean boot measures -66 dB. */
        i2s_ll_mclk_bind_to_rx_clk(&I2S0);
        ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s0_tx_chan), TAG, 
                            "I2S0 TX enable failed");
    }
    
    /* Create ESP-ADF codec data interface */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = i2s0_rx_chan,
        .tx_handle = i2s0_tx_chan,
    };
    i2s0_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK(i2s0_data_if, ESP_FAIL);
    
    ESP_LOGI(TAG, "I2S0 initialized (Master mode)");
    return ESP_OK;
}

/**
 * @brief Initialize I2S1 (Slave) for secondary interface
 */
static esp_err_t bsp_i2s_init_interface_1(const i2s_std_config_t *i2s_config)
{
    if (i2s1_tx_chan && i2s1_rx_chan) {
        ESP_LOGW(TAG, "I2S1 already initialized");
        return ESP_OK;
    }

    /* Setup I2S1 channel - Slave mode (matches I2S0 buffer sizing) */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_SLAVE);
    chan_cfg.auto_clear = true;  // Auto clear legacy data in DMA buffer
    chan_cfg.dma_desc_num = BSP_I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = BSP_I2S_DMA_FRAME_NUM;

    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &i2s1_tx_chan, &i2s1_rx_chan), TAG,
                        "Failed to create I2S1 channel");
    ESP_RETURN_ON_ERROR(tx_clock_attach(i2s1_tx_chan, BSP_I2S_INTERFACE_1), TAG,
                        "I2S1 TX callbacks failed");

    /* Configure I2S1 with different GPIO pins (slave shares clock from I2S0) */
    i2s_std_config_t std_cfg;
    if (i2s_config != NULL) {
        std_cfg = *i2s_config;
    } else {
        std_cfg = (i2s_std_config_t)BSP_I2S_DUAL_MIC_CFG(s_sample_rate);
        std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    }

    /* Override GPIO config for I2S1 (different data pins, shared clock) */
    std_cfg.gpio_cfg = (i2s_std_gpio_config_t)BSP_I2S1_GPIO_CFG;

    /* Initialize and enable TX channel */
    if (i2s1_tx_chan != NULL) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s1_tx_chan, &std_cfg), TAG,
                            "I2S1 TX init failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s1_tx_chan), TAG,
                            "I2S1 TX enable failed");
    }

    /* Initialize and enable RX channel */
    if (i2s1_rx_chan != NULL) {
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(i2s1_rx_chan, &std_cfg), TAG,
                            "I2S1 RX init failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(i2s1_rx_chan), TAG,
                            "I2S1 RX enable failed");
    }

    /* Create ESP-ADF codec data interface */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_1,
        .rx_handle = i2s1_rx_chan,
        .tx_handle = i2s1_tx_chan,
    };
    i2s1_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    BSP_NULL_CHECK(i2s1_data_if, ESP_FAIL);

    ESP_LOGI(TAG, "I2S1 initialized (Slave mode)");
    return ESP_OK;
}

esp_err_t bsp_i2s_init_dual(const i2s_std_config_t *i2s_config)
{
    ESP_LOGI(TAG, "Initializing dual I2S codec interfaces...");
    
    /* Initialize I2S0 (Master) first - it provides clock signals */
    esp_err_t ret = bsp_i2s_init_interface_0(i2s_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S0 (Master)");
        return ret;
    }
    
    /* Initialize I2S1 (Slave) - shares clock from I2S0 */
    ret = bsp_i2s_init_interface_1(i2s_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize I2S1 (Slave)");
        /* Clean up I2S0 on failure */
        if (i2s0_rx_chan) i2s_del_channel(i2s0_rx_chan);
        if (i2s0_tx_chan) i2s_del_channel(i2s0_tx_chan);
        return ret;
    }
    
    ESP_LOGI(TAG, "Dual I2S codec interfaces initialized successfully");
    return ESP_OK;
}

esp_err_t bsp_i2s_deinit_all(void)
{
    ESP_LOGI(TAG, "Deinitializing all I2S interfaces...");
    
    /* Disable and delete I2S0 channels */
    if (i2s0_rx_chan) {
        i2s_channel_disable(i2s0_rx_chan);
        i2s_del_channel(i2s0_rx_chan);
        i2s0_rx_chan = NULL;
    }
    if (i2s0_tx_chan) {
        i2s_channel_disable(i2s0_tx_chan);
        i2s_del_channel(i2s0_tx_chan);
        i2s0_tx_chan = NULL;
    }
    i2s0_data_if = NULL;
    
    /* Disable and delete I2S1 channels */
    if (i2s1_rx_chan) {
        i2s_channel_disable(i2s1_rx_chan);
        i2s_del_channel(i2s1_rx_chan);
        i2s1_rx_chan = NULL;
    }
    if (i2s1_tx_chan) {
        i2s_channel_disable(i2s1_tx_chan);
        i2s_del_channel(i2s1_tx_chan);
        i2s1_tx_chan = NULL;
    }
    i2s1_data_if = NULL;
    
    ESP_LOGI(TAG, "All I2S interfaces deinitialized");
    return ESP_OK;
}

i2s_chan_handle_t bsp_i2s_get_tx_handle(bsp_i2s_interface_t interface)
{
    switch (interface) {
        case BSP_I2S_INTERFACE_0:
            return i2s0_tx_chan;
        case BSP_I2S_INTERFACE_1:
            return i2s1_tx_chan;
        default:
            ESP_LOGE(TAG, "Invalid I2S interface: %d", interface);
            return NULL;
    }
}

i2s_chan_handle_t bsp_i2s_get_rx_handle(bsp_i2s_interface_t interface)
{
    switch (interface) {
        case BSP_I2S_INTERFACE_0:
            return i2s0_rx_chan;
        case BSP_I2S_INTERFACE_1:
            return i2s1_rx_chan;
        default:
            ESP_LOGE(TAG, "Invalid I2S interface: %d", interface);
            return NULL;
    }
}

const audio_codec_data_if_t *bsp_i2s_get_codec_itf(bsp_i2s_interface_t interface)
{
    switch (interface) {
        case BSP_I2S_INTERFACE_0:
            return i2s0_data_if;
        case BSP_I2S_INTERFACE_1:
            return i2s1_data_if;
        default:
            ESP_LOGE(TAG, "Invalid I2S interface: %d", interface);
            return NULL;
    }
}

/* Second ES8388 (I2S1, onboard mic inputs -> +24 dB MicAmp pre-gain). */
static void codec1_bringup(i2c_master_bus_handle_t i2c_handle)
{
    bsp_es8388_config_t es8388_cfg2 = {
        .i2c_handle = i2c_handle,
        .i2c_addr = BSP_ES8388_CODEC_ADDR_ALT,  // Alternative address (0x11)
        .i2s_interface = BSP_I2S_INTERFACE_1,
        .codec_mode = BSP_ES8388_MODE_BOTH,
        .adc_input = BSP_ES8388_ADC_INPUT_LINE1,
        .dac_output = BSP_ES8388_DAC_OUTPUT_ALL,
        .mic_amp_gain = 8,
    };
    esp_err_t ret = bsp_es8388_init(&es8388_cfg2);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES8388 codec 1 initialization failed - trying address 0x10");
        /* Try with same address if ALT address fails (if using I2C multiplexer) */
        es8388_cfg2.i2c_addr = BSP_ES8388_CODEC_ADDR;
        ret = bsp_es8388_init(&es8388_cfg2);
    }
    if (ret == ESP_OK) {
        ret = bsp_es8388_start(BSP_I2S_INTERFACE_1, BSP_ES8388_MODE_BOTH);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "ES8388 codec 1 start failed");
        } else {
            ESP_LOGI(TAG, "Second ES8388 codec initialized successfully");
        }
    } else {
        ESP_LOGW(TAG, "Could not initialize second ES8388 codec");
    }
}

static i2c_master_bus_handle_t s_codec1_task_i2c;
static SemaphoreHandle_t s_codec1_task_done;

static void codec1_bringup_task(void *arg)
{
    (void)arg;
    codec1_bringup(s_codec1_task_i2c);
    xSemaphoreGive(s_codec1_task_done);
    vTaskDelete(NULL);
}

esp_err_t bsp_i2s_es8388_init(i2c_master_bus_handle_t i2c_handle, 
                              const i2s_std_config_t *i2s_config,
                              bool dual_codec)
{
    if (i2c_handle == NULL) {
        ESP_LOGE(TAG, "Invalid I2C handle");
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Initializing I2S with ES8388 codec(s) (dual: %s)...", dual_codec ? "yes" : "no");
    
    /* Initialize I2S interfaces */
    esp_err_t ret = bsp_i2s_init_dual(i2s_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S initialization failed");
        return ret;
    }
    
    /* Initialize ES8388 codec on I2S0 — line-level AIN jacks, no MicAmp pre-gain */
    bsp_es8388_config_t es8388_cfg = {
        .i2c_handle = i2c_handle,
        .i2c_addr = BSP_ES8388_CODEC_ADDR,
        .i2s_interface = BSP_I2S_INTERFACE_0,
        .codec_mode = BSP_ES8388_MODE_BOTH,
        .adc_input = BSP_ES8388_ADC_INPUT_LINE1,
        .dac_output = BSP_ES8388_DAC_OUTPUT_ALL,
        .mic_amp_gain = 0,
    };
    
    /* The second codec comes up on its own task while this one does the
     * first: each bring-up is ~1.4 s of anti-pop waits around a few I2C
     * writes, and the i2c_master driver serialises the bus, so the two
     * sequences interleave instead of queueing -- half the boot-time cost. */
    SemaphoreHandle_t codec1_done = NULL;
    if (dual_codec) {
        codec1_done = xSemaphoreCreateBinary();
        if (codec1_done) {
            s_codec1_task_i2c = i2c_handle;
            s_codec1_task_done = codec1_done;
            ESP_LOGI(TAG, "Initializing second ES8388 codec on I2S1...");
            if (xTaskCreatePinnedToCore(codec1_bringup_task, "codec1_init", 3072, NULL, 5, NULL,
                                        tskNO_AFFINITY) != pdPASS) {
                vSemaphoreDelete(codec1_done);
                codec1_done = NULL;
                ESP_LOGW(TAG, "codec 1 task not created; bringing it up in sequence");
            }
        }
    }
    ret = bsp_es8388_init(&es8388_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ES8388 codec 0 initialization failed");
        if (codec1_done) {
            xSemaphoreTake(codec1_done, pdMS_TO_TICKS(5000));
            vSemaphoreDelete(codec1_done);
        }
        bsp_i2s_deinit_all();
        return ret;
    }
    /* Start ES8388 codec 0 */
    ret = bsp_es8388_start(BSP_I2S_INTERFACE_0, BSP_ES8388_MODE_BOTH);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ES8388 codec 0 start failed");
    }
    if (dual_codec) {
        if (codec1_done) {
            if (xSemaphoreTake(codec1_done, pdMS_TO_TICKS(5000)) != pdTRUE) {
                ESP_LOGW(TAG, "codec 1 bring-up did not finish in 5 s");
            }
            vSemaphoreDelete(codec1_done);
        } else {
            codec1_bringup(i2c_handle);
        }
    }
    
    ESP_LOGI(TAG, "I2S with ES8388 initialized successfully (%s codec mode)", 
             dual_codec ? "dual" : "single");
    return ESP_OK;
}

esp_err_t bsp_i2s_es8388_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing I2S and ES8388...");
    
    /* Stop and deinitialize all ES8388 codecs */
    bsp_es8388_stop(BSP_I2S_INTERFACE_0, BSP_ES8388_MODE_BOTH);
    bsp_es8388_stop(BSP_I2S_INTERFACE_1, BSP_ES8388_MODE_BOTH);
    bsp_es8388_deinit();
    
    /* Deinitialize I2S */
    bsp_i2s_deinit_all();
    
    ESP_LOGI(TAG, "I2S and ES8388 deinitialized");
    return ESP_OK;
}
