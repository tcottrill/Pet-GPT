@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\tools\build-windows.ps1" %*
exit /b %errorlevel%
