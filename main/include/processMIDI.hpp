#pragma once

#define MIDI_DEBUGGING false

#include "CrossPad.h"
#include <MIDI.h>


/////////     MIDI task     ////////

void uartLoop(void *pvParameters);

/// Wire MIDI output targets to MidiInputHandler
void midiCallbacks_init();

/// Bounce foreign-manufacturer SysEx from USB back to the host (bench only,
/// not persisted). Lets a host tool reach a DAW under Wine through the board.
void usbMidi_setSysExEcho(bool on);
bool usbMidi_getSysExEcho();

/////////     BLE MIDI     ////////

/// Start the BLE MIDI stack. mode: 0 = Host (central), 1 = Server (peripheral).
bool bleMidi_start(uint8_t mode);

/// Stop the stack and give its RAM back.
void bleMidi_stop();

/// True when the last start was put off because WiFi holds the radio: BLE
/// starts on its own when the radio stops.
bool bleMidi_deferred_for_wifi();

/// WiFi takes the radio: from here to bleMidi_wifi_release() no start is made,
/// only remembered. A running stack is stopped after replyDelayMs (a reply
/// still leaving over BLE). True = it was running.
bool bleMidi_wifi_hold(uint32_t replyDelayMs);

/// The radio is off again: makes the start asked for meanwhile, else, with
/// restore (it was running at the hold) and BLE enabled in settings, restarts
/// it. A stop asked for meanwhile wins over both.
void bleMidi_wifi_release(bool restore);

/// Start BLE MIDI if wireless settings ask for it (called once at boot).
void bleMidi_boot();

/// Push the note offsets from settings into the BLE transport (in and out).
void bleMidi_applyNoteOffsets();


/////////   MIDI callbacks   ///////

void onNoteOn(uint8_t channel, uint8_t note, uint8_t velocity, uint16_t timestamp);

void onNoteOff(uint8_t channel, uint8_t note, uint8_t velocity, uint16_t timestamp);

void onControlChange(uint8_t channel, uint8_t controller, uint8_t value, uint16_t timestamp);

void onNoteOn2Audio(midi::Channel channel, byte note, byte velocity);

void onNoteOff2Audio(midi::Channel channel, byte note, byte velocity);
