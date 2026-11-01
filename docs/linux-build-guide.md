# Beginner's guide: build Killer Instinct Gold: Recompiled on Linux

This guide explains the commands from scratch. It is for **Arch Linux / CachyOS**, where the native Linux port has been tested. Other distributions need different package-install commands and may behave differently. You do **not** need Windows, Wine, or Proton.

> **Important:** This guide currently applies to the experimental `linux-port` branch. A source build needs your own legally obtained **Killer Instinct Gold (US, Rev 2 / v1.2)** game dump. The repository does not provide the ROM.

## 1. Open a terminal and install the tools

In KDE, open **Konsole** (your terminal app). The blocks below are commands: copy one block at a time, paste into the terminal, and press Enter. Commands such as `sudo` may ask for your Linux account password; your typing might not appear on screen.

Install the compiler, build tools, Python, SDL compatibility layer, Vulkan headers/loader, and GTK file-dialog support:

```sh
sudo pacman -S --needed base-devel clang cmake ninja python python-pip git sdl2-compat vulkan-headers vulkan-icd-loader gtk3 pkgconf
```

Install a working Vulkan graphics driver for your GPU separately if you don't already have one. If `pacman` says a package cannot be found, stop and confirm that you're on Arch/CachyOS; package names differ on other distributions.

You can check your tools:

```sh
git --version
clang --version
cmake --version
ninja --version
python --version
```

Each should print a version rather than `command not found`.

## 2. Download the project's source code

Choose where you want the project. This example uses a `Projects` directory in your home folder:

```sh
mkdir -p ~/Projects
cd ~/Projects
git clone --recurse-submodules --branch linux-port https://github.com/robzombie91/kigold-recomp.git
cd kigold-recomp
```

**Why `--recurse-submodules`?** It also downloads required libraries such as N64Recomp and RT64. If you already cloned without it, run `git submodule update --init --recursive` inside the project.

If `git clone` says the directory already exists, **don't delete it**. Instead, use your existing checkout and ensure it is on the desired branch.

## 3. Supply and verify your own ROM

Put **your own** US Rev 2 dump in the top-level project directory (the directory containing `CMakeLists.txt`). Name the file exactly:

```text
baserom.us.z64
```

Use your file manager to copy and rename it. Check it from the terminal:

```sh
sha1sum baserom.us.z64
```

It must output this SHA-1 hash:

```text
aaeb492b1e538af65a3544a97240675ef438a04f
```

If the hash is different, **do not continue**: the build requires this specific ROM revision. Do not upload or commit the ROM to GitHub.

## 4. Create an isolated Python environment

The project uses Python tools to analyze the ROM and generate code. A *virtual environment* keeps their Python packages separate from system-wide packages.

Create one:

```sh
python -m venv .venv
```

Activate it using the instructions for **your shell**:

**Bash or Zsh:**
```sh
source .venv/bin/activate
```

**Fish (default on some CachyOS setups):**
```fish
source .venv/bin/activate.fish
```

Your prompt might now show `(.venv)`. Install the required Python packages:

```sh
python -m pip install rabbitizer xxhash 'splat64[mips]' spimdisasm
```

If you open a new terminal later, return to the project directory and **activate the environment again**. These dependencies must be installed into the environment, not through `sudo pip`.

## 5. Build the two recompilation tools

These programs translate N64 machine code and audio microcode into code the Linux compiler can build.

First, apply the project's supplied patch. This is part of the normal setup and is **not** a change to submit separately to the N64Recomp submodule:

```sh
git -C lib/N64Recomp apply ../../patches/n64recomp/0001-handwritten-code.patch
```

Then configure and build them:

```sh
cmake -S lib/N64Recomp -B lib/N64Recomp/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build lib/N64Recomp/build --target N64RecompCLI RSPRecomp --parallel 8
```

Check the files were built:

```sh
ls -lh lib/N64Recomp/build/N64Recomp lib/N64Recomp/build/RSPRecomp
```

Both should exist. If `git apply` says the patch **does not apply**, it might already have been applied. Check before trying to apply it again; don't run `git reset --hard` to fix this.

## 6. Generate symbols and translated source code

Windows users run the PowerShell-only `tools/regen_syms.ps1`. Linux users have an equivalent Python script:

```sh
python tools/regen_syms_linux.py
```

This is a substantial generation step. It prepares the ROM layout, runs Splat, exports and analyzes functions, writes symbol definitions, and uses N64Recomp to generate native C code.

Successful output includes messages similar to:

```text
1919 functions (257 named) -> syms/kig.us.syms.toml
N64Recomp exit 0; Function count: 1919; 36 C files in RecompiledFuncs/
```

**Why a special Linux script?** Splat 0.50.1 can reject the free-text `manual:` notes in `syms/libultra_symbols.txt`. The script makes a temporary compatible input without removing the maintainer's annotations. It reuses checked-in library names; rebuilding the underlying library-name source itself requires a separate W.G. 3D Hockey reference project, which is **not** required for the normal Linux build.

Some analysis warnings can appear. A Python traceback or a nonzero exit, however, indicates failure; resolve that before building the game.

## 7. Compile the actual game

Configure the build. The `-B` option tells CMake where to store compiled files, and the Python option tells it to use the environment you created:

```sh
cmake -S . -B build/cmake-linux -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DPython3_EXECUTABLE="$PWD/.venv/bin/python"
```

Compile:

```sh
cmake --build build/cmake-linux --target kig --parallel 8
```

`--parallel 8` means up to eight build jobs at once. On a system with limited RAM, try `--parallel 4` instead. The first build can take a while. Compiler *warnings* do not automatically mean failure; look for `FAILED`, `error:`, or a command that exits unsuccessfully.

The result should be:

```text
build/cmake-linux/kig
```

Check it with `ls -lh build/cmake-linux/kig`.

## 8. Run the game

From the project directory:

```sh
./build/cmake-linux/kig
```

Choose your ROM if prompted. Initial window setup should report a Vulkan backend if your graphics drivers are working.

**Known issue:** On the tested CachyOS/Wayland setup, closing the window can trigger a segmentation fault **during shutdown**, even though gameplay runs. An X11/XWayland run exited cleanly. If you encounter this, try:

```sh
env SDL_VIDEODRIVER=x11 ./build/cmake-linux/kig
```

This changes only the SDL video backend for that launch.

## Common problems

| Symptom | First thing to check |
| --- | --- |
| `command not found` | Did you install the packages in step 1? |
| `No module named splat/rabbitizer` | Did you activate `.venv` and run the pip command? |
| `baserom.us.z64` missing or wrong SHA-1 | Check location, filename, and exact ROM revision in step 3. |
| `N64Recomp` or `RSPRecomp` missing | Recheck step 5, especially the tool build output. |
| Splat complains about `manual:` | Use `python tools/regen_syms_linux.py` rather than running Splat directly against the original YAML. |
| `SDL_Vulkan_CreateSurface` fails | Ensure you built the current `linux-port` branch, have a working Vulkan driver, and did not disable the Linux `RT64_SDL_WINDOW_VULKAN` compile definition. |
| Wayland SIGSEGV when closing | Use the X11 launch command from step 8; see the README's Linux known issues. |

## Rebuilding after you change code

Usually, when only a C/C++ source file changes, you can rebuild with:

```sh
cmake --build build/cmake-linux --target kig --parallel 8
```

You do not ordinarily need to re-clone, reapply patches, reinstall Python packages, or regenerate symbols for every rebuild.

For technical background, see [the main README](../README.md) and [technical notes](technical.md).
