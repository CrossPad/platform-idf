/**
 * @file CrosspadPlatform.hpp
 * @brief ESP-IDF platform singleton — provides access to crosspad-core interfaces
 *
 * This is the glue between the portable crosspad-core and ESP-IDF.
 * All platform-specific implementations are accessed through singletons here.
 */
#pragma once

#include "sdkconfig.h"
#include "crosspad/midi/MidiInputHandler.hpp"
#include "crosspad/protocol/IStm32Transport.hpp"
#include "crosspad/protocol/Stm32CommandSender.hpp"
#include "crosspad/protocol/Stm32MessageHandler.hpp"
#include "crosspad/stm32/Stm32Manager.hpp"

namespace crosspad {

// Forward declarations for init function signatures
class CrosspadSettings;
class CrosspadStatus;
class MarcelSampleNode;

/// Get the STM32 transport instance
IStm32Transport& getStm32Transport();

/// Get the STM32 command sender instance
Stm32CommandSender& getStm32CommandSender();

/// Get the STM32 message handler instance
Stm32MessageHandler& getStm32MessageHandler();

/// Get the STM32 manager instance
Stm32Manager& getStm32Manager();

/// Get the MIDI input handler instance
MidiInputHandler& getMidiInputHandler();

#if CONFIG_CROSSPAD_SAMPLER_MARCEL
/// The prebuilt streamer's node, or nullptr on boards without audio. Only in
/// an image built with it (Kconfig CROSSPAD_SAMPLER_MARCEL).
MarcelSampleNode* getMarcelSampleNode();
#endif

/**
 * @brief Initialize all platform services
 *
 * Call once during app_main(), before any component uses getEventBus() etc.
 * settings and status must remain valid for the lifetime of the application.
 */
void CrosspadPlatform_Init(CrosspadSettings* settings = nullptr,
                            CrosspadStatus* status = nullptr);

/**
 * @brief Wire STM32 framework logic after UART is ready
 *
 * Call after bsp_stm32_init() (which initializes UART).
 * Starts Stm32Manager and the task that runs its maintenance.
 */
void CrosspadPlatform_StartStm32();

/**
 * @brief Stop the STM32 maintenance task, before the STM link is torn down.
 */
void CrosspadPlatform_StopStm32();

/**
 * @brief Probe + start the DRV2605 haptic driver (Drv2605Driver).
 *
 * Spawns a one-shot task: initHardware() blocks up to ~5 s (auto-calibration),
 * so it must not run on the main init path. Call once after bsp_i2c_init().
 * Until the task finishes (or if the chip is absent) getVibration() stays NullVibration.
 */
void CrosspadPlatform_StartVibration();

/**
 * @brief The sequencer fires its notes from the render instead of its own
 * poll task (true), or from the task again (false).
 *
 * The task polls every 2 ms of the wall clock: in real time that is the jitter
 * a fired note inherits, offline (a freeze faster than real time) it is many
 * blocks of the render. Offline the render polls after every pass, so a note
 * lands on the block after the one it falls in, as it would in time.
 * Returns once the task is not inside a poll. Not from the render.
 */
void sequencer_poll_from_render(bool on);
/// The render's poll while sequencer_poll_from_render() holds it.
void sequencer_poll_now();

} // namespace crosspad
