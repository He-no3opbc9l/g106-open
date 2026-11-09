#!/bin/sh
# Headless Ghidra (GHIDRA_HOME=...): import stock app (make stock), analyze, export decompiled C to re/stock_decomp.c
set -e
cd "$(dirname "$0")/../.."
G=${GHIDRA_HOME:?set GHIDRA_HOME to the Ghidra install dir}/support/analyzeHeadless
RE=$PWD/re
mkdir -p "$RE/ghidra"
"$G" "$RE/ghidra" g106_stock -overwrite \
    -import stock/G106Fw-V1.4b01.txall.bin \
    -loader BinaryLoader -loader-baseAddr 0x08020000 \
    -processor ARM:LE:32:Cortex -cspec default \
    -scriptPath "$PWD/tools/ghidra" \
    -preScript G106Setup.java "$RE" \
    -postScript ExportDecomp.java "$RE/stock_decomp.c" \
    -max-cpu 8
