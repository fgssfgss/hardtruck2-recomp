#!/usr/bin/env python3
"""Hard Truck 2 (king.exe) lift driver -- pcrecomp lift32, global-register model.

  run_lift.py check
      Verify king.exe is the build config/hardtruck2.toml describes.

  run_lift.py catalog
      Recursive descent (pcrecomp disasm32), seeded with the entry point and
      every [catalog].seed_files list that exists, to a catalog that keeps each
      function's BASIC BLOCK ranges, so `lift` can re-decode without paying for
      discovery again.

  run_lift.py lift [--only 0xVA,...]
      Lift every catalogued function with lift32.Lifter into [lift].out_dir:
        recomp_NNNN.c        the functions, [lift].split per file
        recomp_funcs.h       declarations
        recomp_dispatch.c    sorted VA -> function table (binary search)
        recomp_imports.c     IAT slot VA -> "dll!name" (crash trails)
        lift_stats.json      counts for docs/STATUS.md

  run_lift.py all
      check + catalog + lift.

Config: config/hardtruck2.toml. pcrecomp: $PCRECOMP or [pcrecomp].path.
Everything this writes is derived from the game binary and is gitignored.
"""
import collections
import hashlib
import json
import os
import subprocess
import sys
import time
import tomllib

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CFG = tomllib.load(open(os.path.join(ROOT, "config", "hardtruck2.toml"), "rb"))


def rp(p):
    """A config path, relative to the repo root unless absolute."""
    return p if os.path.isabs(p) else os.path.join(ROOT, p)


PCRECOMP = os.path.normpath(os.environ.get("PCRECOMP") or rp(CFG["pcrecomp"]["path"]))
for sub in ("pe", "disasm", "lift"):
    sys.path.insert(0, os.path.join(PCRECOMP, "tools", sub))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map   # noqa: E402
from disasm32 import Disassembler, Function, BasicBlock, Instruction  # noqa: E402

EXE = rp(CFG["game"]["exe"])


def cmd_check():
    data = open(EXE, "rb").read()
    h = hashlib.sha256(data).hexdigest()
    if h != CFG["game"]["sha256"]:
        sys.exit(f"[!] {EXE}: sha256 {h}\n    expected {CFG['game']['sha256']} -- "
                 "a different king.exe build needs its own config (AGENTS.md)")
    print(f"[*] {EXE}: the documented build ({len(data):,} bytes)")


def read_va_list(path):
    """'0xADDR rest-of-line' per line, '#' comments. -> {va: text}"""
    out = {}
    if path and os.path.exists(path):
        for line in open(path):
            line = line.split("#", 1)[0].strip()
            if line:
                parts = line.split(None, 1)
                out[int(parts[0], 16)] = parts[1] if len(parts) > 1 else ""
    return out


def load_seeds():
    seeds = set()
    for p in CFG["catalog"]["seed_files"]:
        p = rp(p)
        if not os.path.exists(p):
            print(f"[ ] seed file {os.path.relpath(p, ROOT)} not there, skipped")
            continue
        n0 = len(seeds)
        for e in json.load(open(p)):
            a = e.get("address") if isinstance(e, dict) else e
            seeds.add(int(a, 0) if isinstance(a, str) else a)
        print(f"[*] {len(seeds) - n0:6d} new seeds from {os.path.relpath(p, ROOT)}")
    extra = read_va_list(rp(CFG["catalog"]["extra_seeds"]))
    seeds |= set(extra)
    if extra:
        print(f"[*] {len(extra):6d} seeds from {CFG['catalog']['extra_seeds']}")
    return seeds


def cmd_catalog():
    info = analyze_pe(EXE)
    iat = build_iat_map(info)
    data = open(EXE, "rb").read()
    seeds = {info.image_base + info.entry_point_rva} | load_seeds()
    # Seeds outside .text (data decoded as code by a seed tool) are dropped.
    seeds = {s for s in seeds if info.code_start <= s < info.code_end}
    dis = Disassembler(data, info.image_base, info.sections)
    t = time.time()
    funcs = dis.find_functions(info.code_start, info.code_end, iat, seeds=seeds,
                               release_operands=True)
    print(f"[*] {len(funcs)} functions in {time.time() - t:.0f}s")
    doc = {"exe": os.path.basename(EXE), "image_base": info.image_base,
           "code_start": info.code_start, "code_end": info.code_end,
           "functions": []}
    for f in sorted(funcs.values(), key=lambda f: f.address):
        doc["functions"].append({
            "address": f.address, "end": f.end, "size": f.size,
            "entry_kind": f.entry_kind, "is_thunk": f.is_thunk,
            "jump_targets": sorted(f.jump_targets),
            "calls_to": sorted(f.calls_to),
            "blocks": [[b.start, b.end, sorted(set(b.successors)), int(b.is_exit)]
                       for b in sorted(f.blocks.values(), key=lambda b: b.start)],
        })
    out = rp(CFG["catalog"]["out"])
    json.dump(doc, open(out, "w"))
    print(f"[*] wrote {os.path.relpath(out, ROOT)}")


def rebuild(md, img, base, fd):
    """A disasm32.Function with full capstone operands, from a catalog entry."""
    f = Function(address=fd["address"], end=fd["end"], size=fd["size"],
                 name=f"sub_{fd['address']:08X}", is_thunk=fd["is_thunk"],
                 entry_kind=fd["entry_kind"])
    f.jump_targets = set(fd["jump_targets"])
    f.calls_to = set(fd["calls_to"])
    for s, e, succ, ex in fd["blocks"]:
        b = BasicBlock(start=s, end=e, successors=list(succ), is_exit=bool(ex))
        for ins in md.disasm(img[s - base:e - base], s):
            b.instructions.append(Instruction(ins.address, ins.size, ins.mnemonic,
                                              ins.op_str, bytes(ins.bytes),
                                              list(ins.operands)))
        f.blocks[s] = b
    return f


def pcrecomp_rev():
    try:
        return subprocess.check_output(["git", "-C", PCRECOMP, "describe", "--always", "--dirty"],
                                       text=True).strip()
    except (OSError, subprocess.CalledProcessError):
        return "?"


def cmd_lift(only=None):
    import pefile
    from lift32 import Lifter
    L = CFG["lift"]
    info = analyze_pe(EXE)
    iat = build_iat_map(info)
    pe = pefile.PE(EXE, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    fds = json.load(open(rp(CFG["catalog"]["out"])))["functions"]
    lifted = {fd["address"] for fd in fds}
    names = {}
    if os.path.exists(rp(L["names"])):
        names = {int(k, 16): v for k, v in json.load(open(rp(L["names"]))).items()}
    overrides = read_va_list(rp(L["overrides"]))
    excluded = read_va_list(rp(L["exclude"]))
    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    lifter = Lifter(iat_map=iat, func_names={}, lifted=lifted,
                    precise_carry=L["precise_carry"])
    outdir = rp(L["out_dir"])
    os.makedirs(outdir, exist_ok=True)
    if only:
        fds = [fd for fd in fds if fd["address"] in only]
    else:
        for fn in os.listdir(outdir):
            if fn.startswith("recomp_") and fn.endswith((".c", ".h", ".json")):
                os.remove(os.path.join(outdir, fn))

    stats = collections.Counter()
    unimpl = collections.Counter()
    unimpl_sites = collections.defaultdict(list)
    failed, entries = [], []
    t0 = time.time()
    chunk, idx, lines_total = [], 0, 0
    split = L["split"]

    def flush():
        nonlocal chunk, idx, lines_total
        if not chunk:
            return
        name = "recomp_only.c" if only else f"recomp_{idx:04d}.c"
        with open(os.path.join(outdir, name), "w", newline="\n") as fh:
            fh.write("/* AUTO-GENERATED from king.exe by scripts/run_lift.py (pcrecomp lift32)."
                     " DO NOT EDIT, DO NOT COMMIT. */\n")
            fh.write('#define RECOMP_GENERATED_CODE\n#include "recomp_types.h"\n'
                     '#include "recomp_funcs.h"\n#include <math.h>\n#include <string.h>\n\n')
            body = "\n\n".join(chunk)
            fh.write(body)
            lines_total += body.count("\n")
        idx += 1
        chunk = []

    for n, fd in enumerate(fds):
        a = fd["address"]
        try:
            if a in overrides:
                code = (f"/* OVERRIDE: {overrides[a]} */\n"
                        f"void sub_{a:08X}(void) {{ recomp_func_t _f = recomp_lookup_manual(0x{a:08X}u);"
                        f" if (_f) {{ _f(); return; }} RECOMP_UNIMPL(0x{a:08X}u, \"override missing\"); }}")
                stats["overridden"] += 1
            elif a in excluded:
                code = (f"/* EXCLUDED: {excluded[a]} */\n"
                        f"void sub_{a:08X}(void) {{ RECOMP_UNIMPL(0x{a:08X}u, \"excluded: not code\"); }}")
                stats["excluded"] += 1
            else:
                code = lifter.lift_function(rebuild(md, img, base, fd))
        except Exception as ex:  # noqa: BLE001 -- one bad function must not stop the lift
            failed.append((f"0x{a:08X}", repr(ex)))
            code = (f"/* LIFT FAILED: {ex!r} */\n"
                    f"void sub_{a:08X}(void) {{ RECOMP_UNIMPL(0x{a:08X}u, \"lift failed\"); }}")
        for line in code.splitlines():
            if "RECOMP_UNIMPL(" in line:
                key = line.split("RECOMP_UNIMPL(", 1)[1].split('"')[1].split()[0] if '"' in line else "?"
                unimpl[key] += 1
                if len(unimpl_sites[key]) < 10:
                    unimpl_sites[key].append(f"{a:08X}")
            if "RECOMP_ICALL(" in line:
                stats["icall_sites"] += 1
            if "RECOMP_ITAIL(" in line:
                stats["itail_sites"] += 1
        if names.get(a):
            code = f"/* {names[a]} */\n" + code
        chunk.append(code)
        entries.append(a)
        if len(chunk) >= split:
            flush()
            print(f"[*] {n + 1}/{len(fds)} lifted ({time.time() - t0:.0f}s)", flush=True)
    flush()
    if only:
        print(f"[*] {len(entries)} function(s) -> {outdir}/recomp_only.c, failures: {failed}")
        return

    with open(os.path.join(outdir, "recomp_funcs.h"), "w", newline="\n") as fh:
        fh.write("/* AUTO-GENERATED. DO NOT EDIT. */\n#pragma once\n#include <stdint.h>\n")
        for a in entries:
            fh.write(f"void sub_{a:08X}(void);\n")
    with open(os.path.join(outdir, "recomp_dispatch.c"), "w", newline="\n") as fh:
        fh.write('/* AUTO-GENERATED. DO NOT EDIT. */\n#include "recomp_types.h"\n'
                 '#include "recomp_funcs.h"\n\n')
        fh.write("const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n")
        for a in sorted(entries):
            fh.write(f"    {{ 0x{a:08X}u, sub_{a:08X} }},\n")
        fh.write("};\n")
        fh.write(f"const uint32_t recomp_dispatch_count = {len(entries)};\n")
    with open(os.path.join(outdir, "recomp_imports.c"), "w", newline="\n") as fh:
        fh.write("/* AUTO-GENERATED: IAT slot -> import name, for diagnostics. */\n#include <stdint.h>\n")
        fh.write("typedef struct { uint32_t slot; const char *name; } ht2_import_name_t;\n")
        fh.write("const ht2_import_name_t ht2_import_names[] = {\n")
        for slot, (dll, nm) in sorted(iat.items()):
            fh.write(f'    {{ 0x{slot:08X}u, "{dll}!{nm}" }},\n')
        fh.write("    { 0, 0 } };\n")

    out = {"pcrecomp": pcrecomp_rev(), "functions": len(entries), "files": idx,
           "c_lines": lines_total, "lift_failures": len(failed), "failures": failed[:200],
           "unimplemented_total": sum(unimpl.values()),
           "unimplemented": dict(unimpl.most_common()),
           "unimplemented_sites": dict(unimpl_sites),
           "overridden": stats["overridden"], "excluded": stats["excluded"],
           "icall_sites": stats["icall_sites"], "itail_sites": stats["itail_sites"],
           "seconds": round(time.time() - t0)}
    json.dump(out, open(os.path.join(outdir, "lift_stats.json"), "w"), indent=1)
    json.dump(out, open(rp("work/lift_stats.json"), "w"), indent=1)
    print(json.dumps({k: v for k, v in out.items()
                      if k not in ("failures", "unimplemented_sites")}, indent=1))


if __name__ == "__main__":
    a = sys.argv[1:]
    cmd = a[0] if a else ""
    if cmd == "check":
        cmd_check()
    elif cmd == "catalog":
        cmd_check(); cmd_catalog()
    elif cmd == "lift":
        only = None
        if "--only" in a:
            only = {int(x, 16) for x in a[a.index("--only") + 1].split(",")}
        cmd_check(); cmd_lift(only)
    elif cmd == "all":
        cmd_check(); cmd_catalog(); cmd_lift()
    else:
        sys.exit(__doc__)
