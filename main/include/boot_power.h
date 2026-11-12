/**
 * @file boot_power.h
 * @brief Power and restart (boot_power.cpp): what app_main needs from the
 *        soft-off gate, the qualified wake and the STM's power states.
 */
#pragma once

#include <stdint.h>

#include "crosspad/pad/PadAnimator.hpp"

/* Hardware-reset the USB-OTG controller; also the shutdown handler every
 * esp_restart() runs. */
void usb_otg_hw_reset(void);

/* Restart into the ROM download mode. */
void enter_bootloader();

/* A black frame on the pad strip, sent at once. */
void force_pad_strip_dark();

/* Holds the boot until the power button has been down long enough to make the
 * wake intentional; returns to soft-off otherwise. Learns a charge boot
 * (charge_boot_active()). */
void qualify_power_button_wake();

/* Pad 16 held through the boot: recovery to the main firmware. */
bool recovery_pad_held();

/* stm32_pwr_sync callback: the ESP's side of the STM's power states. */
void app_pwr_state_cb(uint8_t state, void *user_data);

/* bsp_worker job: sees SLEEP_PENDING and takes the ESP into soft-off. */
uint32_t softoff_job(void *);

/* The qualified wake's hold-progress animation on the pads, for the boot to
 * stop once the splash takes them. */
extern bool s_wake_hold_qualified;
extern crosspad::PadAnimator::AnimToken s_wake_hold_anim;
