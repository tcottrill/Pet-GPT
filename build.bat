@echo off
rem Builds dist\pet-gpt.exe (wxWidgets, static CRT, no redistributables needed) and runs the tests.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build-windows.ps1" %*
