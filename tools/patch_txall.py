#!/usr/bin/env python3
"""Remove the amateur-band TX restriction from decrypted G106 V1.4b01 app.

usage: python3 -I tools/patch_txall.py IN.decrypted.bin OUT.bin

0x802ea30 is the "TX allowed on this VFO frequency?" check (walks the 10-entry
{lo,hi,flag} band table at 0x8038b3c). Replace its first instructions with
`movs r0,#1 ; bx lr` so it always reports "allowed".
"""
import sys

BASE = 0x08020000
FUNC = 0x0802EA30
ORIG = bytes.fromhex("b1f90220022a18bf")   # ldrsh r2,[r1,#2]; cmp r2,#2; it ne; cmpne r2,#0
PATCH = bytes.fromhex("01207047")          # movs r0,#1 ; bx lr

d = bytearray(open(sys.argv[1], "rb").read())
off = FUNC - BASE
assert bytes(d[off:off + 8]) == ORIG, "unexpected bytes at patch site (wrong firmware?)"
d[off:off + 4] = PATCH
open(sys.argv[2], "wb").write(d)
print("patched %#x: %s -> %s" % (FUNC, ORIG[:4].hex(), PATCH.hex()))
