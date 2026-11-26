/* Own receive noise reduction (stock has none): short-time spectral noise reduction on the
 * 16 kHz audio of audio.c, see nr.c. Pure logic (no hardware), host-tested by `make test-nr`
 * (tools/nr_test.c). One sample in, one sample out, delayed by NR_DELAY samples. */
#ifndef NR_H
#define NR_H

#include <stdbool.h>
#include <stdint.h>

#define NR_FS        16000
#define NR_N         256                /* FFT frame: 16 ms, 62.5 Hz bins */
#define NR_HOP       (NR_N / 2)         /* 50 % overlap: one frame per 8 ms */
#define NR_BINS      (NR_N / 2 + 1)
#define NR_DELAY     (NR_N + NR_HOP)    /* input -> output delay in samples (24 ms) */
#define NR_LEVEL_MAX 10

typedef struct {
    float in[NR_N];             /* last NR_N input samples (frame) */
    float ola[NR_N];            /* overlap-add accumulator */
    float out[NR_HOP];          /* output of the current hop */
    float buf[NR_N];            /* FFT work buffer */
    float noise[NR_BINS];       /* noise power estimate per bin */
    float s_prev[NR_BINS];      /* clean power estimate of the previous frame (decision-directed) */
    float spp[NR_BINS];         /* smoothed speech presence probability (stagnation guard) */
    float gain[NR_BINS];        /* gains of the last frame */
    const float *ext_gain;      /* host test only: apply these gains (shadow filtering) */
    float gmin;                 /* gain floor (NR level) */
    float over;                 /* noise over-estimation factor (NR level) */
    unsigned pos;               /* samples into the current hop */
    unsigned slot;              /* next work slot of the frame in progress (nr.c) */
    unsigned frames;            /* frames since the last reset (noise estimate warm-up) */
    uint32_t faults;            /* non-finite state seen and reset (diagnostics) */
} nr_t;

void nr_init(void);                     /* shared tables, once before use */
void nr_reset(nr_t *s);                 /* everything incl. the noise estimate */
void nr_flush(nr_t *s);                 /* audio buffers only, keeps the noise estimate */
void nr_set_level(nr_t *s, int level);  /* 1..NR_LEVEL_MAX (strength) */
float nr_process(nr_t *s, float x);

#endif
