# Builds dist/pet-gpt.exe (Release, Ninja, static CRT) in build/windows and runs CTest.
# Run through build.bat. Uses the newest Visual Studio with the C++ tools (vswhere -latest).
#
#   -RomDir <dir>   ROM sets for the frontend and ROM tests (default x64/Release/roms).
#                   They are copied with pet.ini into build/windows/test-data, which
#                   PETGPT_DATA_DIR points at, so the tests never touch your own settings.
#                   Without ROMs those tests report a skip. ROMs are never committed.
#   -WxSource <dir> a local wxWidgets source tree instead of the pinned download.
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$WxSource = '',
    [string]$RomDir = '',
    [ValidateRange(1, 256)]
    [int]$Jobs = 8
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$build = Join-Path $repo 'build/windows'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'No Visual Studio with the C++ tools found' }
Import-Module (Join-Path $vs 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$ninja = Join-Path $vs 'Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe'

# build/windows used to be a Visual Studio build tree; CMake cannot switch its generator.
$cache = Join-Path $build 'CMakeCache.txt'
if ((Test-Path -LiteralPath $cache) -and !(Select-String -LiteralPath $cache -Pattern '^CMAKE_GENERATOR:INTERNAL=Ninja$' -Quiet)) {
    Write-Host "Removing the old non-Ninja build tree $build"
    Remove-Item -LiteralPath $build -Recurse -Force
}
$configure = @('-S', $repo, '-B', $build, '-G', 'Ninja', "-DCMAKE_MAKE_PROGRAM=$ninja",
    "-DCMAKE_BUILD_TYPE=$Configuration", '-DBUILD_TESTING=ON')
if ($WxSource) { $configure += "-DPET_WX_SOURCE=$WxSource" }

if (!$RomDir) { $RomDir = Join-Path $repo 'x64/Release/roms' }
$data = Join-Path $build 'test-data'
$haveRoms = Test-Path -LiteralPath (Join-Path $RomDir 'pet2001n')
if ($haveRoms) { $configure += "-DPET_TEST_ROM_DIR=$((Resolve-Path -LiteralPath $RomDir).Path -replace '\\', '/')" }

cmake @configure
if ($LASTEXITCODE) { throw 'Configure failed' }
cmake --build $build --parallel $Jobs
if ($LASTEXITCODE) { throw 'Build failed' }

if ($haveRoms) {
    if (Test-Path -LiteralPath $data) { Remove-Item -LiteralPath $data -Recurse -Force }
    New-Item -ItemType Directory $data | Out-Null
    Copy-Item -LiteralPath $RomDir -Destination (Join-Path $data 'roms') -Recurse
    $ini = Join-Path (Split-Path $RomDir -Parent) 'pet.ini'
    if (Test-Path -LiteralPath $ini) { Copy-Item -LiteralPath $ini -Destination $data }
    else { New-Item -ItemType File (Join-Path $data 'pet.ini') | Out-Null }
    $env:PETGPT_DATA_DIR = $data
} else {
    Write-Warning "No ROM sets in ${RomDir}: the frontend and ROM tests will be skipped"
}
ctest --test-dir $build --output-on-failure
$failed = $LASTEXITCODE
Remove-Item Env:PETGPT_DATA_DIR -ErrorAction SilentlyContinue
if ($failed) { throw 'Tests failed' }
Write-Host "Executable: $(Join-Path $repo $(if ($Configuration -eq 'Debug') { 'dist/debug/pet-gpt.exe' } else { 'dist/pet-gpt.exe' }))"
