# Pet-GPT shared Windows/Linux conversion

## Intent and scope

The user requests a full conversion of the Windows application to a common
wxWidgets application for Windows and Linux, using Skein Downloader as the
architectural reference. This is feature parity, not a reduced Linux frontend.
The same frontend and emulator sources must build on both operating systems.

Preserve the existing 6502 core and PET devices. Never introduce another CPU
implementation. Preserve cycle timing, disk behavior, tape formats, ROM layouts,
and the existing OpenGL CRT appearance. This project is a portability conversion,
not a redesign of the machine or its interface.

## Current structure

`petemu/petsrc` contains the machine, memory, devices, storage, video, and keyboard
mapping logic. `emulator.cpp` integrates them but also creates Win32 dialogs and
depends on Windows input/audio. `system/host_app.h` provides useful callbacks but
contains Windows types. `system/host_window.cpp` owns the Win32 application shell.
The keyboard editor is a Win32 dialog; the mixer uses XAudio2, including direct
voice operations. Timing, monitor selection, paths, logging, and raw input also
contain Windows calls. Tests currently build through MSVC batch files.

The supplied working directory has no Git repository metadata. Do not initialize
a repository, create commits, or delete historical files just to satisfy workflow
steps. Preserve the original source until the replacement passes verification.

## Proposed implementation choices

These are engineering proposals, not preferences already supplied by the user:

- C++17 and CMake, with one application target and portable core/test targets.
- wxWidgets for startup, windows, menus, dialogs, keyboard events, display/DPI
  handling, paths, configuration integration, and OpenGL context ownership.
- Reuse the user's tested audio and controller library from
  C:/Source2026/AAE_publish_new_vector_test. Its shared mixer and platform
  backends provide Windows XAudio2 and Linux PipeWire/ALSA output, plus native
  controller acquisition. No SDL, GLFW, or replacement audio library.
- Other reusable code may come from C:/Source2026/UltraHLE-main.
- Prefer native Wayland with wx GTK and EGL on Linux, verify the actual backend,
  and retain X11 compatibility only as fallback. Vulkan is a subsequent project.
- Retain the existing PET OpenGL renderer and shaders, hosted by wxGLCanvas.
- Use standard C++ timing, threading, filesystem utilities, and portable logging
  where a GUI dependency is unnecessary.
- Follow Skein's separation of core and UI and its CMake dependency approach.
  Do not copy its disabled-GL build configuration or downloader business logic.
  Dependencies must be pinned or discoverable via documented CMake options;
  builds must not require the sibling Skein directory. A source override may use
  its local wxWidgets checkout during development.

There must be no application-owned Win32 window procedure, HWND-based public
interface, WGL context lifecycle, Raw Input keyboard handling,
or Windows-only application timing in the completed frontend build. The user's
tested native audio/controller backends retain their appropriate OS APIs.
OS implementation code inside third-party libraries is outside this conversion.
Windows executable icons/version resources and manifests remain packaging data.
Unused legacy utility code is excluded or retired deliberately after callers are
audited; it must not be a hidden fallback for missing converted features.

## UI and behavior parity

Preserve the existing menu organization and keyboard shortcuts:

- File: program/disk/tape loading, disk eject, tape play/stop/rewind/eject,
  reset, exit, and the multi-program T64 picker with cancellation.
- Machine: all five models, applicable RAM selections, double speed, graphics
  keyboard mode, keyboard mapping editor, and SNES adapter enablement.
- View: 1x/2x/3x, fit, fullscreen, and the complete live CRT settings dialog.
- Help: About, version, credits, and licensing.

Retain any existing runtime hotkeys and settings discovered during the call-site
audit, including shader adjustments, audio volume, and ambient sound. Do not
silently omit behavior because it is absent from the menu resource.

The keyboard editor retains the keyboard diagram, model-specific labels, shift
preview, capture, assignment selection, clearing, per-key/default restoration,
Apply, OK, and Cancel. Draft edits must not modify active bindings until applied.
Use portable key identities with explicit translation of legacy saved VK values.
Handle shifted punctuation, graphics/business mappings, modifiers, repeat,
shortcut suppression, and release-all on focus loss. Do not use native key codes
as if they were interchangeable between Windows and Linux.

Use wx sizers and DPI-aware dimensions. Preserve 4:3 content, black bars, fullscreen
restore geometry, scale availability based on monitor work area, and the startup
monitor option. Respect compositor placement limits on Linux and safely fall
back to an available display when a saved monitor is absent.

## Runtime boundaries

Keep machine/core code independent of wxWidgets. Frontend callbacks handle error
presentation and program selection; loading failures return actionable errors
without terminating a healthy session. Failed model switches must preserve the
previous usable machine/configuration.

The UI thread owns GL operations and machine mutations. A monotonic elapsed-time
scheduler advances bounded batches of emulated cycles and yields to the wx event
loop. Emulation speed must follow machine time, not monitor refresh or wx timer
precision. Bound catch-up after long pauses. Repaint may present the latest frame
without stepping the machine. Suspend/restart scheduling deliberately during
modal operations and teardown; modeless CRT changes remain live.

Preserve CB2 sample generation and reuse AAE's tested portable mixer/voice behavior
and platform output rather than reimplementing those algorithms.
Bound audio buffering, prevent callback use after shutdown, and avoid an audio
thread accessing mutable machine state. Reset/load/pause must not replay stale
queued samples. Missing audio hardware is nonfatal and reported appropriately.
Controllers support hotplug, disconnect release, the existing SNES button mapping,
and joystick fallback for devices without a standardized gamepad mapping.

## Files and compatibility

Preserve existing pet.ini keys and numeric keyboard mappings through explicit
compatibility conversion. Keep writable portable installations working with
adjacent pet.ini, ROMs, files, shaders, and assets. Installed builds use platform
user configuration/data locations for writes and installed/executable-relative
resources for reads. Resolve these once, independently of the launch directory.
Do not silently move or overwrite user disks, ROMs, or settings. Use UTF-8 at core
boundaries and path-aware file APIs; verify non-ASCII filenames on both systems.
Supply Linux-compatible build/run and ROM setup instructions without bundling ROMs.

## Build and delivery

Provide documented CMake Windows/MSVC and Linux builds, runnable resource layouts,
and CTest registration for the existing meaningful test coverage. CMake becomes
the authoritative cross-platform build. Any retained VS entry point must build
the shared frontend or be clearly retired rather than launch the old application.
Distribution-store publishing, Flatpak/AppImage work, new emulation features,
localization, and a visual redesign are outside this conversion.

## Acceptance and verification

Completion requires Windows and Linux builds of the shared application and the
portable tests. Port existing tests rather than discarding their assertions.
Cover CPU, VIA/PIA, CRTC/video, machine integration, PRG/T64/TAP, disks/IEEE,
keyboard mappings, SNES mapping, CB2 samples, and viewport calculations as applicable.
Add focused regression checks for configuration/key migration, focus loss,
cancel/apply behavior, timing, and error handling where changed behavior needs it.

Exercise boot/model changes with available ROMs, typing and shortcuts, loading and
ejecting media, disposable disk writes, CRT controls, keyboard editing, resize,
fullscreen, settings persistence, and clean shutdown on both platforms. Verify
audio/controller behavior with available devices; explicitly report any hardware
or GUI checks that cannot be performed. Linux under WSL is useful build/runtime
evidence but does not establish every native Wayland/X11 or hardware behavior.
Audit application sources and build inputs for remaining Windows dependencies.
Never claim full verification from compilation alone.

Implementation may be divided into internal milestones, but delivery remains the
full conversion. Dependency versions, module names, and precise scheduling/audio
buffer sizes are left to the implementer, backed by build and runtime evidence.
