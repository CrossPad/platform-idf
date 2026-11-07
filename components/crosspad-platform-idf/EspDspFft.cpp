// SPDX-License-Identifier: GPL-3.0-or-later

#include "EspDspFft.hpp"

#include <esp_dsp.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace crosspad {

namespace {

/// Largest transform this build can serve: the library refuses a table bigger
/// than the CONFIG_DSP_MAX_FFT_SIZE it was compiled for, and an Arduino core
/// ships that value prebuilt. Rounded down to a power of two so a
/// non-power-of-two Kconfig value cannot produce a table nothing can use.
uint32_t tableSize() {
    uint32_t want = static_cast<uint32_t>(CONFIG_DSP_MAX_FFT_SIZE);
    if (want > kMaxFftSize) want = kMaxFftSize;
    if (want < kMinFftSize) return 0;
    uint32_t s = kMinFftSize;
    while ((s << 1) <= want) s <<= 1;
    return s;
}

void logf(const FftMemory& mem, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void logf(const FftMemory& mem, const char* fmt, ...) {
    if (!mem.log) return;
    char line[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    mem.log(line);
}

/// esp-dsp's twiddle table is a process-wide global, so it is built once.
bool initTable(const FftMemory& mem) {
    const uint32_t n = tableSize();
    if (n == 0) {
        logf(mem, "EspDspFft: esp-dsp built for FFT <= %d", (int)CONFIG_DSP_MAX_FFT_SIZE);
        return false;
    }
    if (!mem.alloc || !mem.free) {
        logf(mem, "EspDspFft: no allocator for the twiddle table");
        return false;
    }
    // Ours, not esp-dsp's: left to itself the library memaligns the table in
    // internal RAM, and 32 kB of it is more than the launcher has to spare.
    // The caller's allocator puts it in PSRAM, which is fine for the aes3
    // kernel — on the S3 the library's own default table for N <= 1024 is a
    // const in flash, which is external memory too.
    float* table = static_cast<float*>(mem.alloc(sizeof(float) * n, 16));
    if (!table) {
        logf(mem, "EspDspFft: no memory for the %u-point twiddle table", (unsigned)n);
        return false;
    }
    const int err = (int)dsps_fft2r_init_fc32(table, (int)n);
    if (err != 0) {
        logf(mem, "EspDspFft: dsps_fft2r_init_fc32 failed (%d)", err);
        mem.free(table);
        return false;
    }
    logf(mem, "EspDspFft: twiddle table ready (%u points)", (unsigned)n);
    return true;
}

/// True once the table exists, building it on the first call that finds it
/// missing. **A failure must not latch.** The only way initTable() fails at
/// runtime is 32 kB of memory not being there, and it is scarcest during a
/// kit load — exactly when the first caller arrives. A `static const bool`
/// would freeze that transient into "this board has no FFT" for the life of
/// the process, so every later load fails with "no FFT" against a heap that
/// has since freed the room. Hence the explicit mutex: the kit-load worker and
/// the CDC bench task can both be first, and either may retry.
bool tableReady(const FftMemory& mem) {
    static std::mutex m;
    static bool ok = false;
    std::lock_guard<std::mutex> lock(m);
    if (!ok) ok = initTable(mem);
    return ok;
}

inline bool aligned16(const void* p) {
    return (reinterpret_cast<uintptr_t>(p) & 0xF) == 0;
}

} // namespace

EspDspFft::EspDspFft(const FftMemory& mem) : mem_(mem) {}

uint32_t EspDspFft::maxSize() const {
    return tableSize();
}

bool EspDspFft::available() {
    return tableReady(mem_);
}

bool EspDspFft::run(float* data, uint32_t n) {
    if (aligned16(data)) {
        // dsps_bit_rev2r_fc32 has lookup tables for 16..4096 and falls back to
        // the generic in-place reversal above that, so it covers 8192 too.
        if (dsps_fft2r_fc32(data, (int)n) != ESP_OK) return false;
        return dsps_bit_rev2r_fc32(data, (int)n) == ESP_OK;
    }
    // Scalar kernels for an unaligned buffer — see the header for why this is
    // a fallback and not a copy.
    if (dsps_fft2r_fc32_ansi_(data, (int)n, dsps_fft_w_table_fc32) != ESP_OK) return false;
    return dsps_bit_rev_fc32_ansi(data, (int)n) == ESP_OK;
}

bool EspDspFft::forward(float* data, uint32_t n) {
    if (!data || !fftSizeValid(n) || n > maxSize()) return false;
    if (!tableReady(mem_)) return false;
    return run(data, n);
}

bool EspDspFft::inverse(float* data, uint32_t n) {
    if (!data || !fftSizeValid(n) || n > maxSize()) return false;

    for (uint32_t i = 0; i < n; ++i) data[2 * i + 1] = -data[2 * i + 1];
    if (!forward(data, n)) {
        // Undo the conjugate. forward() can refuse without touching the buffer
        // (no twiddle table), and a caller that retries — or that treats its
        // buffer as unchanged after a failure, as FftResampler's overlap-add
        // does — would otherwise be handed a silently mirrored spectrum.
        for (uint32_t i = 0; i < n; ++i) data[2 * i + 1] = -data[2 * i + 1];
        return false;
    }

    const float scale = 1.0f / static_cast<float>(n);
    for (uint32_t i = 0; i < n; ++i) {
        data[2 * i]     *= scale;
        data[2 * i + 1] *= -scale;
    }
    return true;
}

IFft* espDspFftBackend(const FftMemory& mem) {
    // Built with the memory of the *first* caller; a later caller's allocator
    // is ignored, since the table esp-dsp keeps is a single global anyway.
    static EspDspFft esp(mem);
    return esp.available() ? &esp : nullptr;
}

} // namespace crosspad
