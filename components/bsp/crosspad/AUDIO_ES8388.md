# ES8388 Audio Codec Integration

This document describes the ES8388 audio codec integration with the CrossPad S3 BSP.

## Overview

The CrossPad S3 board features **dual ES8388 audio codecs** with independent I2S interfaces, providing professional-quality audio input and output capabilities.

### Key Features

- **Dual Codec Architecture**:
  - I2S0 (Master mode): Primary codec with full ADC/DAC support  
  - I2S1 (Slave mode): Secondary codec sharing clock signals
- **High-Quality Audio**: Up to 96kHz/24-bit stereo
- **Full Duplex**: Simultaneous recording and playback
- **Software Control**: Volume, mute, routing via I2C
- **ESP-ADF Compatible**: Full integration with ESP Audio Development Framework

## Hardware Configuration

### I2C Connections (Codec Control)
```
ESP32-S3          ES8388
GPIO9  (SDA) <--> SDA
GPIO10 (SCL) <--> SCL
```

Both addresses are in use: codec #1 (I2S0) is at **0x10** (`BSP_ES8388_CODEC_ADDR`,
CE low), codec #2 (I2S1) at **0x11** (`BSP_ES8388_CODEC_ADDR_ALT`, CE high;
`include/bsp_es8388.h`). `i2s_driver.c` falls back to 0x10 for codec #2 if 0x11
does not answer. The I2C pins above are Rev 1.x (v1); Rev 2.0 uses GPIO12 (SDA) /
GPIO11 (SCL). The I2S pins below are the same on both revisions. Every pin of
each revision is in `include/pinout/pinout_v*.h`.

### I2S0 - Master Interface
```
ESP32-S3           ES8388 #1
GPIO39 (MCLK) ---> MCLK
GPIO38 (SCLK) ---> SCLK/BCLK
GPIO41 (LCLK) ---> LRCK/WS
GPIO42 (DOUT) ---> DIN (DAC input)
GPIO2  (DIN)  <--- DOUT (ADC output)
```

### I2S1 - Slave Interface  
```
ESP32-S3           ES8388 #2
GPIO38 (SCLK) ---> SCLK (shared from I2S0)
GPIO41 (LCLK) ---> LRCK (shared from I2S0)
GPIO40 (DOUT) ---> DIN
GPIO1  (DIN)  <--- DOUT
```

## Software Architecture

### Module Structure

```
bsp/crosspad/
├── i2s_driver.h/c        # I2S peripheral driver
├── bsp_es8388.h/c        # ES8388 codec driver
└── examples/
    ├── es8388_example.c  # Usage examples
    └── README.md         # Example documentation
```

### API Layers

```
┌─────────────────────────────────────┐
│   Application / ESP-ADF Pipeline    │
├─────────────────────────────────────┤
│   BSP ES8388 API (bsp_es8388_*)    │
│   - Volume control                  │
│   - Mute/unmute                     │
│   - ADC/DAC routing                 │
├─────────────────────────────────────┤
│   BSP I2S API (bsp_i2s_*)          │
│   - Channel management              │
│   - DMA configuration               │
│   - Clock generation                │
├─────────────────────────────────────┤
│   ESP-IDF I2S Driver               │
└─────────────────────────────────────┘
```

## Quick Start

### 1. Basic Initialization (Single Codec)

```c
#include "crosspad_s3.h"
#include "i2s_driver.h"
#include "bsp_es8388.h"

/* Initialize I2C */
bsp_i2c_init();
i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();

/* Initialize I2S + ES8388 with defaults (single codec mode) */
bsp_i2s_es8388_init(i2c, NULL, false);

/* Configure audio for codec on I2S0 */
bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, 70);
bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false);
```

### 1b. Dual Codec Initialization

```c
/* Initialize I2S + BOTH ES8388 codecs */
bsp_i2s_es8388_init(i2c, NULL, true);

/* Configure independent volumes for each codec */
bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, 80);  // Codec 0: 80%
bsp_es8388_set_volume(BSP_I2S_INTERFACE_1, 60);  // Codec 1: 60%

/* Unmute both codecs */
bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false);
bsp_es8388_set_mute(BSP_I2S_INTERFACE_1, false);
```

### 2. Audio Recording

```c
/* Get RX handle */
i2s_chan_handle_t rx = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_0);

/* Allocate buffer */
int32_t audio_buffer[480];  // 240 frames × 2 channels
size_t bytes_read;

/* Record audio */
esp_err_t ret = i2s_channel_read(
    rx, 
    audio_buffer, 
    sizeof(audio_buffer),
    &bytes_read, 
    pdMS_TO_TICKS(1000)
);
```

### 3. Audio Playback

```c
/* Get TX handle */
i2s_chan_handle_t tx = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_0);

/* Prepare audio data */
int32_t audio_buffer[480];
// ... fill with audio samples ...

/* Play audio */
size_t bytes_written;
esp_err_t ret = i2s_channel_write(
    tx,
    audio_buffer,
    sizeof(audio_buffer),
    &bytes_written,
    pdMS_TO_TICKS(1000)
);
```

## Advanced Usage

### Custom I2S Configuration

```c
/* Configure 44.1kHz, 16-bit, stereo */
i2s_std_config_t i2s_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
        I2S_DATA_BIT_WIDTH_16BIT,
        I2S_SLOT_MODE_STEREO
    ),
    .gpio_cfg = {
        .mclk = BSP_I2S_MCLK,
        .bclk = BSP_I2S_SCLK,
        .ws = BSP_I2S_LCLK,
        .dout = BSP_I2S_DOUT,
        .din = BSP_I2S_DSIN,
    },
};

i2s_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

bsp_i2s_es8388_init(i2c, &i2s_cfg, false);
```

### Direct ES8388 Control

```c
/* Manual ES8388 initialization for codec on I2S0 */
bsp_es8388_config_t es8388_cfg = {
    .i2c_handle = i2c,
    .i2c_addr = BSP_ES8388_CODEC_ADDR,
    .i2s_interface = BSP_I2S_INTERFACE_0,
    .codec_mode = BSP_ES8388_MODE_BOTH,
    .adc_input = BSP_ES8388_ADC_INPUT_LINE1,
    .dac_output = BSP_ES8388_DAC_OUTPUT_ALL,
};

bsp_es8388_init(&es8388_cfg);
bsp_es8388_start(BSP_I2S_INTERFACE_0, BSP_ES8388_MODE_BOTH);

/* Configure audio routing for codec 0 */
bsp_es8388_config_adc_input(BSP_I2S_INTERFACE_0, BSP_ES8388_ADC_INPUT_DIFFERENCE);
bsp_es8388_config_dac_output(BSP_I2S_INTERFACE_0, BSP_ES8388_DAC_OUTPUT_LINE1);

/* Initialize second codec on I2S1 */
bsp_es8388_config_t es8388_cfg2 = {
    .i2c_handle = i2c,
    .i2c_addr = BSP_ES8388_CODEC_ADDR_ALT,  // Alternative I2C address
    .i2s_interface = BSP_I2S_INTERFACE_1,
    .codec_mode = BSP_ES8388_MODE_BOTH,
    .adc_input = BSP_ES8388_ADC_INPUT_LINE1,
    .dac_output = BSP_ES8388_DAC_OUTPUT_ALL,
};

bsp_es8388_init(&es8388_cfg2);
bsp_es8388_start(BSP_I2S_INTERFACE_1, BSP_ES8388_MODE_BOTH);
```

### ESP-ADF Integration

```c
/* Get codec data interface for ESP-ADF */
const audio_codec_data_if_t *codec_if = 
    bsp_i2s_get_codec_itf(BSP_I2S_INTERFACE_0);

/* Use with ESP-ADF audio pipeline */
audio_element_handle_t i2s_stream_writer = 
    i2s_stream_init(&i2s_cfg);

audio_pipeline_link(pipeline, i2s_stream_writer);
audio_pipeline_run(pipeline);
```

## API Reference

### I2S Driver Functions

| Function | Description |
|----------|-------------|
| `bsp_i2s_init_dual()` | Initialize both I2S interfaces |
| `bsp_i2s_es8388_init()` | Initialize I2S + ES8388 codecs |
| `bsp_i2s_deinit_all()` | Deinitialize all I2S interfaces |
| `bsp_i2s_get_tx_handle()` | Get TX channel handle |
| `bsp_i2s_get_rx_handle()` | Get RX channel handle |
| `bsp_i2s_get_codec_itf()` | Get codec data interface |

### ES8388 Codec Functions

All ES8388 functions accept `bsp_i2s_interface_t interface` parameter to specify which codec (BSP_I2S_INTERFACE_0 or BSP_I2S_INTERFACE_1) to control.

| Function | Description |
|----------|-------------|
| `bsp_es8388_init()` | Initialize ES8388 codec on specified interface |
| `bsp_es8388_deinit()` | Deinitialize all ES8388 codecs |
| `bsp_es8388_deinit_interface()` | Deinitialize specific codec |
| `bsp_es8388_start(interface, mode)` | Start ADC/DAC on specific codec |
| `bsp_es8388_stop(interface, mode)` | Stop ADC/DAC on specific codec |
| `bsp_es8388_set_volume(interface, vol)` | Set output volume (0-100) for specific codec |
| `bsp_es8388_get_volume(interface, *vol)` | Get current volume of specific codec |
| `bsp_es8388_set_mute(interface, mute)` | Mute/unmute specific codec output |
| `bsp_es8388_config_adc_input(interface, input)` | Configure ADC input source for specific codec |
| `bsp_es8388_config_dac_output(interface, output)` | Configure DAC output routing for specific codec |
| `bsp_es8388_get_codec_dev(interface)` | Get ESP-ADF codec device handle |

## Configuration Options

### Supported Sample Rates
- 8 kHz
- 11.025 kHz
- 16 kHz
- 22.05 kHz
- 32 kHz
- 44.1 kHz
- 48 kHz
- 88.2 kHz
- 96 kHz

### Supported Bit Depths
- 16-bit
- 24-bit
- 32-bit

### ADC Input Options
- `BSP_ES8388_ADC_INPUT_LINE1` - Line input 1
- `BSP_ES8388_ADC_INPUT_LINE2` - Line input 2
- `BSP_ES8388_ADC_INPUT_DIFFERENCE` - Differential input

### DAC Output Options
- `BSP_ES8388_DAC_OUTPUT_LINE1` - LOUT1/ROUT1 powered
- `BSP_ES8388_DAC_OUTPUT_LINE2` - LOUT2/ROUT2 powered
- `BSP_ES8388_DAC_OUTPUT_ALL` - All outputs powered

The selection is the DACPOWER output-enable bits, kept across stop()/start();
an unpowered driver still holds its pins on VREF through 1.5 kOhm unless
`bsp_es8388_set_off_outputs_hiz()` (DACCONTROL23 VROI) puts it at 40 kOhm.

### Codec Modes
- `BSP_ES8388_MODE_ENCODE` - ADC only (recording)
- `BSP_ES8388_MODE_DECODE` - DAC only (playback)
- `BSP_ES8388_MODE_BOTH` - Full duplex (ADC + DAC)
- `BSP_ES8388_MODE_LINE_IN` - Line input passthrough

## Performance Characteristics

### Audio Quality
- **SNR (ADC)**: >90 dB
- **THD+N (DAC)**: <-85 dB
- **Dynamic Range**: >95 dB
- **Crosstalk**: >-80 dB

### Timing
- **Startup Time**: ~100 ms
- **Latency**: ~10 ms (DMA buffer dependent)
- **Settling Time**: <5 ms after volume change

### Power Consumption
- **Active (Both ADC+DAC)**: ~20 mA @ 3.3V
- **Playback Only**: ~15 mA @ 3.3V
- **Recording Only**: ~12 mA @ 3.3V
- **Standby**: <1 mA @ 3.3V

## Troubleshooting

### No Audio Output

**Symptoms**: No sound from speakers

**Solutions**:
1. Check I2C connections (SDA, SCL)
2. Verify ES8388 I2C address: `i2c_master_probe(i2c, 0x10, 100)`
3. Ensure unmuted: `bsp_es8388_set_mute(BSP_I2S_INTERFACE_0, false)`
4. Check volume: `bsp_es8388_set_volume(BSP_I2S_INTERFACE_0, 70)`
5. Verify DAC output routing
6. Check speaker amplifier enable (if external)

### No Audio Input

**Symptoms**: Recorded audio is silent or noise

**Solutions**:
1. Verify microphone power supply
2. Check ADC input configuration
3. Ensure correct ADC bias voltage
4. Test with known-good audio source
5. Check I2S RX data line connections

### I2C Communication Errors

**Symptoms**: `ESP_ERR_TIMEOUT`, `ESP_FAIL` from ES8388 functions

**Solutions**:
1. Verify pull-up resistors on SDA/SCL (4.7kΩ typical)
2. Check I2C clock speed (400 kHz max)
3. Verify ES8388 power supply (3.3V ±10%)
4. Check for I2C bus contention
5. Use logic analyzer to debug bus signals

### I2S Synchronization Issues

**Symptoms**: Clicks, pops, distorted audio

**Solutions**:
1. Verify MCLK signal stability
2. Check BCLK/LRCK waveforms
3. Ensure proper I2S master/slave configuration
4. Verify DMA buffer sizes (multiples of frame size)
5. Check for buffer underruns/overruns
6. Reduce system load or increase DMA buffer size

### Audio Artifacts

**Symptoms**: Noise, hum, distortion

**Solutions**:
1. Use proper grounding (star ground recommended)
2. Add decoupling capacitors (0.1µF + 10µF on VDD)
3. Route audio signals away from digital traces
4. Use shielded cables for external connections
5. Check for ground loops
6. Verify power supply quality

## Examples

Complete working examples are available in:
- `components/bsp/crosspad/examples/es8388_example.c`
- `components/bsp/crosspad/examples/README.md`

Examples demonstrate:
- Basic audio I/O
- Volume control
- Loopback test
- Custom configuration
- ESP-ADF integration

## References

### Datasheets & Documentation
- [ES8388 Datasheet](http://www.everest-semi.com/pdf/ES8388%20DS.pdf)
- [ESP32-S3 Technical Reference Manual](https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf)
- [ESP-ADF Documentation](https://docs.espressif.com/projects/esp-adf/en/latest/)
- [ESP-IDF I2S Driver](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)

### Related projects
- [ESP-ADF GitHub](https://github.com/espressif/esp-adf)
- [ESP-BOX](https://github.com/espressif/esp-box) - Similar dual codec design
- [ESP32-LyraT](https://docs.espressif.com/projects/esp-adf/en/latest/design-guide/dev-boards/get-started-esp32-lyrat.html)

## License

This BSP is provided under the Apache License 2.0.

## Support

For issues and questions:
- GitHub Issues: [CrossPad Repository](https://github.com/crosspad/crosspad)
- ESP32 Forum: [Espressif Forums](https://esp32.com/)
