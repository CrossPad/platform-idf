// SPDX-License-Identifier: GPL-3.0-or-later
#include "EspDspKernels.hpp"

#include "crosspad/dsp/AudioKernels.hpp"

namespace crosspad {

namespace {

/* Four floats, lowest address in the last operand. The memory clobber keeps
 * GCC from moving a plain load or store of the same buffer across them. */
#define CP_LDF4(p, x0, x1, x2, x3) \
    asm volatile("ee.ldf.128.ip %0, %1, %2, %3, %4, 16" \
                 : "=f"(x3), "=f"(x2), "=f"(x1), "=f"(x0), "+r"(p) :: "memory")
#define CP_STF4(p, x0, x1, x2, x3) \
    asm volatile("ee.stf.128.ip %1, %2, %3, %4, %0, 16" \
                 : "+r"(p) : "f"(x3), "f"(x2), "f"(x1), "f"(x0) : "memory")

void pieMac(float* dst, const float* src, float g, uint32_t n)
{
    const float* d = dst;
    for (uint32_t i = 0; i < n; i += 4) {
        float s0, s1, s2, s3, d0, d1, d2, d3;
        CP_LDF4(src, s0, s1, s2, s3);
        CP_LDF4(d, d0, d1, d2, d3);
        d0 += s0 * g; d1 += s1 * g; d2 += s2 * g; d3 += s3 * g;
        CP_STF4(dst, d0, d1, d2, d3);
    }
}

/* Four mono frames against eight interleaved ones a turn. */
void pieMacPanned(float* out, const float* mono, uint32_t frames, float gl, float gr)
{
    const float* o = out;
    for (uint32_t i = 0; i < frames; i += 4) {
        float m0, m1, m2, m3, l0, r0, l1, r1, l2, r2, l3, r3;
        CP_LDF4(mono, m0, m1, m2, m3);
        CP_LDF4(o, l0, r0, l1, r1);
        CP_LDF4(o, l2, r2, l3, r3);
        l0 += m0 * gl; r0 += m0 * gr; l1 += m1 * gl; r1 += m1 * gr;
        l2 += m2 * gl; r2 += m2 * gr; l3 += m3 * gl; r3 += m3 * gr;
        CP_STF4(out, l0, r0, l1, r1);
        CP_STF4(out, l2, r2, l3, r3);
    }
}

void pieScaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
    const float* s = buf;
    for (uint32_t i = 0; i < frames; i += 2) {
        float l0, r0, l1, r1;
        CP_LDF4(s, l0, r0, l1, r1);
        l0 *= gl; r0 *= gr; l1 *= gl; r1 *= gr;
        CP_STF4(buf, l0, r0, l1, r1);
    }
}

#undef CP_LDF4
#undef CP_STF4

/* The two steps the S3 does in one instruction each, which GCC would spend a
 * compare and a branch a sample on: TRUNC.S, which saturates and turns a NaN
 * into INT_MAX, and CLAMPS, the clamp to int16. */
inline int truncSatAsm(float y)
{
    int t;
    asm("trunc.s %0, %1, 0" : "=r"(t) : "f"(y));
    return t;
}

inline int clampS16Asm(int t)
{
    int r;
    asm("clamps %0, %1, 15" : "=r"(r) : "r"(t));
    return r;
}

bool s_registered = false;

}  // namespace

void s3Mac(float* dst, const float* src, float g, uint32_t n)
{
    if (dsp::quadAligned(n, dst, src)) { pieMac(dst, src, g, n); return; }
    dsp::fast::mac(dst, src, g, n);
}

void s3MacPanned(float* out, const float* mono, uint32_t frames, float gl, float gr)
{
    if (dsp::quadAligned(frames, out, mono)) { pieMacPanned(out, mono, frames, gl, gr); return; }
    dsp::fast::macPanned(out, mono, frames, gl, gr);
}

void s3ScaleStereo(float* buf, uint32_t frames, float gl, float gr)
{
    if (dsp::quadAligned(frames * 2, buf)) { pieScaleStereo(buf, frames, gl, gr); return; }
    dsp::fast::scaleStereo(buf, frames, gl, gr);
}

void s3FloatToInt16(const float* src, int16_t* dst, uint32_t n)
{
    dsp::fast::floatToInt16With(src, dst, n, truncSatAsm, clampS16Asm);
}

void espDspKernelsRegister()
{
    dsp::AudioKernelTable t;
    t.mac          = s3Mac;
    t.macPanned    = s3MacPanned;
    t.scaleStereo  = s3ScaleStereo;
    t.floatToInt16 = s3FloatToInt16;
    dsp::setAudioKernels(&t);
    s_registered = true;
}

bool espDspKernelsRegistered() { return s_registered; }

} // namespace crosspad
