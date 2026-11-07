// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

/**
 * @file EspDspFft.hpp
 * @brief crosspad::IFft on top of esp-dsp's radix-2 complex FFT.
 *
 * Built only where esp-dsp is (the ESP32-S3 list of this component). Nothing
 * outside names the class: the platform registers espDspFftBackend() with
 * crosspad-core's setFftBackend(), and `createFft()` in dsp/FftFactory.hpp is
 * how a caller asks for an FFT.
 *
 * The ESP32-S3 build of esp-dsp resolves `dsps_fft2r_fc32` to the `aes3`
 * assembly, which moves complex pairs with `ee.ldf.64` / `ee.stf.64`. Those
 * need the buffer 8-byte aligned; **this class takes that path only at 16**,
 * the alignment an aligned allocator is asked for, so the same rule would
 * cover a future 128-bit variant.
 *
 * A caller's buffer that is not 16-byte aligned is **not rejected**: the
 * transform runs through esp-dsp's scalar (`_ansi`) kernels instead, which
 * have no alignment requirement. That matters because crosspad-core's
 * PitchDetector works out of a `std::vector<float>`, whose allocation the
 * heap only guarantees to 4 bytes. Bouncing it through an aligned copy is the
 * wrong trade: the copy is another 64 kB of PSRAM at exactly the moment PSRAM
 * is scarcest — a kit load, with the zones already resident — and a failed
 * allocation then surfaces as "pitch detection failed". Scalar is a few
 * milliseconds slower, once per zone, at load time.
 *
 * The twiddle table is esp-dsp's single global: it is built once, for
 * maxSize() points, out of the caller's memory, and kept for the life of the
 * process. **maxSize() is decided at runtime**, as
 * min(kMaxFftSize, CONFIG_DSP_MAX_FFT_SIZE) — a prebuilt Arduino sdkconfig
 * may say 4096 where an IDF project asks for 8192, and the library refuses a
 * table larger than it was compiled for. Callers that choose a transform size
 * read maxSize() rather than assume.
 *
 * A table build that *fails* is retried on the next call rather than
 * remembered: the failure mode is 32 kB of PSRAM missing at a moment when a
 * kit load has it, and that moment passes.
 *
 * One table serves every smaller power of two: esp-dsp bit-reverses the table
 * over N_max/2 points, and for a j below N/2 the reversal in order_max bits is
 * the reversal in order_N bits scaled by 2^(order_max-order_N) — which is
 * exactly the factor between the two angle grids. Verified against
 * dsps_gen_w_r2_fc32() + dsps_bit_rev_fc32_ansi(); `PITCHED_BENCH`'s
 * `fft_maxdiff` row checks it on the device against PortableFft.
 */

#include "crosspad/dsp/FftFactory.hpp"
#include "crosspad/dsp/IFft.hpp"

namespace crosspad {

class EspDspFft : public IFft {
public:
    /// @param mem where the twiddle table comes from, and where table
    ///        diagnostics go. Copied; must describe memory valid for the life
    ///        of the process.
    explicit EspDspFft(const FftMemory& mem);
    ~EspDspFft() override = default;

    EspDspFft(const EspDspFft&) = delete;
    EspDspFft& operator=(const EspDspFft&) = delete;

    /// Forward transform, in place, no scaling.
    bool forward(float* data, uint32_t n) override;

    /// Inverse transform, in place, including the 1/n scale. Implemented as
    /// conjugate → forward → conjugate → scale, so there is one kernel.
    bool inverse(float* data, uint32_t n) override;

    /// min(kMaxFftSize, CONFIG_DSP_MAX_FFT_SIZE), the size the shared table is
    /// built for. Constant for the life of the process.
    uint32_t maxSize() const override;

    /// True once the shared twiddle table exists, building it if it does not.
    /// False means every call to forward()/inverse() will fail — the table's
    /// memory was not there. Retry-able: see the file comment.
    bool available();

private:
    /// Forward transform, picking the kernel from @p data's alignment.
    bool run(float* data, uint32_t n);

    FftMemory mem_;
};

/// The FftBackend this platform registers: the one EspDspFft, built with the
/// memory of the first caller, while its twiddle table exists; nullptr while
/// it does not, so createFft() falls back to PortableFft and asks again later.
IFft* espDspFftBackend(const FftMemory& mem);

} // namespace crosspad
