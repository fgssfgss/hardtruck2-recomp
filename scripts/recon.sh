#!/bin/sh
# Phase 0 + the reference catalog: everything under work/ that the lift needs
# before `run_lift.py catalog`. Idempotent; rerun after a pcrecomp update.
#
#   scripts/recon.sh            pe/rtti/vtable recon (seconds)
#   scripts/recon.sh --ghidra   + fresh Ghidra headless import/analysis (~10 min)
#
# Env: PCRECOMP (default ../pcrecomp), GHIDRA (install dir), JAVA_HOME.
set -eu
cd "$(dirname "$0")/.."
P=${PCRECOMP:-../pcrecomp}/tools
PY=${PY:-.venv/bin/python}
EXE=game/king.exe
mkdir -p work

$PY scripts/run_lift.py check
$PY $P/pe/pe_analyze.py $EXE --json work/pe_analysis.json > work/pe_analysis.txt
$PY $P/pe/analyze_sections.py $EXE > work/sections.txt
$PY $P/pe/extract_imports.py $EXE > work/imports.txt
$PY $P/pe/catalog.py game/ > work/install_catalog.txt
$PY $P/cpp/rtti.py $EXE -o work/rtti.json --seeds work/rtti_seeds.json --names work/rtti_names.json
$PY $P/cpp/vtable_scan.py $EXE --seeds work/vtable_seeds.json -o work/vtables.json

if [ "${1:-}" = --ghidra ]; then
  GHIDRA=${GHIDRA:-$HOME/Downloads/ghidra_12.1.2_PUBLIC}
  export JAVA_HOME=${JAVA_HOME:-/opt/openjdk-bin-25}
  rm -rf work/ghidra && mkdir -p work/ghidra
  "$GHIDRA/support/analyzeHeadless" "$PWD/work/ghidra" king -import $EXE -overwrite \
    -scriptPath "$P/ghidra" \
    -postScript DumpBounds.java "$PWD/work/ghidra_bounds.csv" \
    -postScript ExportFunctions.java "$PWD/work" > work/ghidra_analyze.log 2>&1
fi
if [ -f work/ghidra_bounds.csv ]; then
  $PY scripts/ghidra_ref.py work/ghidra_bounds.csv work/king_functions.txt $EXE
fi
