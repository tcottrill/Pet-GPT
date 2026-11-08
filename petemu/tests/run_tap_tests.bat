@echo off
setlocal
set "build=%~dp0..\..\build\windows"
cmake -S "%~dp0..\.." -B "%build%" -A x64 -DBUILD_TESTING=ON
if errorlevel 1 exit /b 1
cmake --build "%build%" --config Release --target tap_tests
if errorlevel 1 exit /b 1
"%build%\Release\tap_tests.exe" %*
exit /b %errorlevel%
