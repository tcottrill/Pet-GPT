# Windows/Linux port and build notes

The application uses a shared wxWidgets frontend and the existing PET machine,
6502 core, devices, and OpenGL CRT renderer. CMake is the supported build system.
The historical Visual Studio/WinMain shell is retired from the active build.
The supplied working directory has no Git metadata; building does not require it.

## Host architecture

wxWidgets owns windows, menus, dialogs, input events, and the GL canvas. Linux uses
GTK3 with EGL and native Wayland support enabled. GTK can also run under X11;
Xvfb is useful for control tests but does not establish native Wayland behavior.
Window placement on Wayland remains subject to compositor policy.

Audio, controller input, and logging reuse the tested implementation copied from
`AAE_publish_new_vector_test`, adapted to PET's existing mixer and SNES bridge.
Windows uses native audio/controller backends; Linux uses ALSA, optional PipeWire,
and evdev. There is no SDL or GLFW dependency. Missing audio/controller hardware
must not prevent the machine from running. `UltraHLE-main` was available as an
additional implementation reference; neither sibling checkout is a runtime dependency.
Vulkan is a possible future renderer, not part of this conversion.

## Windows build

Install Visual Studio 2022 with Desktop development with C++, a Windows SDK, and
CMake 3.24 or newer. Run from the source root:

```powershell
cmake -S . -B build/windows -A x64
cmake --build build/windows --config Release --parallel
ctest --test-dir build/windows -C Release --output-on-failure
```

Or run `./tools/build-windows.ps1`. The executable is
`build/windows/bin/Release/pet-gpt.exe`. The helper accepts `-Configuration`,
`-BuildDir`, `-WxSource`, and `-Jobs`; it stops on configure, build, or test failure.
A desktop session is needed for native GUI tests.

## Linux build

On a Debian/Ubuntu system, install the compiler, GTK3/EGL/OpenGL development files,
ALSA, and Wayland support before configuring wxWidgets:

```sh
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build pkg-config \
  libgtk-3-dev libgl1-mesa-dev libegl1-mesa-dev libasound2-dev \
  libwayland-dev wayland-protocols libxkbcommon-dev
```

Optional PipeWire and headless test dependencies:

```sh
sudo apt-get install libpipewire-0.3-dev xvfb xauth
```

Build and test in a desktop session:

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

Or run `bash tools/build-linux.sh`. The executable is `build/linux/bin/pet-gpt`.
The helper accepts an optional build-directory argument and honors `PET_WX_SOURCE`,
`PET_BUILD_TYPE`, and `CMAKE_BUILD_PARALLEL_LEVEL`. On a headless machine:

```sh
xvfb-run -a bash tools/build-linux.sh
```

To explicitly exercise native Wayland from an existing Wayland session:

```sh
GDK_BACKEND=wayland PETGPT_DATA_DIR="$HOME/PET-data" ./build/linux/bin/pet-gpt
```

A working OpenGL 3.3 implementation is required. A WSL/Xvfb build or test run is
useful evidence but does not prove every native compositor, display, audio device,
or controller configuration.

## Dependencies and core-only builds

The default GUI configuration fetches wxWidgets **3.3.3** using CMake FetchContent.
Initial configuration therefore needs network access. To use an existing source
checkout, pass `-DPET_WX_SOURCE=/absolute/path/to/wxWidgets`. The local sibling
Skein checkout can be used for development, but is not required or hardcoded.
Use a separate build directory when switching toolchains or wx configurations.

Core tests can be built without GUI or audio development packages:

```sh
cmake -S . -B build/core -DPET_BUILD_GUI=OFF -DBUILD_TESTING=ON
cmake --build build/core --parallel
ctest --test-dir build/core --output-on-failure
```

This excludes GUI-dependent keyboard/editor and host-audio tests; it is not a
substitute for the complete application test suite.

## Resources, settings, and user files

Paths do not depend on the directory from which the application was launched.
Selection order is:

1. A nonempty `PETGPT_DATA_DIR` explicitly selects a common resource and writable
   data root. Put your `roms/`, `files/`, and `pet.ini` there.
2. Without the override, resources start beside the executable. Installed Linux
   layouts can use `../share/pet-gpt` relative to it.
3. An existing writable `pet.ini` in the resource directory enables portable mode.
   Otherwise settings and user content go to per-user folders:
   - Windows: wxWidgets' user data directory, `%APPDATA%\Pet-GPT`, for both.
   - Linux: split the way AAE and Dolphin do it, because the program's own folder
     and dot-folders are hidden on SteamOS. `pet.ini` and the log go to
     `$XDG_DATA_HOME/pet-gpt` (default `~/.local/share/pet-gpt`); `files/`, `roms/`
     and `screenshots/` go to the visible `~/Pet-GPT`, which gets a `README.txt`.
     An older single folder, `~/.Pet-GPT`, is moved into this layout on first start
     without overwriting anything; leftovers are reported in the log and the old
     folder is removed only once empty.
4. Resources present in the user folders take precedence over the resource
   directory. This permits private user ROM sets with a system installation.

The routing lives in `system/portable_paths.cpp` (`pet_paths::route`); `paths_tests`
covers the layout and the migration. The application creates the writable `files/`
directory. Existing installations
can retain their numeric keyboard mappings and `pet.ini` keys. Do not copy private
ROMs or user disks into public packages. A writable `PETGPT_DATA_DIR` is also useful
for isolated tests without modifying an existing installation.

Example data layout:

```text
PET-data/
├─ pet.ini
├─ files/
└─ roms/
   ├─ pet2001/
   ├─ pet2001n/
   ├─ pet4000-9/
   ├─ pet4000-12/
   └─ cbm8032/
```

The README lists the exact ROM filenames. ROMs are never distributed by the build
or installation process. Supply your own images, or explicitly invoke the optional
PowerShell downloader with `-RomsDir` pointing to your chosen data root:

```sh
pwsh ./petemu/tools/download-roms.ps1 -RomsDir "$HOME/PET-data/roms"
```

## Installation

CMake provides the executable and Linux desktop integration:

```sh
cmake --install build/linux --prefix "$HOME/.local"
```

For Windows, use `cmake --install build/windows --config Release --prefix <directory>`.
ROMs and user disks must be supplied separately. Installation into system directories
may require administrator privileges; a user prefix avoids that requirement.

## Verification status

Verification on 2026-11-07 uses MSVC on Windows and GCC on Ubuntu/WSLg, with the
local wxWidgets 3.3.4 source override from `skein-downloader/vendor/wxWidgets`.

- Both platforms: 25 CTest tests passed, including actual frontend menu/dialog
  operations, Unicode titles, keyboard mapping, mixer and native audio tests.
- Real-ROM tape save/load checks cover all five models. ROMs are locally supplied.
- Rendering smoke checks boot all five models and capture nonblank screenshots.
  Linux is forced to `GDK_BACKEND=wayland`, and checks the actual native Wayland
  display backend in the application log.
- Windows also passes the suite with configuration, ROMs and logging in a Chinese
  directory name. Disk tests cover save, wildcard load and scratch in such paths.

Run `tools/test-windows-smoke.ps1` or `python3 tools/test-linux-smoke.py` after
building. These use isolated copies of local `x64/Release` data. Set
`PETGPT_DATA_DIR` to the resulting `build/<platform>/smoke-data` for frontend CTest
coverage; otherwise that optional test reports a skip. Configure with
`-DPET_TEST_ROM_DIR=<absolute-path-to-roms>` to include `tape_rom_roundtrip`.

Native audio output was exercised through XAudio2 on Windows and ALSA on WSLg.
Physical controller hotplug, a live PipeWire server, international keyboard
layouts and other Wayland compositors remain hardware/manual validation items.

## Unicode and future translations

The wxWidgets interface uses Unicode strings; non-ASCII source literals use
`wxS`, with MSVC compiling sources as UTF-8. Host/core text boundaries use UTF-8
explicitly and filesystem operations use Unicode paths. Windows log creation uses
the wide file API. This fixes garbled punctuation without restricting labels to
ASCII. Translation catalogs and a language selector have not been added yet.
