# pcrecomp patches

Changes this project needs in `../pcrecomp`, as `git format-patch` output
applied in order on top of `[pcrecomp].base_commit` (`944e296`) in
`config/hardtruck2.toml`. `scripts/pcrecomp_patches.sh` makes the `ht2`
branch from them. Each one is generic win32hle work, upstreamable as is,
and comes with selftests (`runtime/win32hle/build_selftests.sh`, all
passing with `HLE_BASED=1`).

| | |
|---|---|
| 0001 | `hle_sdl_vk` / `hle_sdl_scan`: screen.c's SDL key tables, for a host with its own SDL window |
| 0002 | .ini profiles as Windows reads and writes them (blanks, quotes, comments, NULL section/key lists, Int hex, in-place writes) |
| 0003 | `IStorage::EnumElements` and `IEnumSTATSTG` |
| 0004 | `GetFullPathNameA` answers a guest path, with `.` and `..` resolved |
| 0005 | window activation: `SetWindowPos` activates, destroying the active window activates the next (keyboard in the race) |

Known follow-up, not done yet: `disasm/disasm32.py` reads `jmp [reg*4 + table]`
tables forward only. The MSVC CRT's memmove indexes one backward (ecx < 0),
so those targets are in `config/seeds.txt` for now.

The nfsu2-recompiled series (`../nfsu2-recompiled/patches/pcrecomp/`) was
written against `35548b8`. Of its 14 patches only 0001, 0005, 0010 and 0014
still apply to `944e296`, and some are upstream already (the sign-flag fix,
1be12a0). Take one from there only when this game shows the bug it fixes.

Workflow for a new one:

    cd ../pcrecomp            # on branch ht2
    # fix, add a selftest, commit
    git format-patch 944e296 -o ../hardtruck2-recomp/patches/pcrecomp/
