/*
 * SPDX-License-Identifier: Apache-2.0
 * CrossPad PCB pinout - revision 1.x
 */
#pragma once

#define BSP_HAS_STM32_UART        1
#define BSP_HAS_STM32_I2C         0
#define BSP_HAS_EEPROM            0
#define BSP_CODEC0_DAC_LR_CROSSED 0   /* not checked on this revision */
#define BSP_CODEC1_OUT_POLE_HZ    0   /* not measured on this revision */
#define BSP_CODEC1_OUT_INVERTED   0   /* not measured on this revision */

/* GPIO */
#define BSP_LED_STRIP_GPIO        (GPIO_NUM_17)
#define BSP_BUTTON_2              (GPIO_NUM_0)
#define BSP_STM_RST_PIN           (GPIO_NUM_46)

/* ADC */
#define ADC_PIN                   (GPIO_NUM_8)

/* Back cover detect (none on rev1) */
#define BSP_BACK_COVER_PIN        (GPIO_NUM_NC)

/* IMU interrupt (not wired on rev1) */
#define BSP_IMU_INT2_PIN          (GPIO_NUM_NC)

/* I2C (codecs / touch / IMU) */
#define BSP_I2C_SCL               (GPIO_NUM_10)
#define BSP_I2C_SDA               (GPIO_NUM_9)

/* UART (STM32) */
#define BSP_UART_TX               (GPIO_NUM_47)
#define BSP_UART_RX               (GPIO_NUM_21)

/* Audio inputs: one per codec, no input pairs. */
#define BSP_HAS_SHARED_LINE_IN    0
#define BSP_CODEC_LOOP_CODEC      (1)
#define BSP_CODEC_LOOP_DAC_OUTPUT (0x00)

/* Analog output levels: which codec output pairs drive the speakers and
 * which a jack has not been measured on this revision, so codec 0 is the
 * speakers as a whole and Out 1/2 has no level of its own (0x00). */
#define BSP_OUT_SPEAKERS_CODEC    (0)
#define BSP_OUT_SPEAKERS_PAIRS    (0x3C)
#define BSP_OUT_JACK12_CODEC      (0)
#define BSP_OUT_JACK12_PAIRS      (0x00)
#define BSP_OUT_JACK34_CODEC      (1)
#define BSP_OUT_JACK34_PAIRS      (0x3C)

/* I2S audio */
#define BSP_I2S_SCLK              (GPIO_NUM_38)
#define BSP_I2S_MCLK              (GPIO_NUM_39)
#define BSP_I2S_LCLK              (GPIO_NUM_41)
#define BSP_I2S_DOUT              (GPIO_NUM_42)
#define BSP_I2S_DSIN              (GPIO_NUM_2)
#define BSP_I2S_DOUT2             (GPIO_NUM_40)
#define BSP_I2S_DSIN2             (GPIO_NUM_1)

/* LCD / Touch (LCD_RST shared with touch reset) */
#define BSP_LCD_DATA0             (GPIO_NUM_13)
#define BSP_LCD_PCLK              (GPIO_NUM_12)
#define BSP_LCD_CS                (GPIO_NUM_11)
#define BSP_LCD_DC                (GPIO_NUM_18)
#define BSP_LCD_RST               (GPIO_NUM_14)
#define BSP_LCD_TOUCH_INT         (GPIO_NUM_3)

/* USB */
#define BSP_USB_POS               (GPIO_NUM_20)
#define BSP_USB_NEG               (GPIO_NUM_19)

/* SDMMC (legacy hardcoded values from bsp_sdmmc.h) */
#define BSP_SD_D0                 (GPIO_NUM_16)
#define BSP_SD_D1                 (GPIO_NUM_15)
#define BSP_SD_D2                 (GPIO_NUM_4)
#define BSP_SD_D3                 (GPIO_NUM_5)
#define BSP_SD_CMD                (GPIO_NUM_6)
#define BSP_SD_CLK                (GPIO_NUM_7)
#define BSP_SD_DET                (GPIO_NUM_NC)
#define BSP_SD_WIDTH              4

/* Buttons */
#define BSP_BUTTON_CONFIG_IO      (GPIO_NUM_0)
