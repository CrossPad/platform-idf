# ES8388 Codec + I2S Driver Examples

This directory contains examples demonstrating how to use the ES8388 audio codec with the BSP I2S driver.

## Overview

The ES8388 is a high-performance audio codec that supports:
- Stereo ADC (microphone input) up to 24-bit/96kHz
- Stereo DAC (speaker output) up to 24-bit/96kHz
- I2C control interface
- I2S audio data interface
- Built-in analog mixer and volume control

## Features

- **Dual I2S Support**: Master (I2S0) and Slave (I2S1) configurations
- **ESP-ADF Compatible**: Full compatibility with ESP Audio Development Framework
- **Flexible Configuration**: Support for various sample rates and bit depths
- **Volume Control**: Software volume control (0-100%)
- **Mute Function**: Software mute/unmute
- **ADC/DAC Control**: Independent control of ADC and DAC

## Hardware Connections

### I2C (Control Interface)
```
ESP32-S3          ES8388
GPIO9  (SDA) <--> SDA
GPIO10 (SCL) <--> SCL
```

### I2S0 (Master - Audio Interface)
```
ESP32-S3           ES8388
GPIO39 (MCLK) ---> MCLK
GPIO38 (SCLK) ---> SCLK/BCLK
GPIO41 (LCLK) ---> LRCK/WS
GPIO42 (DOUT) ---> DIN (DAC input)
GPIO2  (DIN)  <--- DOUT (ADC output)
```

### I2S1 (Slave - Optional Second Codec)
```
ESP32-S3           ES8388 #2
GPIO38 (SCLK) ---> SCLK (shared)
GPIO41 (LCLK) ---> LRCK (shared)
GPIO40 (DOUT) ---> DIN
GPIO1  (DIN)  <--- DOUT
```

## API Usage

### Basic Initialization

```c
#include "crosspad_s3.h"
#include "i2s_driver.h"
#include "bsp_es8388.h"

// 1. Initialize I2C
bsp_i2c_init();
i2c_master_bus_handle_t i2c = bsp_i2c_get_handle();

// 2. Initialize I2S + ES8388 (uses default 48kHz, 32-bit, stereo)
bsp_i2s_es8388_init(i2c, NULL, false);

// 3. Set volume and unmute
bsp_es8388_set_volume(70);  // 0-100%
bsp_es8388_set_mute(false);
```

### Custom Configuration

```c
// Configure for 44.1kHz, 16-bit
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

### Audio Recording

```c
// Get RX handle
i2s_chan_handle_t rx = bsp_i2s_get_rx_handle(BSP_I2S_INTERFACE_0);

// Read audio data
int32_t buffer[480];  // 240 frames * 2 channels
size_t bytes_read;

i2s_channel_read(rx, buffer, sizeof(buffer), &bytes_read, portMAX_DELAY);
```

### Audio Playback

```c
// Get TX handle
i2s_chan_handle_t tx = bsp_i2s_get_tx_handle(BSP_I2S_INTERFACE_0);

// Write audio data
int32_t buffer[480];  // Fill with audio samples
size_t bytes_written;

i2s_channel_write(tx, buffer, sizeof(buffer), &bytes_written, portMAX_DELAY);
```

### Volume Control

```c
// Set volume (0-100)
bsp_es8388_set_volume(80);

// Get current volume
int volume;
bsp_es8388_get_volume(&volume);

// Mute/Unmute
bsp_es8388_set_mute(true);   // Mute
bsp_es8388_set_mute(false);  // Unmute
```

## Examples

### 1. es8388_example.c
Complete example demonstrating:
- Basic initialization
- Audio recording
- Audio playback
- Audio loopback (mic to speaker)
- **Audio loopback with performance monitoring (PERFMON)**
- Volume control
- Custom I2S configuration
- ESP-ADF integration
- Dual codec operation

**Build modes** (edit `es8388_example.c`):
- `DUAL_CODEC_MODE=0` `PERFMON_MODE=0` — Standard examples
- `DUAL_CODEC_MODE=1` — Dual ES8388 codec example
- `PERFMON_MODE=1` — I2S loopback with hardware performance counters

To build and run:
```bash
idf.py build flash monitor
```

### Performance Monitoring (PERFMON)

The PERFMON example demonstrates how to use Xtensa hardware performance counters to profile I2S audio loops. Measured metrics include:
- CPU cycles
- Instruction count
- Cache hit/miss rates (I-cache and D-cache)
- Cycles per instruction (CPI)

See [../PERFMON.md](../PERFMON.md) for detailed usage guide and integration instructions.

## Troubleshooting

### No Audio Output
1. Check I2C connections (SDA, SCL)
2. Verify ES8388 I2C address (0x10 or 0x11)
3. Ensure codec is unmuted: `bsp_es8388_set_mute(false)`
4. Check volume level: `bsp_es8388_set_volume(70)`

### No Audio Input
1. Verify microphone connections
2. Check ADC input configuration
3. Ensure I2S RX is enabled

### I2C Communication Errors
1. Check pull-up resistors on SDA/SCL (typically 4.7kΩ)
2. Verify I2C clock speed (400kHz recommended)
3. Check ES8388 power supply (3.3V)

### I2S Synchronization Issues
1. Ensure MCLK is connected and stable
2. Verify BCLK/LRCK signals
3. Check I2S master/slave configuration

## Performance Notes

- **Sample Rate**: 8kHz to 96kHz supported
- **Bit Depth**: 16-bit, 24-bit, 32-bit
- **Latency**: ~10ms typical (depends on buffer size)
- **Power Consumption**: ~15mA active, <1mA in standby

## References

- [ES8388 Datasheet](http://www.everest-semi.com/pdf/ES8388%20DS.pdf)
- [ESP-ADF Documentation](https://docs.espressif.com/projects/esp-adf/en/latest/)
- [ESP32-S3 I2S Documentation](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)

## License

Apache License 2.0
