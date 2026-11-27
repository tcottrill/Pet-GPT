cmake_minimum_required(VERSION 3.24)
project(PetGPT VERSION 2.7.0 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Visual Studio solutions (tools/make-vs2022.ps1): wx's projects go in solution folders.
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
# Static CRT (wx too, cmake/Dependencies.cmake): no Visual C++ redistributable needed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
# The version shown to users: 2.7 for 2.7.0, 2.7.1 for 2.7.1. The version lives only
# in project() above; the About box, title bar, VERSIONINFO and release names read it.
set(PETGPT_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
if(PROJECT_VERSION_PATCH)
  string(APPEND PETGPT_VERSION ".${PROJECT_VERSION_PATCH}")
endif()
option(PET_BUILD_GUI "Build the shared wxWidgets application" ON)
include(CTest)
# The test sources are kept out of the published tree; build them only where they exist.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/petemu/tests/pet_machine_tests.cpp")
  set(BUILD_TESTING OFF)
endif()
if(MSVC)
  add_compile_options(/utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
if(WIN32)
  add_compile_definitions(UNICODE _UNICODE)
endif()
set(PET_INCLUDES petemu petemu/cpu_cores petemu/petsrc petemu/sys_general petemu/sys_input petemu/sys_audio petemu/system petemu/thirdparty)
# The vendored AAE 6502 builds at the compiler's default warning level;
# the rest of the PET core builds with full warnings.
add_library(pet_cpu OBJECT petemu/cpu_cores/cpu_6502.cpp)
target_include_directories(pet_cpu PUBLIC ${PET_INCLUDES})
add_library(pet_core STATIC
  $<TARGET_OBJECTS:pet_cpu>
  petemu/petsrc/pet_machine.cpp petemu/petsrc/pet_mem.cpp
  petemu/petsrc/pet2001io.cpp petemu/petsrc/pia6520.cpp petemu/petsrc/via6522.cpp
  petemu/petsrc/mos6545.cpp petemu/petsrc/pet2001video.cpp
  petemu/petsrc/pet2001ieee_ioport.cpp petemu/petsrc/pet2001ieee_d64.cpp
  petemu/petsrc/pet2001ieee_vdrive.cpp petemu/petsrc/pet2001ieee_seq.cpp
  petemu/petsrc/pet_roms.cpp)
target_include_directories(pet_core PUBLIC ${PET_INCLUDES})
if(MSVC)
  target_compile_options(pet_core PRIVATE /W4)
else()
  target_compile_options(pet_core PRIVATE -Wall -Wextra)
endif()
if(BUILD_TESTING)
  function(pet_test name)
    add_executable(${name} petemu/tests/${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE pet_core)
    add_test(NAME ${name} COMMAND ${name})
  endfunction()
  foreach(name cpu_6502_tests via6522_tests snes_adapter_tests cb2_render_tests d64_tests pet2001video_tests mos6545_tests pet_machine_tests prg_relink_tests tap_tests t64_tests)
    pet_test(${name})
  endforeach()
  pet_test(host_view_tests petemu/system/host_view.cpp)
  pet_test(rom_loading_tests)
  pet_test(frame_schedule_tests)
  pet_test(fullscreen_cursor_tests)
  pet_test(ini_file_tests petemu/sys_general/iniFile.cpp petemu/sys_general/sys_log.cpp)
  find_package(Threads REQUIRED)  # sys_log runs a writer thread
  target_link_libraries(ini_file_tests PRIVATE Threads::Threads)
  pet_test(sys_log_tests petemu/sys_general/sys_log.cpp)
  target_link_libraries(sys_log_tests PRIVATE Threads::Threads)
  set(PET_TEST_ROM_DIR "" CACHE PATH "Optional locally supplied ROM sets for real tape/boot integration tests")
  if(PET_TEST_ROM_DIR)
    add_test(NAME tape_rom_roundtrip COMMAND tap_tests "${PET_TEST_ROM_DIR}/pet2001n/")
    set_tests_properties(tape_rom_roundtrip PROPERTIES TIMEOUT 120)
    add_executable(ieee_rom_tests petemu/tests/ieee_rom_tests.cpp)
    target_link_libraries(ieee_rom_tests PRIVATE pet_core)
    add_test(NAME ieee_rom_drive COMMAND ieee_rom_tests "${PET_TEST_ROM_DIR}")
  endif()
endif()
if(PET_BUILD_GUI)
  include(cmake/Dependencies.cmake)
  find_package(OpenGL REQUIRED)
  add_library(pet_support STATIC petemu/sys_general/iniFile.cpp petemu/sys_general/sys_log.cpp
    petemu/sys_input/rawinput.cpp petemu/petsrc/pet_kbd_input.cpp
    petemu/sys_input/pad_map.cpp petemu/sys_audio/mixer.cpp
    petemu/sys_audio/voice_mixer.cpp petemu/sys_audio/wav_writer.cpp)
  find_package(Threads REQUIRED)
  if(WIN32)
    target_sources(pet_support PRIVATE petemu/sys_input/Joystick.cpp petemu/sys_audio/xaudio2_backend.cpp petemu/sys_audio/audio_3d.cpp)
    target_link_libraries(pet_support PUBLIC xaudio2 xinput winmm dinput8 dxguid ole32)
  else()
    find_package(ALSA REQUIRED)
    target_sources(pet_support PRIVATE petemu/sys_input/linux/evdev_device.cpp petemu/sys_input/linux/evdev_joystick.cpp
      petemu/sys_audio/linux/alsa_backend.cpp petemu/sys_audio/linux/audio_3d_null.cpp)
    target_link_libraries(pet_support PUBLIC ALSA::ALSA Threads::Threads)
    find_package(PkgConfig REQUIRED)
    # OFF for the AppImage: Ubuntu 22.04's PipeWire headers are too old (ALSA only).
    option(PET_USE_PIPEWIRE "Use the native PipeWire audio backend when available" ON)
    if(PET_USE_PIPEWIRE)
      pkg_check_modules(PIPEWIRE IMPORTED_TARGET libpipewire-0.3)
    endif()
    # PIPEWIRE_FOUND is cached by an earlier configure, so check the option as well.
    if(PET_USE_PIPEWIRE AND PIPEWIRE_FOUND)
      target_compile_definitions(pet_support PRIVATE AAE_HAVE_PIPEWIRE)
      target_link_libraries(pet_support PRIVATE PkgConfig::PIPEWIRE)
    endif()
  endif()
  target_link_libraries(pet_support PUBLIC pet_core wx::core wx::base)
  add_library(pet_glew STATIC petemu/thirdparty/glew.c)
  # GLEW_NO_GLU: Pet-GPT calls no GLU function, and the Freedesktop runtime the
  # Flatpak builds against ships no GL/glu.h, which glew.h includes otherwise.
  target_compile_definitions(pet_glew PUBLIC GLEW_STATIC GLEW_NO_GLU)
  target_include_directories(pet_glew PUBLIC petemu/thirdparty)
  target_link_libraries(pet_glew PUBLIC OpenGL::GL)
  if(NOT WIN32)
    target_compile_definitions(pet_glew PUBLIC GLEW_EGL)
    target_link_libraries(pet_glew PUBLIC OpenGL::EGL)
  endif()
  # PNGs compiled in as byte arrays (no committed hex headers): the 256 px app icon,
  # which the About box scales, and the Ko-fi cup logo for the Ko-fi button (copied
  # from FreeSnip's src/wx/res/kofi-cup.png; the rest of the button is drawn).
  function(embed_png png header name)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${png}")
    file(READ "${png}" hex HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," hex "${hex}")
    file(RELATIVE_PATH source "${CMAKE_CURRENT_SOURCE_DIR}" "${png}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${header}"
      CONTENT "// Generated from ${source} by CMakeLists.txt.\nstatic const unsigned char ${name}[] = {${hex}};\n")
  endfunction()
  embed_png("${CMAKE_CURRENT_SOURCE_DIR}/packaging/icons/hicolor/256x256/apps/io.github.tcottrill.Pet-GPT.png" app_icon.h appIconPng)
  embed_png("${CMAKE_CURRENT_SOURCE_DIR}/petemu/wx/res/kofi-cup.png" kofi_cup.h kofiCup)
  # Version, generated headers and the .rc defines for the app and frontend_tests.
  function(pet_gui_target target)
    target_compile_definitions(${target} PRIVATE PETGPT_VERSION="${PETGPT_VERSION}"
      PETGPT_VERSION_FULL="${PROJECT_VERSION}"
      PETGPT_VERSION_RC=${PROJECT_VERSION_MAJOR},${PROJECT_VERSION_MINOR},${PROJECT_VERSION_PATCH},0)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
  endfunction()
  set(PET_GUI_SOURCES petemu/wx/crt_dialog.cpp petemu/wx/about.cpp
    petemu/wx/keyboard_input.cpp petemu/petsrc/pet_keyboard_dialog.cpp
    petemu/emulator.cpp petemu/petsrc/pet_gl.cpp petemu/system/portable_paths.cpp
    petemu/system/host_view.cpp)
  if(WIN32)
    list(APPEND PET_GUI_SOURCES petemu/system/monitor_select.cpp)
  endif()
  add_executable(pet-gpt WIN32 petemu/wx/app.cpp ${PET_GUI_SOURCES})
  target_link_libraries(pet-gpt PRIVATE pet_support pet_glew wx::core wx::base wx::gl)
  pet_gui_target(pet-gpt)
  if(WIN32)
    target_sources(pet-gpt PRIVATE petemu/app_resources.rc)
    if(MSVC)
      target_link_options(pet-gpt PRIVATE /MANIFEST:EMBED "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/petemu/petemu.manifest")
    endif()
    # Visual Studio (tools/make-vs2022.ps1): F5 starts Pet-GPT from the repository.
    set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT pet-gpt)
    set_target_properties(pet-gpt PROPERTIES VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
  endif()
  if(NOT WIN32)
    pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
    target_link_libraries(pet-gpt PRIVATE PkgConfig::GTK3)
  endif()
  # dist/pet-gpt.exe (dist/debug/ for Debug). Packaging builds, whose source tree is
  # read-only, set their own folder.
  set(PETGPT_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/dist" CACHE PATH "Folder for the built executable")
  set_target_properties(pet-gpt PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PETGPT_OUTPUT_DIR}$<$<CONFIG:Debug>:/debug>")
  install(TARGETS pet-gpt RUNTIME DESTINATION bin)
  # The ROM setup dialog looks for the downloader next to the executable.
  add_custom_command(TARGET pet-gpt POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/petemu/tools/download-roms.ps1" "$<TARGET_FILE_DIR:pet-gpt>")
  # Beside the .exe on Windows; on Linux a data file, not a command in bin/
  # (FindRomScript also looks in ../share/pet-gpt).
  if(WIN32)
    install(FILES petemu/tools/download-roms.ps1 DESTINATION bin)
  else()
    install(FILES petemu/tools/download-roms.ps1 DESTINATION share/pet-gpt)
  endif()
  install(FILES petemu/petemu.ico DESTINATION share/pet-gpt)
  if(UNIX)
    install(FILES packaging/pet-gpt.desktop DESTINATION share/applications)
    install(FILES packaging/io.github.tcottrill.Pet-GPT.xml DESTINATION share/mime/packages)
    install(FILES images/pet-gpt-logo.png DESTINATION share/pixmaps RENAME pet-gpt.png)
    install(DIRECTORY packaging/icons/hicolor DESTINATION share/icons)
  endif()
  if(BUILD_TESTING)
    add_executable(frontend_tests petemu/tests/frontend_tests.cpp ${PET_GUI_SOURCES})
    target_link_libraries(frontend_tests PRIVATE pet_support pet_glew wx::core wx::base wx::gl)
    pet_gui_target(frontend_tests)
    if(WIN32)
      target_sources(frontend_tests PRIVATE petemu/app_resources.rc)
    else()
      target_link_libraries(frontend_tests PRIVATE PkgConfig::GTK3)
    endif()
    add_test(NAME frontend_tests COMMAND frontend_tests)
    set_tests_properties(frontend_tests PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 90)
    pet_test(keyboard_tests petemu/petsrc/pet_kbd_input.cpp)
    pet_test(keyboard_dialog_tests petemu/petsrc/pet_kbd_input.cpp petemu/wx/keyboard_input.cpp petemu/sys_general/iniFile.cpp)
    target_link_libraries(keyboard_dialog_tests PRIVATE wx::core wx::base)
    add_subdirectory(petemu/sys_audio/tests)
    pet_test(pad_map_tests petemu/sys_input/pad_map.cpp)
    pet_test(paths_tests petemu/system/portable_paths.cpp petemu/sys_general/iniFile.cpp petemu/sys_general/sys_log.cpp)
    target_link_libraries(paths_tests PRIVATE wx::base Threads::Threads)
  endif()
endif()
