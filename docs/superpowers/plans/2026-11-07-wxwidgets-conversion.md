# wxWidgets Conversion Implementation Plan

> **For agentic workers:** Use superpowers:subagent-driven-development or superpowers:executing-plans task by task.

**Goal:** Deliver the complete shared Windows/Linux application and verified builds.
**Architecture:** Preserve the machine and renderer; use wxWidgets host integration and the user's tested AAE native audio/controllers. CMake owns builds and CTest.
**Tech Stack:** C++17, wxWidgets, native XAudio2/PipeWire/ALSA and controller backends, OpenGL, CMake. No SDL or GLFW.
**Spec:** ../specs/2026-11-07-wxwidgets-conversion-design.md

## Global constraints

- Full feature parity; preserve CPU and devices, settings, ROM layout, and CRT appearance.
- No Git operations: supplied directory has no repository metadata.
- User approved continuous implementation and Windows/Linux verification; no further design handoff is needed.
- Dependency installation requires normal sandbox approval where applicable.

## Review focus

Unicode paths; failed model switches; focus-loss stuck keys; modal pause/audio queue behavior; missing hardware/resources. Include these in integration verification.

## Tasks

### 1. Portable build/core and runtime integration

Files: CMakeLists.txt, petemu/emulator.cpp/.h, framework.h, sys_general utilities, system paths, tests/CMakeLists.txt.
Interfaces: retain pet_* control functions; emu_init returns bool; emu_present renders without stepping; frontend supplies host message/selection callbacks via system/host_services.h.
- [x] Run original Windows tests and record baseline.
- [x] Configure Linux core tests and observe portability failures.
- [x] Remove platform dependencies from core/build helpers without changing machine semantics.
- [x] Add CTest targets for existing tests and run both toolchains.
- [x] Make model loading transactional and paths independent of launch directory.

### 2. Portable keyboard and mapping editor

Files: petsrc/pet_kbd_input.*, petsrc/pet_keyboard_dialog.*, sys_input/rawinput.*, tests/keyboard_tests.cpp, wx keyboard adapter.
Interfaces: preserve 256-entry saved key identities and PetKeyBindings API; provide wx event translation, release-all, and wxWindow-parented editor.
- [x] Establish existing keyboard regression assertions on both platforms.
- [x] Replace OS key translation/polling with wx events and portable key constants.
- [x] Port complete mapping editor with draft Apply/Cancel behavior.
- [x] Test modifier, punctuation, reserved shortcut, and focus release behavior.

### 3. Portable sound and controller backend

Files: sys_audio/mixer.*, reused AAE native audio backends, sys_input/Joystick.cpp and joystick.h.
Interfaces: preserve mixer/stream calls used by emulator and joy_* SNES bridge; add audio queue flush for pause/reset.
- [x] Audit live PET mixer calls and existing CB2/SNES tests.
- [x] Reuse AAE XAudio2 and PipeWire/ALSA audio, Windows controller backends and Linux evdev. No SDL or GLFW.
- [x] Preserve sample mixing, gains, looping, missing-device behavior.
- [x] Exercise the actual mixer with a test output backend and controller mapping tests; physical controller hotplug remains manual validation.

### 4. Shared wx application

Files: new petemu/wx/app.cpp, frame/dialog modules; retire WinMain host entry from build.
Interfaces: use emulator.h, keyboard editor, host services, iniFile; wxGLCanvas owns current GL context; emu_run_frame advances and emu_present redraws.
- [x] Implement complete menus and accelerators, model/RAM checks, all dialogs.
- [x] Add bounded monotonic scheduling, resize/paint separation, modal suspension.
- [x] Preserve display scaling, fullscreen, monitor choice and persistence.
- [x] Verify UI smoke mode and rendering on Windows/Linux.

### 5. Integration and delivery

Files: README.md, build scripts, portable smoke tests, verification report.
- [x] Run all CTest tests in Windows and Linux builds.
- [x] Exercise ROM boots, loading, disk operations, editing/settings and audio.
- [x] Audit active build for Windows APIs and retire obsolete build entry points.
- [x] Fresh review, resolve findings, rerun affected tests and full suite.
- [x] Document exact results and any hardware/display limits honestly.

## Execution ledger

- 2026-11-07: Design approved. WSL available after sandbox approval; CMake/G++ present, GTK/wx/SDL development packages absent. Windows CMake and Python available.
- User correction: NO SDL or GLFW; reuse tested code from C:/Source2026/AAE_publish_new_vector_test and C:/Source2026/UltraHLE-main. This overrides the proposed replacement audio/controller approach. AAE native audio, mixer, voice mixer, controller mapping/backends and logger are copied with minimal integration changes. SDL removed from active build; downloaded artifacts pending cleanup.
- User requires native Wayland preferred over XWayland. wx GTK3 GL uses EGL, GLEW_EGL; startup prefers wayland,x11 when WAYLAND_DISPLAY exists and GDK_BACKEND is unset. Actual GDK display backend logged for verification. WSLg socket exists.
- Vulkan is a FUTURE phase; do not implement now. Preserve renderer/core/UI boundary.
- Original Windows suite passed entirely before edits. Linux 12 original portable core suites passed after C++ portability edits. Keyboard Windows40 checks and GUI28 checks pass independently; expanded Linux verification pending full build. Scheduler/CLI tests pass MSVC.
- New ROM transactional failure tests initially failed (partial BASIC replaced on missing character ROM; invalid editor accepted). Fixed by staging character ROM before installing other ROMs and rejecting invalid editor. Rerun pending.
- Audio agent reused AAE library, added mixer_flush backend operation. Actual mixer mocked-output test passed MSVC and Linux ASan/UBSan, verifies 735 native 44.1k samples -> 800 device48k frames and flush/pause. emu_init now calls stream_set_native_rate(1,44100).
- Frontend agent completed wx menus/dialogs, GL lifecycle, bounded scheduler, original CLI/scale compatibility, smoke screenshot/nonblank check, nested pause, joystick window. Root owns integration. All sources in petemu/wx.
- Builds: build/windows (MSVC2026, wx source override sibling); build/linux (GCC15, same wx source); root building both. Need fix compile errors, then full tests, native-Wayland and Windows smoke, ROM boots, packaging/docs/legacy cleanup, independent review. Do not redispatch completed implementation tasks.

## Final verification — 2026-11-07

- Final Windows and Linux builds succeeded. Each passed all 25 CTest tests, with no skips; logs: build/windows-tests.log and build/linux-tests.log.
- Windows test runtime used a Chinese directory name; configuration, ROM loading, Unicode titles/menus and log creation passed. Disk tests cover Unicode paths for save, wildcard load and scratch.
- All five model rendering smoke tests passed on each platform. Linux explicitly used native Wayland and verified the actual GDK backend; screenshots/logs are under build/linux/smoke-data and build/windows/smoke-data.
- Actual ROM tape save/load passed across all five profiles. Native XAudio2 and ALSA output smoke tests passed. Physical controller hotplug, a live PipeWire server, international keyboard layouts and other compositors remain manual/hardware validation.
- Review findings resolved: transactional ROM loading, visible software-load failures, right-click menu parity, Unicode disk operations and GTK RAM radio-state synchronization. Final suites rerun after fixes.
- Retired Win32 frontend/project files are preserved under backups/legacy-win32. Batch test entry points now use CMake. SDL source/build caches removed; no SDL or GLFW in the active application.
- Unicode strings and UTF-8 path boundaries are in place. Translation catalogs/language selection and Vulkan remain future work.
