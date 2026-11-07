/*
 * Copyright (c) 2026 Marcel Licence
 */

/**
 * @file ml_click.h
 * @brief Header file for the Click module.
 *
 * This module provides timing and beat control for audio processing.
 * It allows synchronization with beats (e.g., for metronome or sequencer use).
 *
 * @date 29.04.2025
 * @author Marcel Licence
 */

#ifndef CLICK_H_
#define CLICK_H_

#include <stdint.h>
#include <stdbool.h>  /**< Needed for 'bool' type */

/**
 * @brief Initializes the Click module.
 *
 * This function must be called before using any other Click functions.
 *
 * @param sample_rate The audio sample rate in Hz (e.g., 44100.0f).
 */
void Click_Setup(float sample_rate);

/**
 * @brief Call all callbacks with the current data
 */
void Click_RequestAllInfos(void);

/**
 * @brief Processes audio data using the Click module.
 *
 * This function should be called inside the audio processing loop.
 * It modifies the provided audio buffers.
 *
 * @param left Pointer to the left channel buffer.
 * @param right Pointer to the right channel buffer.
 * @param len Number of samples in each buffer.
 * @return Number of processed samples.
 */
uint32_t Click_Process(float *left, float *right, int len);

/**
 * @brief Same as Click_Process but for 16-bit integer samples.
 *
 * @param left Pointer to the left channel buffer.
 * @param right Pointer to the right channel buffer.
 * @param len Number of samples in each buffer.
 * @return Number of processed samples.
 */
uint32_t Click_Process(int16_t *left, int16_t *right, int len);

/**
 * @brief Gets the current tempo in beats per minute (BPM).
 *
 * @return The current BPM.
 */
float Click_GetBpm(void);

/**
 * @brief Gets the current beat position.
 *
 * Returns the index of the current beat in the bar.
 *
 * @return Beat position (starting from 0).
 */
int Click_GetPos(void);

/**
 * @brief Returns elapsed pulses from first beat
 */
int Click_GetSum(void);

/**
 * @brief Resets the beat to the beginning of the bar.
 */
void Click_Reset(void);

/**
 * @brief Sets whether the click sound is audible.
 *
 * @param audible If true, the click will be heard. If false, it is silent.
 */
void Click_SetAudible(bool audible);

/**
 * @brief Sets the tempo of the click.
 *
 * @param bpm New BPM value.
 */
void Click_SetBpm(float bpm);

/**
 * @brief Sets the tempo of the click used within midi mapping
 *
 * @param tempo value between 0..127 will be mapped to 60 to 240
 */
void Click_SetBpmLog10FromU7(uint8_t param, uint8_t tempo);

/**
 * @brief Enables or disables the Click module.
 *
 * When disabled, internal processing is paused.
 * Equivalent to not calling Click_Process.
 *
 * @param enabled If true, the module is active. If false, it is paused.
 */
void Click_SetEnable(bool enabled);

/**
 * @brief Returns state if click is active
 */
bool Click_IsActive(void);

#if 0
/**
 * @brief Callback function triggered at the start of each bar.
 *
 * This function is called automatically when a new bar begins.
 * The user should implement this function to handle bar events.
 */
void Click_BarCompleteCb(void) __attribute__((weak));
#endif

/* callbacks */

void Click_Event(uint8_t event);

void Click_BarChanged(uint8_t current_bar);

void Click_TempoChanged(float bpm);

#define CLICK_EVENT_FIRST_ON 0
#define CLICK_EVENT_FIRST_OFF 1
#define CLICK_EVENT_OTHER_ON 2
#define CLICK_EVENT_OHTER_OFF 3

#endif /* CLICK_H_ */
