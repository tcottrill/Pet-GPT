<#
.SYNOPSIS
  Downloads the Commodore PET ROM images PetEmu needs and installs them into
  the per-ROM-set folders it expects.

.DESCRIPTION
  PetEmu does not ship ROM firmware in the repo (it's copyrighted Commodore
  software). This script fetches the .bin files from the public zimmers.net
  CBM firmware archive and drops them into:
    <RomsDir>\basic2\   (BASIC 2 / 2001-N editor)
    <RomsDir>\basic4\   (BASIC 4, 40-column)
    <RomsDir>\8032\     (BASIC 4, 80-column / 8032 business set)
  Every filename here is the same one used on zimmers.net -- no renaming.

.PARAMETER RomsDir
  Destination roms folder. Defaults to a "roms" folder next to this script,
  so running it from inside x64\Release (or a distributed copy of the
  emulator) just works.

.PARAMETER Sets
  Which ROM sets to fetch: any of Basic2, Basic4, 8032 (default: all three).

.PARAMETER Force
  Re-download even if a file is already present.

.EXAMPLE
  .\download-roms.ps1
.EXAMPLE
  .\download-roms.ps1 -Sets Basic4,8032
.EXAMPLE
  .\download-roms.ps1 -RomsDir "C:\Source2026\Pet-GPT-2026\x64\Release\roms" -Force
#>
param(
    [string]$RomsDir = (Join-Path $PSScriptRoot "roms"),
    [ValidateSet("Basic2", "Basic4", "8032")]
    [string[]]$Sets = @("Basic2", "Basic4", "8032"),
    [switch]$Force
)

$BaseUrl = "https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet"

# Character ROMs are identical across every set; each set folder gets its
# own copy so it stays self-contained.
$CharRoms = @("characters-1.901447-08.bin", "characters-2.901447-10.bin")

$Manifest = @{
    "Basic2" = @(
        "basic-2-c000.901465-01.bin",
        "basic-2-d000.901465-02.bin",
        "edit-2-n.901447-24.bin",
        "kernal-2.901465-03.bin"
    ) + $CharRoms
    "Basic4" = @(
        "basic-4-b000.901465-23.bin",
        "basic-4-c000.901465-20.bin",
        "basic-4-d000.901465-21.bin",
        "edit-4-n.901447-29.bin",
        "kernal-4.901465-22.bin"
    ) + $CharRoms
    "8032" = @(
        "basic-4-b000.901465-19.bin",
        "basic-4-c000.901465-20.bin",
        "basic-4-d000.901465-21.bin",
        "edit-4-80-b-60Hz.901474-03.bin",
        "kernal-4.901465-22.bin"
    ) + $CharRoms
}

$downloaded = 0
$skipped = 0
$failures = @()

foreach ($set in $Sets) {
    $destDir = Join-Path $RomsDir $set.ToLower()
    New-Item -ItemType Directory -Force -Path $destDir | Out-Null

    Write-Host "== $set -> $destDir ==" -ForegroundColor Cyan
    foreach ($name in $Manifest[$set]) {
        $destPath = Join-Path $destDir $name

        if (-not $Force -and (Test-Path $destPath) -and (Get-Item $destPath).Length -gt 0) {
            Write-Host "  skip   $name (already present)"
            $skipped++
            continue
        }

        $url = "$BaseUrl/$name"
        $ok = $false
        for ($attempt = 1; $attempt -le 2 -and -not $ok; $attempt++) {
            try {
                Invoke-WebRequest -Uri $url -OutFile $destPath -UseBasicParsing -ErrorAction Stop
                if ((Get-Item $destPath).Length -lt 256) {
                    throw "suspiciously small file (got an error page instead of the ROM?)"
                }
                Write-Host "  ok     $name"
                $downloaded++
                $ok = $true
            }
            catch {
                if ($attempt -eq 2) {
                    Write-Host "  FAIL   $name  <- $url  ($($_.Exception.Message))" -ForegroundColor Red
                    $failures += "$set/$name"
                    Remove-Item $destPath -ErrorAction SilentlyContinue
                }
            }
        }
    }
}

Write-Host ""
Write-Host "Downloaded: $downloaded   Already present: $skipped   Failed: $($failures.Count)"
if ($failures.Count -gt 0) {
    Write-Host "Failed files:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  $_" -ForegroundColor Red }
    exit 1
}
exit 0
