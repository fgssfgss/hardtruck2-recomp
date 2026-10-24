#!/usr/bin/env python3
"""Which of king.exe's imports have a shim, and where.

  import_coverage.py [--missing]

Scans the `{ "Name", fn }` shim tables in pcrecomp runtime/win32hle/*.c and in
src/shims/*.c (ours win: they are registered first), and matches them against
king.exe's import table the way win32hle's pe_loader binds (by name, an ordinal
as "dll#N" lower-case). Prints a per-DLL table; --missing lists only the gaps.
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
sys.path.insert(0, os.path.join(ROOT, "scripts"))
import run_lift  # noqa: E402  (config + pcrecomp path)

SHIM_RE = re.compile(r'\{\s*"([^"]+)"\s*,\s*[A-Za-z_]\w*\s*\}')
MACRO_RE = re.compile(r"#define\s+(\w+)\(\s*ord\s*,\s*name\s*,\s*fn\s*\)\s*\{\s*\"([^\"]+#)\"\s*#ord")


def shim_names(pattern):
    out = {}
    for path in sorted(glob.glob(pattern)):
        if path.endswith("_selftest.c"):
            continue
        src = open(path, errors="replace").read()
        for m in SHIM_RE.finditer(src):
            out.setdefault(m.group(1), os.path.basename(path))
        # Ordinal+name macros: #define SHIM2(ord, name, fn) { "wsock32.dll#" #ord, fn }, { name, fn }
        for mac, prefix in MACRO_RE.findall(src):
            for ordn, name in re.findall(rf'\b{mac}\(\s*(\d+)\s*,\s*"([^"]+)"', src):
                out.setdefault(f"{prefix}{ordn}", os.path.basename(path))
                out.setdefault(name, os.path.basename(path))
    return out


def main():
    import pefile
    pe = pefile.PE(run_lift.EXE)
    ours = shim_names(os.path.join(ROOT, "src", "shims", "*.c"))
    hle = shim_names(os.path.join(run_lift.PCRECOMP, "runtime", "win32hle", "*.c"))
    missing_only = "--missing" in sys.argv
    tot = collections.Counter()
    for imp in pe.DIRECTORY_ENTRY_IMPORT:
        dll = imp.dll.decode()
        rows = []
        for s in imp.imports:
            byord = getattr(s, "import_by_ordinal", not s.name)
            name = f"{dll.lower()}#{s.ordinal}" if byord else s.name.decode()
            where = ours.get(name) and f"src/shims/{ours[name]}" or \
                hle.get(name) and f"win32hle/{hle[name]}" or None
            tot["ours" if name in ours else "hle" if name in hle else "missing"] += 1
            if where is None or not missing_only:
                rows.append((name, where or "-- MISSING --"))
        if rows:
            print(f"\n{dll} ({len(imp.imports)})")
            for name, where in rows:
                print(f"    {name:36s} {where}")
    print(f"\n[*] {sum(tot.values())} imports: {tot['ours']} src/shims, "
          f"{tot['hle']} win32hle, {tot['missing']} missing")


if __name__ == "__main__":
    main()
