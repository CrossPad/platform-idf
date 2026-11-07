/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "uac_descriptors.h"

//------------- CLASS -------------//
// The number of audio interfaces
#ifndef CFG_TUD_AUDIO
#define CFG_TUD_AUDIO             1
#endif

//--------------------------------------------------------------------
// AUDIO CLASS DRIVER CONFIGURATION
//--------------------------------------------------------------------

// Enable feedback EP
#ifndef CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_EP                             1
#endif

// Enable/disable conversion from 16.16 to 10.14 format on full-speed devices
#if CONFIG_UAC_SUPPORT_MACOS
#define CFG_TUD_AUDIO_ENABLE_FEEDBACK_FORMAT_CORRECTION              1
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_DESC_LEN
#define CFG_TUD_AUDIO_FUNC_1_DESC_LEN                                TUD_AUDIO_DEVICE_DESC_LEN
#endif

// How many formats are used, need to adjust USB descriptor if changed
#ifndef CFG_TUD_AUDIO_FUNC_1_N_FORMATS
#define CFG_TUD_AUDIO_FUNC_1_N_FORMATS                               1
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE
#define CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE                         DEFAULT_SAMPLE_RATE
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX                           SPEAK_CHANNEL_NUM
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX
#define CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX                           MIC_CHANNEL_NUM
#endif

// 16bit in 16bit slots
#ifndef CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX          2
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_TX                  16
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX          2
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_RESOLUTION_RX                  16
#endif

// EP and buffer size - for isochronous EP's, the buffer and EP size are equal
#ifndef CFG_TUD_AUDIO_ENABLE_EP_IN
#define CFG_TUD_AUDIO_ENABLE_EP_IN                1
#endif

// MIC
#ifndef CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN    ((CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE / 1000 * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_RX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_RX) + 4)
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SW_BUF_SZ      CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN * (MIC_INTERVAL_MS + 1)
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_IN_SZ_MAX         CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_IN
#endif

// EP and buffer size - for isochronous EP's, the buffer and EP size are equal
#ifndef CFG_TUD_AUDIO_ENABLE_EP_OUT
#define CFG_TUD_AUDIO_ENABLE_EP_OUT               1
#endif

// SPK +4 for audio feedback
#ifndef CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT
#define CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT   ((CFG_TUD_AUDIO_FUNC_1_MAX_SAMPLE_RATE / 1000 * CFG_TUD_AUDIO_FUNC_1_FORMAT_1_N_BYTES_PER_SAMPLE_TX * CFG_TUD_AUDIO_FUNC_1_N_CHANNELS_TX) + 4)
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SW_BUF_SZ     CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT * (SPK_INTERVAL_MS + 1)
#endif

#ifndef CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX
#define CFG_TUD_AUDIO_FUNC_1_EP_OUT_SZ_MAX        CFG_TUD_AUDIO_FUNC_1_FORMAT_1_EP_SZ_OUT
#endif

// Number of Standard AS Interface Descriptors (4.9.1) defined per audio function
#ifndef CFG_TUD_AUDIO_FUNC_1_N_AS_INT
#define CFG_TUD_AUDIO_FUNC_1_N_AS_INT             1
#endif

// Size of control request buffer
#ifndef CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ
#define CFG_TUD_AUDIO_FUNC_1_CTRL_BUF_SZ    64
#endif

#ifdef __cplusplus
}
#endif
