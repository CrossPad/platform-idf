#pragma once

// On S3: the real CrossPad.h from the crosspad BSP component is the authoritative
// source. Guard against re-defining everything if the BSP version was included first
// (it uses #ifndef PINDEFINITIONS_H, so the include guard handles it).
#ifndef PINDEFINITIONS_H
#define PINDEFINITIONS_H

#ifdef CONFIG_IDF_TARGET_ESP32S3
// On S3: pull in the BSP hardware header (pin macros, BSP_BOARD_CROSSPAD_S3, etc.)
#include "crosspad_s3.h"
#include "imu.h"
#endif

#include <stdlib.h>
#include <stdint.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// MIDI SysEx protocol — from crosspad-core, portable
#include "crosspad/protocol/SysExProtocol.h"
#define MIDI_SYSEX_MANUFACTURER  CROSSPAD_SYSEX_MANUFACTURER
#define MIDI_SYSEX_START         CROSSPAD_SYSEX_START
#define MIDI_SYSEX_END           CROSSPAD_SYSEX_END
#define ENCODER_COUNTER          CROSSPAD_CMD_ENCODER_COUNTER
#define BUTTONS_DIGITAL          CROSSPAD_CMD_BUTTONS_DIGITAL
#define BUTTONS_ANALOG           CROSSPAD_CMD_BUTTONS_ANALOG
#define STATUS_REGISTER          CROSSPAD_CMD_STATUS_REGISTER
#define ROUTING_MATRIX           CROSSPAD_CMD_ROUTING_MATRIX
#define MIDI_SYSEX_POWEROFF      CROSSPAD_CMD_POWEROFF
#define MIDI_SYSEX_BRIGHTNESS    CROSSPAD_CMD_BRIGHTNESS
#define MIDI_SYSEX_MIDIOUT       CROSSPAD_CMD_MIDIOUT
#define SYSEX_STRING             CROSSPAD_CMD_STRING
#define SYSEX_DFU_REQUEST        CROSSPAD_CMD_DFU_REQUEST
#define SYSEX_KEEP_ALIVE         CROSSPAD_CMD_KEEP_ALIVE
#define SYSEX_USB_MODE           CROSSPAD_CMD_USB_MODE
#define VOLTAGES_MILLIVOLTS      CROSSPAD_CMD_VOLTAGES_MV
#define CHG_STATUS               CROSSPAD_CMD_CHG_STATUS
#define CHG_FAULT                CROSSPAD_CMD_CHG_FAULT
#define ADC_VALUES               CROSSPAD_CMD_ADC_VALUES
#define ADC_BUTTONS              CROSSPAD_CMD_ADC_BUTTONS

#define USE_SYSEX_FOR_BUTTONS    true

// GUI layout constants
#define LV_STATUS_BAR_HEIGHT     30
#define QUICK_BUTTON_SIZE        25
#define APP_BUTTON_SIZE          64
#define APP_BUTTON_NAME_VISIBLE  true
#define LV_HIDE_QUICK_SETTINGS   true
#define KIT_AUTO_LOAD_ON_BOOT    false

// CrossPad core types
#include "crosspad/settings/CrosspadSettings.hpp"
#include "crosspad/status/CrosspadStatus.hpp"
using crosspad::CrosspadSettings;
using crosspad::CROSSPAD_MAX_PADS;
using crosspad::KeypadSettings;
using crosspad::STMRoutingSettings;
using crosspad::ESPRoutingSettings;
using crosspad::WirelessSettings;
using crosspad::AudioSettings;
using crosspad::VibrationSettings;
using crosspad::Stm32Status;
using crosspad::CrosspadStatus;

#endif // PINDEFINITIONS_H
