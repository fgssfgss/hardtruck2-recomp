#!/usr/bin/env python3
"""Turn Ghidra's headless export into the three files the pipeline reads.

  ghidra_ref.py <bounds.csv> <king_functions.txt> <exe>

  work/ghidra_ref.json    {"functions": [{"start", "end"}]} -- the reference
                          for pcrecomp score_recovery.py; ends walked back over
                          0xCC/0x90 padding (score_recovery.py explains why)
  work/ghidra_seeds.json  every Ghidra function start, for disasm32 seeding
  work/names.json         {"0xVA": name} for names that are not FUN_/thunk_FUN_
                          (Ghidra's FID matches: MFC 4.2, the VC6 CRT), plus
                          RTTI names from work/rtti_names.json where Ghidra had none

Ghidra is a second opinion, not ground truth (no symbols shipped with king.exe).
"""
import json
import os
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))


def main(bounds, funcs_txt, exe):
    import pefile
    pe = pefile.PE(exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    ref = []
    for line in open(bounds):
        s, e = (int(x, 16) for x in line.strip().split(","))
        while e > s + 1 and img[e - 1 - base] in (0xCC, 0x90):
            e -= 1
        ref.append({"start": s, "end": e})
    json.dump({"functions": ref}, open(os.path.join(ROOT, "work/ghidra_ref.json"), "w"))
    json.dump([f"0x{r['start']:08X}" for r in ref],
              open(os.path.join(ROOT, "work/ghidra_seeds.json"), "w"))

    names = {}
    for line in open(funcs_txt):
        if line.startswith("#") or "|" not in line:
            continue
        cols = [c.strip() for c in line.split("|")]
        va, name = int(cols[0], 16), cols[-1]
        if not name.startswith(("FUN_", "thunk_FUN_")):
            names[f"0x{va:08X}"] = name
    rtti = os.path.join(ROOT, "work/rtti_names.json")
    if os.path.exists(rtti):
        for k, v in json.load(open(rtti)).items():
            names.setdefault(f"0x{int(k, 0):08X}", v)
    json.dump(names, open(os.path.join(ROOT, "work/names.json"), "w"), indent=0, sort_keys=True)
    print(f"[*] {len(ref)} reference functions, {len(names)} names")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
