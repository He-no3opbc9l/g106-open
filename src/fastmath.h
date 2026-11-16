/* Fast log2 / exp2 (newlib's log10f/powf cost ~1000 cycles each here); ~1e-4 accuracy,
 * i.e. far below 0.01 dB in the AGC. Shared by audio.c (ISR) and cwdec.c. */
#ifndef FASTMATH_H
#define FASTMATH_H

#include <math.h>
#include <stdint.h>

static inline float fast_log2(float x)
{
    union { float f; uint32_t u; } v = {x};
    float e = (float)(int)((v.u >> 23) & 0xFF) - 127.0f;
    v.u = (v.u & 0x007FFFFF) | 0x3F800000;      /* mantissa in [1, 2) */
    float m = v.f;
    return e + (-2.49684606f + (4.02854794f + (-2.08121416f + (0.628873618f - 0.0791581614f * m) * m) * m) * m);
}

static inline float fast_exp2(float x)
{
    if (x < -126.0f)
        return 0.0f;
    float fi = floorf(x), f = x - fi;
    float p = 1.0f + f * (0.6951579f + f * (0.2262207f + f * 0.0781009f));
    union { uint32_t u; float f; } v = {(uint32_t)((int)fi + 127) << 23};
    return v.f * p;
}

#endif
