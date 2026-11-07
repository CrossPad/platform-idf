#ifndef PINDEFINITIONS_H
#define PINDEFINITIONS_H

// usiversal typical minimum includes template

#include <stdlib.h>
#include <stdint.h>

#include "esp_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


////// Hard settings //////

// Touch screen settings
#define FT6236_X_OFFSET 1
#define FT6236_Y_OFFSET 1 
#define TOUCH_TRESHOLD 15


///////// MIDI API settings //////////
// Single source of truth in crosspad-core
#include "crosspad/protocol/SysExProtocol.h"

// Backward-compat aliases used by midiCallbacks.cpp etc.
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

// Audio settings
#define ESP32_AUDIO_KIT
#define SAMPLE_BUFFER_SIZE  128
#define SAMPLE_RATE 44100
#define SAMPLE_SIZE_16BIT

#ifndef SAMPLER_MAX_POLYPHONY
#define SAMPLER_MAX_POLYPHONY 16 // how many mono audio streams can run at once
#endif

#define ES8388_ENABLED
#define KEEP_CODEC_MUTED_IN_SETUP

// audio codec pinout
#define I2S_MCLK_PIN ES8388_PIN_MCLK
#define I2S_BCLK_PIN ES8388_PIN_SCLK
#define I2S_WCLK_PIN ES8388_PIN_LRCK
#define I2S_DOUT_PIN ES8388_PIN_DIN
#define I2S_DIN_PIN ES8388_PIN_2_DOUT


#define SAMPLER_INPUT_KEYPAD    // Use the keypad to trigger samples

// #define USE_WIFI
// #define USE_STATIC_IP

///////   GUI settings   ////////
// status bar settings
#define LV_STATUS_BAR_HEIGHT 30 // pixels
#define QUICK_BUTTON_SIZE 25 // height in pixels
// app drawer settings
#define APP_BUTTON_SIZE 64 // size in pixels
#define APP_BUTTON_NAME_VISIBLE true // set to false to hide app names
#define LV_HIDE_QUICK_SETTINGS true // hide the quick settings button in the status bar
#define KIT_AUTO_LOAD_ON_BOOT false // load the last used kit on boot automatically

// Audio jack detection settings - PinSetup.cpp
enum Jacks
{
    JACK_HP_OUT,
    JACK_LINE_IN_1,
    JACK_LINE_OUT,
    JACK_LINE_IN_2
};

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
