#!/bin/sh
# Re-export decompiled C from the existing project (no re-import/analysis).
# The MCP server must not hold the project open while this runs.
set -e
cd "$(dirname "$0")/../.."
"${GHIDRA_HOME:?set GHIDRA_HOME to the Ghidra install dir}"/support/analyzeHeadless "$PWD/re/ghidra" g106_stock \
    -process G106Fw-V1.4b01.txall.bin -noanalysis -readOnly \
    -scriptPath "$PWD/tools/ghidra" -postScript ExportDecomp.java "$PWD/re/stock_decomp.c"
