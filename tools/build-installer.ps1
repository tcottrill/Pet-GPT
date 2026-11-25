# Builds the Windows installer, build/installer/Pet-GPT-<version>-windows-x64-setup.exe
# (collect-release.ps1 copies it to dist/release), from packaging/windows/pet-gpt.iss.
# Run after tools/build-windows.ps1. Needs Inno Setup 6 (https://jrsoftware.org/isinfo.php,
# or "winget install JRSoftware.InnoSetup"); pass -Iscc when ISCC.exe is somewhere else.
param([string]$Iscc)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if (!$Iscc) {
    $found = @(@(Get-Command ISCC.exe -ErrorAction SilentlyContinue | ForEach-Object Source) + @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { $_ -and (Test-Path -LiteralPath $_) })
    if (!$found) { throw 'Inno Setup 6 (ISCC.exe) is not installed: winget install JRSoftware.InnoSetup' }
    $Iscc = $found[0]
}
if (!(Test-Path -LiteralPath (Join-Path $repo 'dist/pet-gpt.exe'))) { throw 'dist/pet-gpt.exe is missing: run tools/build-windows.ps1 first' }
& $Iscc /Q (Join-Path $repo 'packaging/windows/pet-gpt.iss')
if ($LASTEXITCODE) { throw 'Building the installer failed' }
Get-ChildItem (Join-Path $repo 'build/installer/Pet-GPT-*-windows-x64-setup.exe') | Sort-Object LastWriteTime | Select-Object -Last 1 |
    ForEach-Object { Write-Host "Installer: $($_.FullName) ($([math]::Round($_.Length / 1MB, 1)) MB)" }
