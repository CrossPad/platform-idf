/*
 * SPDX-License-Identifier: Apache-2.0
 * CrossPad PCB pinout - revision 2.0
 *
 * Single I2C bus (codecs / touch / IMU). In rev2 the STM32 co-processor
 * joins as an additional I2C slave; UART link to STM32 stays as the main
 * MIDI note stream.
 */
#pragma once

#define BSP_HAS_STM32_UART        1
#define BSP_HAS_STM32_I2C         1
#define BSP_HAS_EEPROM            1
/* Codec 0 (Out 2: phones and the speakers) reaches its jack with left and
 * right crossed; the codec swaps them back (DACCONTROL1 DACLRSWAP). Heard
 * 2026-10-11: a left-only voice came out of the right ear on Out 2, and out
 * of the left on Out 1 (codec 1). */
#define BSP_CODEC0_DAC_LR_CROSSED 1
/* Codec 1 (Out 3/4) reaches its jack through a first-order low-pass that
 * codec 0's outputs do not have: -0.45 dB at 10 kHz, -0.95 at 15, -1.3 at
 * 18 against Out 1/2 -- a pole at ~30.3 kHz, fitted to 0.05 dB. It followed
 * the jack when the audio interface's two inputs were swapped (2026-10-29).
 * The render puts a shelf on that bus that undoes it (IdfAudioModule); 0 is
 * none. */
#define BSP_CODEC1_OUT_POLE_HZ    30300
/* ...and inverted: a noise burst came back from Out 3/4 with the opposite
 * sign to the file and to Out 1/2 (2026-10-29). The codec inverts its
 * DAC back (DACCONTROL6 DACINVL/DACINVR). */
#define BSP_CODEC1_OUT_INVERTED   1

/* GPIO */
#define BSP_LED_STRIP_GPIO        (GPIO_NUM_17)
#define BSP_BUTTON_2              (GPIO_NUM_0)
#define BSP_STM_RST_PIN           (GPIO_NUM_46)
#define BSP_IMU_INT2_PIN          (GPIO_NUM_48)
#define BSP_BACK_COVER_PIN        (GPIO_NUM_8)

/* No jack-detect ADC on rev2 (info comes from STM32) */
#define ADC_PIN                   (GPIO_NUM_NC)

/* I2C */
#define BSP_I2C_SCL               (GPIO_NUM_11)
#define BSP_I2C_SDA               (GPIO_NUM_12)
#define BSP_I2C_STM32_ADDR        (0x42)        /* 7-bit address for ESP32 to talk to STM32 (rev2) */
#define BSP_I2C_EEPROM_ADDR       (0x50)        /* 7-bit address for ESP32 to talk to EEPROM (rev2) */

/* UART (STM32) */
#define BSP_UART_TX               (GPIO_NUM_10)
#define BSP_UART_RX               (GPIO_NUM_9)

/* Audio inputs: three stereo inputs, two ADCs. The mics are codec 0 LINE2,
 * Line 1/2 codec 1 LINE2, Line 3/4 LINE1 of both codecs. The Line 3/4 jack
 * (J7, "UNIVERSAL") is also driven by codec 1's LOUT1/ROUT1 through 1 uF, L
 * and R crossed -- the DAC->ADC loop; codec 0 has no driver on it (schematic
 * rev 2.1, sheet "ES8388 Audio 2"). Listening to Line 3/4 powers that pair
 * down and lets go of the net (hi-Z). */
#define BSP_HAS_SHARED_LINE_IN    1
#define BSP_CODEC_LOOP_CODEC      (1)
#define BSP_CODEC_LOOP_DAC_OUTPUT (0x30)        /* LOUT1/ROUT1 (DACPOWER bits) */

/* Analog output levels: the codec and output pairs (DACPOWER bits) each
 * physical output is on. Measured 2026-10-12: codec 0 LOUT1/ROUT1 drives the
 * built-in speakers, LOUT2/ROUT2 the Out 1/2 jack -- one stream, two levels;
 * codec 1 LOUT2/ROUT2 is Out 3/4 (its LOUT1/ROUT1 is the loop above). */
#define BSP_OUT_SPEAKERS_CODEC    (0)
#define BSP_OUT_SPEAKERS_PAIRS    (0x30)        /* LOUT1/ROUT1 */
#define BSP_OUT_JACK12_CODEC      (0)
#define BSP_OUT_JACK12_PAIRS      (0x0C)        /* LOUT2/ROUT2 */
#define BSP_OUT_JACK34_CODEC      (1)
#define BSP_OUT_JACK34_PAIRS      (0x0C)        /* LOUT2/ROUT2 */

/* I2S audio */
#define BSP_I2S_SCLK              (GPIO_NUM_38)
#define BSP_I2S_MCLK              (GPIO_NUM_39)
#define BSP_I2S_LCLK              (GPIO_NUM_41)
#define BSP_I2S_DOUT              (GPIO_NUM_42)
#define BSP_I2S_DSIN              (GPIO_NUM_2)
#define BSP_I2S_DOUT2             (GPIO_NUM_40)
#define BSP_I2S_DSIN2             (GPIO_NUM_1)

/* LCD / Touch */
#define BSP_LCD_DATA0             (GPIO_NUM_47)
#define BSP_LCD_PCLK              (GPIO_NUM_14)
#define BSP_LCD_CS                (GPIO_NUM_13)
#define BSP_LCD_DC                (GPIO_NUM_21)
#define BSP_LCD_RST               (GPIO_NUM_18)
#define BSP_LCD_TOUCH_INT         (GPIO_NUM_3)

/* USB */
#define BSP_USB_POS               (GPIO_NUM_20)
#define BSP_USB_NEG               (GPIO_NUM_19)

/* SDMMC */
#define BSP_SD_D0                 (GPIO_NUM_5)
#define BSP_SD_D1                 (GPIO_NUM_4)
#define BSP_SD_D2                 (GPIO_NUM_16)
#define BSP_SD_D3                 (GPIO_NUM_15)
#define BSP_SD_CLK                (GPIO_NUM_6)
#define BSP_SD_CMD                (GPIO_NUM_7)
#define BSP_SD_DET                (GPIO_NUM_NC)
#define BSP_SD_WIDTH              4

/* Buttons */
#define BSP_BUTTON_CONFIG_IO      (GPIO_NUM_0)
