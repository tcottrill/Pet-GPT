[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',
    [string]$BuildDir = 'build/windows',
    [string]$WxSource = '',
    [ValidateRange(1, 256)]
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$sourceRoot = Split-Path -Parent $PSScriptRoot
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $sourceRoot $BuildDir }
$configureArgs = @('-S', $sourceRoot, '-B', $BuildDir, '-A', 'x64', '-DBUILD_TESTING=ON')
if ($WxSource) { $configureArgs += "-DPET_WX_SOURCE=$WxSource" }
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configuration failed ($LASTEXITCODE)." }
& cmake --build $BuildDir --config $Configuration --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
& ctest --test-dir $BuildDir -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "CTest failed ($LASTEXITCODE)." }
Write-Host "Executable: $(Join-Path $BuildDir "bin/$Configuration/pet-gpt.exe")"
