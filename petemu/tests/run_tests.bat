@echo off
setlocal enabledelayedexpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
pushd "%~dp0"

set FAIL=0

echo === VIA 6522 ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" /I "..\sys_general" /I ".." "..\petsrc\via6522.cpp" "via6522_tests.cpp" /Fe:"via6522_tests.exe" 1>build_via.log 2>&1
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

echo === PRG relink ===
cl /nologo /std:c++17 /EHsc /I "..\petsrc" "prg_relink_tests.cpp" /Fe:"prg_relink_tests.exe" 1>build_relink.log 2>&1
if errorlevel 1 ( echo BUILD FAILED & type build_relink.log & set FAIL=1 ) else ( "%~dp0prg_relink_tests.exe" & if errorlevel 1 set FAIL=1 )

echo.
if "%FAIL%"=="0" ( echo ALL TESTS PASSED ) else ( echo SOME TESTS FAILED )
popd
exit /b %FAIL%
