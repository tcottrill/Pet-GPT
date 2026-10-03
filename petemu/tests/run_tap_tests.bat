@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
pushd "%~dp0"
cl /nologo /std:c++17 /EHsc /O2 /I "..\petsrc" /I "..\cpu_cores" /I "..\sys_general" /I "..\sys_audio" /I ".." tap_tests.cpp ..\petsrc\pet_roms.cpp ..\petsrc\pet_machine.cpp ..\cpu_cores\cpu_6502.cpp ..\petsrc\pet_mem.cpp ..\petsrc\pet2001io.cpp ..\petsrc\pia6520.cpp ..\petsrc\via6522.cpp ..\petsrc\mos6545.cpp ..\petsrc\pet2001video.cpp ..\petsrc\pet2001ieee_ioport.cpp ..\petsrc\pet2001ieee_d64.cpp ..\petsrc\pet2001ieee_seq.cpp ..\petsrc\pet2001ieee_vdrive.cpp /Fe:tap_tests.exe >build_tap.log 2>&1
if errorlevel 1 (type build_tap.log & popd & exit /b 1)
tap_tests.exe %*
set result=%errorlevel%
popd
exit /b %result%
