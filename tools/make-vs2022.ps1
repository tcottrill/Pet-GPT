# Generates a Visual Studio 2022 solution, build/vs2022/PetGPT.sln, with CMake.
# Open it, pick Debug or Release, and build or debug pet-gpt (the startup project).
# Release writes dist/pet-gpt.exe, Debug dist/debug/pet-gpt.exe, as tools/build-windows.ps1.
# The wxWidgets libraries are projects in the "Libraries" and "Third Party Libraries" folders.
# CMake downloads the pinned wxWidgets release on the first run (cmake/Dependencies.cmake).
#
# Run it again after adding or removing source files (CMakeLists.txt changes regenerate the
# solution on the next build by themselves). -Open opens the solution afterwards.
param([switch]$Open, [string]$WxSource = '')
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build/vs2022'
$configure = @('-S', $repo, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-DBUILD_TESTING=ON')
if ($WxSource) { $configure += "-DPET_WX_SOURCE=$WxSource" }
cmake @configure
if ($LASTEXITCODE) { throw 'Generating the solution failed' }
$sln = Join-Path $build 'PetGPT.sln'
Write-Host "Solution: $sln"
if ($Open) { Invoke-Item -LiteralPath $sln }
