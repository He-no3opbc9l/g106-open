/* Cortex-M4 cost estimate of src/nr.c (`make bench-nr`, needs qemu-arm): built for the M4 like
 * the firmware, run as a bare Linux user binary under qemu-arm with an instruction trace;
 * tools/nr_cycles.py weights the executed instructions with Cortex-M4 cycle counts (FPU loads 2,
 * VFMA 3, VDIV 14, taken branches 3, ...) per nr_process() call and per frame. */
#include "nr.h"

#define FRAMES 20

static nr_t st;
volatile float sink;

void bench(void)
{
    nr_init();
    nr_set_level(&st, 5);
    nr_reset(&st);
    unsigned r = 1;
    for (int i = 0; i < 16 * NR_HOP; i++) {     /* past the warm-up, noise-like input */
        r = r * 1103515245u + 12345u;
        sink = nr_process(&st, (int)(r >> 8) * 1e-9f);
    }
    __asm volatile("bench_mark: nop");          /* measured part starts here */
    for (int i = 0; i < FRAMES * NR_HOP; i++) {
        r = r * 1103515245u + 12345u;
        sink = nr_process(&st, (int)(r >> 8) * 1e-9f);
    }
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm volatile("bl bench\n mov r0, #0\n mov r7, #1\n svc 0\n");    /* Linux exit(0) */
}
