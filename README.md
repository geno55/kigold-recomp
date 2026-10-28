# Killer Instinct Gold: Recompiled

Play Killer Instinct Gold, the 1996 Nintendo 64 fighting game, as a native Windows program, with no
emulator needed. The game's original code has been translated into a form that runs directly on a PC,
using [N64Recomp](https://github.com/N64Recomp/N64Recomp) and the [RT64](https://github.com/rt64/rt64)
renderer.

**You need your own copy of the game.** No part of the game is included here.

## What works

- The intro and title screen
- Character select
- Full fights, with graphics, sound and controller input
- The Continue screen

Still to be checked: saving, every character and stage, and a side-by-side comparison with the original
hardware.

## Getting the game file

You need the US version of the cartridge, Rev 2 (v1.2), dumped to a `.z64`, `.v64` or `.n64` file. To
check that you have the right one, its SHA-1 fingerprint should be
`aaeb492b1e538af65a3544a97240675ef438a04f`.

## Playing

Run `kig.exe`. The first time, it asks you to pick your game file. You can also drag the file onto
`kig.exe`.

Your settings and save data are kept in `%APPDATA%\KIGRecomp`.

## Building it yourself

You'll need Visual Studio 2022 Build Tools (with clang-cl and the Windows SDK), CMake 3.20 or newer, Ninja,
and Python 3.12 with the `rabbitizer`, `xxhash`, `splat64[mips]` and `spimdisasm` packages. Put your game
file in this folder as `baserom.us.z64`, then run these commands from PowerShell or Command Prompt (not
Git Bash):

```
git submodule update --init --recursive
git -C lib/N64Recomp apply ../../patches/n64recomp/0001-handwritten-code.patch
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build lib/N64Recomp/build --target N64RecompCLI RSPRecomp
tools\regen_syms.ps1
tools\cmake_build.bat kig
```

The finished game is at `build/cmake/kig.exe`. The last two steps need the Visual Studio build
environment; `tools\cmake_build.bat` sets it up for you.

## For developers

How the port works, the fixes it needed, the build pipeline and the testing tools are described in
[docs/technical.md](docs/technical.md) and [docs/rom_map.md](docs/rom_map.md).

## License

GPL-3.0 (see LICENSE). Some code is adapted from Quest64-Recomp and W.G. 3D Hockey: Recompiled. The game
itself belongs to its owners.
