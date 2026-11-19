# Contributing to Pet-GPT

Thanks for helping. Pet-GPT is a Commodore PET/CBM emulator in C++17 with a shared wxWidgets/OpenGL frontend for Windows and Linux, released under GPL-3.0-or-later.

## No ROMs, no commercial software

Do not add Commodore ROM images, commercial programs, disk or tape images of commercial software, or anything else you do not have the right to redistribute, to the repository, to a pull request, or to a release archive. `.gitignore` already excludes `roms/`, `files/`, `*.bin`, `*.d64` and `*.prg`; do not work around it. The program fetches ROMs from the user's own request with `petemu/tools/download-roms.ps1`.

If you need a program to show a bug, post a short BASIC listing or link to a freely redistributable one.

## Building

CMake 3.24 or newer is the only supported build. The full build fetches wxWidgets (see `cmake/Dependencies.cmake`), so the first configure needs network access; pass `-DPET_WX_SOURCE=/path/to/wxWidgets` to use an existing checkout. Details are in [docs/porting.md](docs/porting.md).

Windows (Visual Studio 2022, from the source root):

```powershell
./tools/build-windows.ps1
```

or by hand:

```powershell
cmake -S . -B build/windows -A x64
cmake --build build/windows --config Release --parallel
ctest --test-dir build/windows -C Release --output-on-failure
```

Linux (install the packages listed in docs/porting.md first):

```sh
bash tools/build-linux.sh
```

or by hand:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

The executables are `build/windows/bin/Release/pet-gpt.exe` and `build/linux/bin/pet-gpt`.

### Core-only build

The emulation core and its tests need no wxWidgets, GTK, OpenGL or audio packages:

```sh
cmake -S . -B build/core -DPET_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build/core --parallel
ctest --test-dir build/core --output-on-failure
```

This leaves out the GUI-dependent tests (frontend, keyboard, audio, paths, gamepad mapping), so it does not replace a full build before you send a change that touches the frontend.

## Tests

Run `ctest` as shown above. GUI tests need a desktop session; on a headless Linux machine use `xvfb-run -a ctest --test-dir build/linux --output-on-failure`. The frontend test reports a skip unless `PETGPT_DATA_DIR` points at a prepared data folder (see docs/porting.md).

Tests that need real Commodore ROMs (`tape_rom_roundtrip`, `ieee_rom_drive`) are only registered when you configure with your own ROM folder:

```sh
cmake -S . -B build/linux -DPET_TEST_ROM_DIR=/absolute/path/to/roms
```

The folder holds the per-model subfolders (`pet2001n`, `pet4000-9`, ...). Keep it outside the repository. CI never sets it, and a ROM must never be committed.

Add or update a test in `petemu/tests/` with a change when you can; tests are registered in `CMakeLists.txt`.

## Code style

- C++17. Keep `pet_core` free of wxWidgets and OS headers; host-specific code lives in `petemu/wx/`, `petemu/system/`, `petemu/sys_audio/` and `petemu/sys_input/`.
- Follow the file you are editing: the older `petsrc` code is indented with tabs, `petemu/wx/` and newer files with four spaces. Do not reformat lines you are not changing.
- Log through `LOG_INFO` / `LOG_WARN` / `LOG_ERROR` / `LOG_DEBUG` from `sys_log.h`. Sources are UTF-8; non-ASCII literals in the wx UI use `wxS(...)`.
- New third-party code needs a compatible licence and an entry in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md), with its original licence header kept in the file.

## Reporting a bug

Open an issue and fill in the template. Please include:

- the Pet-GPT version (Help > About, or the window title) and your operating system,
- the PET model selected in Machine > Model,
- the steps to reproduce, and what you expected,
- the log file, `petemu-log.txt`, attached after you quit the program:
  - Windows: `%APPDATA%\Pet-GPT\petemu-log.txt`
  - Linux: `~/.local/share/pet-gpt/petemu-log.txt` (or `$XDG_DATA_HOME/pet-gpt/`)
  - Flatpak: `~/.var/app/io.github.tcottrill.Pet-GPT/data/pet-gpt/petemu-log.txt`
  - portable install, or when `PETGPT_DATA_DIR` is set: that folder

Do not attach ROMs or commercial software.
