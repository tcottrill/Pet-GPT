<#
.SYNOPSIS
  Downloads the Commodore PET ROM images PetEmu needs and installs them into
  the per-ROM-set folders it expects.

.DESCRIPTION
  PetEmu does not ship ROM firmware in the repo (it's copyrighted Commodore
  software). This script fetches the .bin files from the public zimmers.net
  CBM firmware archive and drops them into:
    <RomsDir>\pet2001\     (original 2001, BASIC 1, 8 KB)
    <RomsDir>\pet2001n\    (2001N / 3000, BASIC 2)
    <RomsDir>\pet4000-9\   (9-inch 4000, discrete video, BASIC 4)
    <RomsDir>\pet4000-12\  (12-inch 4000, 40-column CRTC, 60 Hz)
    <RomsDir>\cbm8032\     (8032, 80-column business CRTC, 60 Hz)
  Every filename here is the same one used on zimmers.net -- no renaming.

.PARAMETER RomsDir
  Destination roms folder. Defaults to a "roms" folder next to this script,
  so running it from inside x64\Release (or a distributed copy of the
  emulator) just works.

.PARAMETER Sets
  Which ROM sets to fetch: Pet2001, Pet2001N, Pet4000-9, Pet4000-12, CBM8032.
  Legacy names Basic2, Basic4, and 8032 remain accepted aliases.

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
    [ValidateSet("Pet2001", "Pet2001N", "Pet4000-9", "Pet4000-12", "CBM8032", "Basic2", "Basic4", "8032")]
    [string[]]$Sets = @("Pet2001", "Pet2001N", "Pet4000-9", "Pet4000-12", "CBM8032"),
    [switch]$Force
)

$BaseUrl = "https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet"

# Later models share character ROMs; each set folder gets its
# own copy so it stays self-contained.
$CharRoms = @("characters-1.901447-08.bin", "characters-2.901447-10.bin")

$Manifest = @{
    "Pet2001" = @(
        "rom-1-c000.901447-01.bin",
        "rom-1-c800.901447-02.bin",
        "rom-1-d000.901447-03.bin",
        "rom-1-d800.901447-04.bin",
        "rom-1-e000.901447-05.bin",
        "rom-1-f000.901447-06.bin",
        "rom-1-f800.901447-07.bin",
        "characters-1.901447-08.bin"
    )
    "Pet2001N" = @(
        "basic-2-c000.901465-01.bin",
        "basic-2-d000.901465-02.bin",
        "edit-2-n.901447-24.bin",
        "kernal-2.901465-03.bin"
    ) + $CharRoms
    "Pet4000-9" = @(
        "basic-4-b000.901465-23.bin",
        "basic-4-c000.901465-20.bin",
        "basic-4-d000.901465-21.bin",
        "edit-4-n.901447-29.bin",
        "kernal-4.901465-22.bin"
    ) + $CharRoms
    "Pet4000-12" = @(
        "basic-4-b000.901465-23.bin",
        "basic-4-c000.901465-20.bin",
        "basic-4-d000.901465-21.bin",
        "edit-4-40-n-60Hz.901499-01.bin",
        "kernal-4.901465-22.bin"
    ) + $CharRoms
    "CBM8032" = @(
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
    $set = switch ($set) {
        "Basic2" { "Pet2001N" }
        "Basic4" { "Pet4000-9" }
        "8032" { "CBM8032" }
        default { $set }
    }
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
