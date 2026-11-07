// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file EspDspKernels.hpp
 * @brief The ESP32-S3's versions of crosspad-core's render kernels
 *        (crosspad/dsp/AudioKernels.hpp), registered with setAudioKernels().
 *
 * Built only for the S3 (this component's ESP32-S3 list): EE.LDF.128 /
 * EE.STF.128 (PIE) for the sends and gains, TRUNC.S and CLAMPS for the
 * int16 conversion. The PIE ones fall back to core's portable loop for a
 * buffer not on 16 bytes or a length not a multiple of four.
 */

#include <cstdint>

namespace crosspad {

void s3Mac(float* dst, const float* src, float g, uint32_t n);
void s3MacPanned(float* out, const float* mono, uint32_t frames, float gl, float gr);
void s3ScaleStereo(float* buf, uint32_t frames, float gl, float gr);
void s3FloatToInt16(const float* src, int16_t* dst, uint32_t n);

/// Registers the four with core; once, before the audio task starts.
void espDspKernelsRegister();

/// Whether espDspKernelsRegister() ran: what SIMD_MODE reports.
bool espDspKernelsRegistered();

} // namespace crosspad
