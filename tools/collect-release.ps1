# Gathers the files for a GitHub release into dist/release/, named with the version from
# CMakeLists.txt, with SHA256SUMS.txt:
#   Pet-GPT-<version>-windows-x64-setup.exe     installer (tools/build-installer.ps1)
#   Pet-GPT-<version>-windows-x64-portable.zip  pet-gpt.exe to run from any folder; its empty
#                                               pet.ini keeps settings, ROMs and programs
#                                               in that folder (portable mode)
#   Pet-GPT-<version>-linux-x86_64.AppImage     tools/build-linux-appimage.sh
#   Pet-GPT-<version>-linux-x86_64.flatpak      tools/package-flatpak.sh
# Run after those builds, or use tools/build-all.ps1 for all of it. Missing packages are
# reported and skipped. No ROMs go into any of them (the testing Flatpak is never collected).
# Upload everything in dist/release/ to the release for the tag.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dist = Join-Path $repo 'dist'
$version = [regex]::Match((Get-Content (Join-Path $repo 'CMakeLists.txt') -Raw), 'project\(\w+ VERSION ([0-9.]+)').Groups[1].Value
if (!$version) { throw 'No VERSION in CMakeLists.txt' }
$out = Join-Path $dist 'release'
if (Test-Path -LiteralPath $out) { Remove-Item -LiteralPath $out -Recurse -Force }
New-Item -ItemType Directory $out | Out-Null

$exe = Join-Path $dist 'pet-gpt.exe'
$wxLicence = Join-Path $repo 'build/windows/_deps/wx-src/docs/licence.txt'
if (Test-Path -LiteralPath $exe) {
    $stage = Join-Path $out "Pet-GPT-$version-windows-x64-portable"
    New-Item -ItemType Directory $stage | Out-Null
    Copy-Item -LiteralPath $exe, (Join-Path $dist 'download-roms.ps1'), (Join-Path $repo 'README.md'),
        (Join-Path $repo 'THIRD-PARTY-NOTICES.md') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination (Join-Path $stage 'LICENSE.txt')
    Copy-Item -LiteralPath $wxLicence -Destination (Join-Path $stage 'wxWidgets-licence.txt')
    New-Item -ItemType File (Join-Path $stage 'pet.ini') | Out-Null
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath "$stage.zip"
    Remove-Item -LiteralPath $stage -Recurse -Force
} else { Write-Warning "No Windows build ($exe)" }
$setup = Join-Path $repo "build/installer/Pet-GPT-$version-windows-x64-setup.exe"
if (Test-Path -LiteralPath $setup) { Copy-Item -LiteralPath $setup -Destination $out }
else { Write-Warning "No Windows installer ($setup): run tools/build-installer.ps1" }

$linux = Join-Path $dist 'linux'
foreach ($package in @(@('Pet-GPT-x86_64.AppImage', "Pet-GPT-$version-linux-x86_64.AppImage"),
                       @('Pet-GPT-x86_64.flatpak', "Pet-GPT-$version-linux-x86_64.flatpak"))) {
    $file = Join-Path $linux $package[0]
    if (Test-Path -LiteralPath $file) { Copy-Item -LiteralPath $file -Destination (Join-Path $out $package[1]) }
    else { Write-Warning "No $($package[0]) in $linux" }
}

$sums = Get-ChildItem -LiteralPath $out -File | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
Set-Content -LiteralPath (Join-Path $out 'SHA256SUMS.txt') -Value $sums -Encoding ascii
Get-ChildItem -LiteralPath $out | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
