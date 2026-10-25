# port/ — the PortMaster port (AmberELEC, ArkOS, ROCKNIX, muOS, ...)

The files that go to a handheld besides the binary. `scripts/package_port.sh`
turns this tree plus `build-aarch64/ht2.aarch64` into `dist/hardtruck2.zip`
(this README stays behind).

```sh
docker/build_aarch64.sh        # build-aarch64/ht2.aarch64, in PortMaster's builder image (qemu on x86)
scripts/package_port.sh        # dist/hardtruck2.zip and dist/port/
```

On the device, after unzipping into `ports/` (AmberELEC: `/storage/roms/ports`):

```
ports/
  Hard Truck 2.sh             the launcher: PortMaster's control.txt, gptokeyb, then ht2
  hardtruck2/
    ht2.aarch64               the recompiled game (glibc >= 2.31; SDL2 and GLES are the device's)
    libs.aarch64/             libmpg123 and SDL2_ttf (not on every CFW) and their licenses
    hardtruck2.gptk           pad -> keyboard and mouse (README.md has the table)
    port.json gameinfo.xml README.md screenshot.png    PortMaster's metadata
    gamedata/                 the player's own GOG install: king.exe, COMMON, ENV, MUSIC, ...
    log.txt                   the last run's output
```

`ht2 --game gamedata` makes `ports/hardtruck2` drive C:, so the game sees
itself at `C:\gamedata\king.exe`, and writes its settings and logs
(`warn.log`, `aimsg.log`) into `gamedata/` as it does on Windows.

## Choices

- **aarch64 only.** The build is the 64-bit "based" one. Its guest memory is
  reserved at `0x4000000000` (256 GB, `HT2_MEM_BASE` in `CMakeLists.txt`) and
  not at the desktop's `0x100000000000`, because handheld kernels often give
  user space 39 bits (512 GB) only. The 4 GB is reserved with
  `MAP_NORESERVE`, not committed, so it does not count against a 1 GB
  device's memory.
- **gptokeyb**, until the game gets a real DirectInput gamepad. The menus
  are mouse-driven, so the right stick moves the pointer and A clicks. The
  left stick is the arrow keys (steer, accelerate, brake), the d-pad the
  camera. A stick direction can only be one thing in gptokeyb, so the
  camera could not share the right stick with the pointer.
- **Bundled libraries**: only what a CFW may lack. SDL2 must be the device's,
  since it is the one built for its KMSDRM and GPU.
- **Old and new PortMaster.** The launcher falls back for what AmberELEC's
  older PortMaster lacks (`DEVICE_ARCH`, `pm_platform_helper`, `pm_finish`).

## Not verified on a device yet

Built and packaged, not yet run on a handheld. Watch for:

- **Memory**: the desktop build's resident size is the floor for a 1 GB device.
- **Threads**: the lifted code assumes x86's strong memory ordering, and ARM
  is weaker. A hang or a glitch that only shows on ARM should be checked for
  this first.
- **Speed**: the game draws at 1024x768, and the present scales that to
  the screen. `LONGROAD2_FPS_LIMIT` (see the launcher) is the first knob.
