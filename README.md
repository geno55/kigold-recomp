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

## Building on Linux (CachyOS / Arch Linux)

A native Linux build requires Clang, CMake, Ninja, SDL2 development headers, Vulkan development
headers and loader, GTK 3 development files (for the native file dialog), Python and Git. The tested
CachyOS system used Clang 23.1.4, CMake 4.4.5, Python 3.14.8, and
`sdl2-compat 2.32.74-1.1` on KDE Wayland. Equivalent libraries may also work on other distributions.

Install the Arch packages (package names can vary on other distributions):

```sh
sudo pacman -S --needed base-devel clang cmake ninja python git sdl2-compat vulkan-headers vulkan-icd-loader gtk3 pkgconf
```

Clone the repository and its submodules. You must supply your own US Rev 2 game ROM as
`baserom.us.z64`; its SHA-1 is listed above. Do not commit the ROM or generated files.

```sh
git clone --recurse-submodules https://github.com/geno55/kigold-recomp.git
cd kigold-recomp
python -m venv .venv
. .venv/bin/activate
python -m pip install rabbitizer xxhash 'splat64[mips]' spimdisasm

git -C lib/N64Recomp apply ../../patches/n64recomp/0001-handwritten-code.patch
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build lib/N64Recomp/build --target N64RecompCLI RSPRecomp --parallel

python tools/regen_syms_linux.py
cmake -S . -B build/cmake-linux -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
cmake --build build/cmake-linux --target kig --parallel
./build/cmake-linux/kig
```

For **Fish**, activate the environment with `source .venv/bin/activate.fish` instead.
The generator uses native `N64Recomp` and `RSPRecomp` binaries from `lib/N64Recomp/build` by default;
you can override their paths with CMake's `N64RECOMP_EXE` and `RSPRECOMP_EXE` cache entries.

The Linux symbol workflow uses `tools/regen_syms_linux.py` in place of the PowerShell-only
`tools/regen_syms.ps1`. Splat 0.50.1 rejects free-text `manual:` symbol annotations; the script
creates a temporary Splat-compatible copy in `build/`, preserving the tracked annotations. It
uses the checked-in library symbol names and does **not** rerun `tools/kig_libsyms.py`, which
additionally requires W.G. 3D Hockey reference files. The original library symbol metadata
remains unchanged.

### Linux troubleshooting and known issues

**Splat 0.50.1 and `manual:` annotations:** Directly running
`python -m splat split splat/kig.us.yaml` fails when parsing the original
`syms/libultra_symbols.txt`, for example:

```text
Missing attribute value in 'manual:', is there extra whitespace?
```

The free-text annotation (e.g. `manual: VI manager thread...`) is valid
identification metadata for this repository, and `tools/gen_syms.py` reads
the named functions correctly. This is a **Splat parsing compatibility issue**,
not bad source annotations. The Linux regeneration script writes
`build/libultra_symbols_splat.txt` and `splat/kig.local.yaml`, changing
the annotation to a Splat-compatible `manual` flag **only in the generated
copy**. The tracked symbol file and its explanations remain untouched.
To regenerate the underlying library-name source itself, the separate
W.G. 3D Hockey reference project is still required.

**Wayland shutdown SIGSEGV:** With CachyOS KDE Wayland,
`sdl2-compat 2.32.74-1.1`, and an RTX 5080, closing the game window
can result in `SIGSEGV` (exit status 139). Vulkan, sound, ROM loading
and gameplay startup are successful. An X11/XWayland run exited normally;
two Wayland runs under GDB also exited normally, so no reliable backtrace
has been captured. Cause unconfirmed; it may be timing or backend specific.
As a temporary workaround, run `env SDL_VIDEODRIVER=x11 ./build/cmake-linux/kig`.
This is being tracked separately from native Linux compilation support.

**Other limitations:** Other Linux distributions have not been tested
in this PR, and AppImage packaging is not automated.

## For developers

How the port works, the fixes it needed, the build pipeline and the testing tools are described in
[docs/technical.md](docs/technical.md) and [docs/rom_map.md](docs/rom_map.md).

## License

GPL-3.0 (see LICENSE). Some code is adapted from Quest64-Recomp and W.G. 3D Hockey: Recompiled. The game
itself belongs to its owners.
