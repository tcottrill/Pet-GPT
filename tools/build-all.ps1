# The complete build, from a Windows checkout: the Windows program and its installer, then
# the Linux AppImage and Flatpak through WSL, then dist/release/ for a GitHub release.
#
# Needs Visual Studio 2022 with the C++ tools, Inno Setup 6 (tools/build-installer.ps1) and,
# for Linux, a WSL distribution with bash, curl, bzip2, bubblewrap and flatpak with the
# org.freedesktop.Sdk//26.08 SDK (see docs/porting.md). -SkipLinux builds only Windows.
#
# -RomDir: your ROM sets (default x64/Release/roms), used only by the tests and smoke tests
# on both systems. No release file contains ROMs.
param([switch]$SkipLinux, [string]$Distro = 'Ubuntu', [string]$RomDir = '', [int]$Jobs = 8)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
function Step($Name) { Write-Host "`n== $Name" -ForegroundColor Cyan }
if (!$RomDir) { $RomDir = Join-Path $repo 'x64/Release/roms' }

Step 'Windows program and tests'
& (Join-Path $PSScriptRoot 'build-windows.ps1') -Jobs $Jobs -RomDir $RomDir
Step 'Windows installer'
& (Join-Path $PSScriptRoot 'build-installer.ps1')

if (!$SkipLinux) {
    $root = (& wsl.exe -d $Distro -e wslpath -a ($repo -replace '\\', '/')).Trim()
    if ($LASTEXITCODE -or !$root) { throw "WSL distribution '$Distro' is not available" }
    $roms = ''
    if (Test-Path -LiteralPath $RomDir) { $roms = (& wsl.exe -d $Distro -e wslpath -a ((Resolve-Path -LiteralPath $RomDir).Path -replace '\\', '/')).Trim() }
    Step 'Linux AppImage (Ubuntu 22.04 root) and Linux tests'
    & wsl.exe -d $Distro -e env "PET_TEST_ROM_DIR=$roms" bash "$root/tools/build-linux-appimage.sh"
    if ($LASTEXITCODE) { throw 'AppImage build failed' }
    Step 'Flatpak (Freedesktop 26.08 SDK)'
    & wsl.exe -d $Distro -e bash "$root/tools/package-flatpak.sh"
    if ($LASTEXITCODE) { throw 'Flatpak packaging failed' }
}

Step 'Release files'
& (Join-Path $PSScriptRoot 'collect-release.ps1')
