@echo off
setlocal enabledelayedexpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
pushd "%~dp0"

set FAIL=0

echo === TAP cassette ===
call "%~dp0run_tap_tests.bat"
if errorlevel 1 set FAIL=1

echo === T64 archive ===
cl /nologo /std:c++17 /EHsc "t64_tests.cpp" /Fe:"t64_tests.exe" 1>build_t64.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_t64.log & set FAIL=1 ) else ( "%~dp0t64_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === Keyboard ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" /I "..\cpu_cores" /I "..\sys_general" /I "..\sys_audio" /I "..\sys_input" /I ".." "keyboard_tests.cpp" "..\petsrc\pet_kbd_input.cpp" /Fe:"keyboard_tests.exe" user32.lib 1>build_keyboard.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_keyboard.log & set FAIL=1 ) else ( "%~dp0keyboard_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === Keyboard dialog sizing ===
pushd "%~dp0.."
rc /nologo /fo tests\keyboard_dialog_test.res petemu.rc 1>tests\build_keyboard_dialog.log 2>&1
set RCFAIL=!errorlevel!
popd
if not "!RCFAIL!"=="0" (
  echo RESOURCE BUILD FAILED
  type build_keyboard_dialog.log
  set FAIL=1
) else (
  cl /nologo /std:c++17 /EHsc /I "..\petsrc" /I "..\cpu_cores" /I "..\sys_general" /I "..\sys_audio" /I "..\sys_input" /I ".." "keyboard_dialog_tests.cpp" "..\petsrc\pet_kbd_input.cpp" "..\sys_general\iniFile.cpp" "..\system\host_view.cpp" "keyboard_dialog_test.res" /Fe:"keyboard_dialog_tests.exe" user32.lib gdi32.lib 1>>build_keyboard_dialog.log 2>&1
  if errorlevel 1 ( echo BUILD FAILED & type build_keyboard_dialog.log & set FAIL=1 ) else ( "%~dp0keyboard_dialog_tests.exe" & if errorlevel 1 set FAIL=1 )
)

echo === 6502 CPU ===
cl /nologo /std:c++17 /EHsc /I "..\cpu_cores" /I "..\sys_general" "cpu_6502_tests.cpp" "..\cpu_cores\cpu_6502.cpp" /Fe:"cpu_6502_tests.exe" 1>build_cpu.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_cpu.log & set FAIL=1 ) else ( "%~dp0cpu_6502_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === VIA 6522 ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" /I "..\sys_general" /I ".." "..\petsrc\via6522.cpp" "..\petsrc\pia6520.cpp" "via6522_tests.cpp" /Fe:"via6522_tests.exe" 1>build_via.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_via.log & set FAIL=1 ) else ( "%~dp0via6522_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === SNES adapter ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" "snes_adapter_tests.cpp" /Fe:"snes_adapter_tests.exe" 1>build_snes.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_snes.log & set FAIL=1 ) else ( "%~dp0snes_adapter_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === CB2 render ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" /I "..\sys_audio" "cb2_render_tests.cpp" /Fe:"cb2_render_tests.exe" 1>build_cb2.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_cb2.log & set FAIL=1 ) else ( "%~dp0cb2_render_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === D64 disk backend ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" /I "..\cpu_cores" /I "..\sys_general" /I "..\sys_audio" /I ".." "..\petsrc\pet2001ieee_ioport.cpp" "..\petsrc\pet2001ieee_d64.cpp" "..\petsrc\pet2001ieee_seq.cpp" "..\petsrc\pet2001ieee_vdrive.cpp" "d64_tests.cpp" /Fe:"d64_tests.exe" 1>build_d64.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_d64.log & set FAIL=1 ) else ( "%~dp0d64_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === host_view ===
cl /nologo /std:c++17 /EHsc /I "..\system" "host_view_tests.cpp" "..\system\host_view.cpp" /Fe:"host_view_tests.exe" 1>build_hostview.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_hostview.log & set FAIL=1 ) else ( "%~dp0host_view_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === PET video ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" "..\petsrc\pet2001video.cpp" "pet2001video_tests.cpp" /Fe:"pet2001video_tests.exe" 1>build_video.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_video.log & set FAIL=1 ) else ( "%~dp0pet2001video_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === MOS 6545 CRTC ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" "..\petsrc\mos6545.cpp" "mos6545_tests.cpp" /Fe:"mos6545_tests.exe" 1>build_crtc.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_crtc.log & set FAIL=1 ) else ( "%~dp0mos6545_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === PET machine integration ===
cl /nologo /std:c++17 /EHsc /MDd /D_DEBUG /I "..\petsrc" /I "..\cpu_cores" /I "..\sys_general" /I "..\sys_audio" /I ".." "..\petsrc\pet_machine.cpp" "..\cpu_cores\cpu_6502.cpp" "..\petsrc\pet_mem.cpp" "..\petsrc\pet2001io.cpp" "..\petsrc\pia6520.cpp" "..\petsrc\via6522.cpp" "..\petsrc\mos6545.cpp" "..\petsrc\pet2001video.cpp" "..\petsrc\pet2001ieee_ioport.cpp" "..\petsrc\pet2001ieee_d64.cpp" "..\petsrc\pet2001ieee_seq.cpp" "..\petsrc\pet2001ieee_vdrive.cpp" "pet_machine_tests.cpp" /Fe:"pet_machine_tests.exe" 1>build_machine.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_machine.log & set FAIL=1 ) else ( "%~dp0pet_machine_tests.exe" & if errorlevel 1 set FAIL=1 )

echo === PRG relink ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" "prg_relink_tests.cpp" /Fe:"prg_relink_tests.exe" 1>build_relink.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_relink.log & set FAIL=1 ) else ( "%~dp0prg_relink_tests.exe" & if errorlevel 1 set FAIL=1 )

echo.
if "%FAIL%"=="0" ( echo ALL TESTS PASSED ) else ( echo SOME TESTS FAILED )
popd
exit /b %FAIL%
