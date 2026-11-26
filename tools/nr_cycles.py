#!/usr/bin/env python3
"""Cortex-M4 cycle estimate for tools/nr_bench.c (see there): ELF + qemu exec trace ->
cycles per nr_process() call (max = the heaviest work slot) and per frame / sample."""
import collections
import re
import subprocess
import sys

FRAMES, HOP = 20, 128
elf, log = sys.argv[1], sys.argv[2]

syms = {}
for line in subprocess.run(['arm-none-eabi-nm', elf], capture_output=True, text=True).stdout.split('\n'):
    p = line.split()
    if len(p) == 3:
        syms[p[2]] = int(p[0], 16) & ~1
ins = {}
dis = subprocess.run(['arm-none-eabi-objdump', '-d', elf], capture_output=True, text=True).stdout
for line in dis.split('\n'):
    m = re.match(r'\s*([0-9a-f]+):\s+(?:[0-9a-f]{4}\s?){1,2}\s+(\S+)\s*(.*)', line)
    if m:
        ins[int(m.group(1), 16)] = (m.group(2).split('.')[0], m.group(3))


def cost(pc, nxt):
    mn, ops = ins.get(pc, ('?', ''))
    if mn in ('vdiv', 'vsqrt'):
        return 14
    if mn in ('vfma', 'vfms', 'vmla', 'vmls', 'vnmla', 'vnmls', 'vfnma', 'vfnms'):
        return 3
    if mn in ('vldr', 'vstr'):
        return 2
    if mn in ('vldmia', 'vstmia', 'vldmdb', 'vstmdb', 'vpush', 'vpop'):
        return 1 + max(1, len(re.findall(r's\d+', ops)))
    if mn.startswith('v'):
        return 1
    if mn in ('ldr', 'ldrb', 'ldrh', 'ldrsh', 'ldrd', 'strd'):
        return 2
    if mn in ('push', 'pop', 'ldmia', 'stmia', 'ldm', 'stm', 'stmdb'):
        return 1 + len(re.findall(r'r\d+|lr|pc|ip|fp|sl', ops))
    if mn.startswith('b') or mn in ('cbz', 'cbnz'):
        return 3 if nxt not in (pc + 2, pc + 4) else 1
    return 1


pcs, started = [], False
for line in open(log):
    if line.startswith('Trace'):
        pc = int(re.search(r'/([0-9a-f]{8,16})/', line).group(1), 16)
        started = started or pc == syms['bench_mark']
        if started:
            pcs.append(pc)
calls, cur, total = [], None, 0
for i, pc in enumerate(pcs):
    c = cost(pc, pcs[i + 1] if i + 1 < len(pcs) else 0)
    total += c
    if pc == syms['nr_process']:
        if cur is not None:
            calls.append(cur)
        cur = 0
    if cur is not None:
        cur += c
calls.append(cur)
heavy = collections.Counter(c for c in calls if c > 500)
print(f"{len(pcs) / FRAMES:.0f} instructions, ~{total / FRAMES:.0f} cycles per frame "
      f"= ~{total / FRAMES / HOP:.0f} cycles per 16 kHz sample = ~{total / FRAMES / HOP * 16000 / 168e6 * 100:.1f} % "
      f"of 168 MHz")
print(f"light call ~{sorted(calls)[len(calls) // 2]} cycles; work slots (cycles): "
      + ", ".join(str(c) for c in sorted(heavy)))
print(f"heaviest call ~{max(calls)} cycles = {max(calls) / 112000 * 100:.1f} % of one DSP interrupt period "
      "(32 frames at 48 kHz = 112k cycles)")
