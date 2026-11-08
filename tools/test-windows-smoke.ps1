param([string]$BuildDir = 'build/windows')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root $BuildDir
$runtime = Join-Path $build 'smoke-data'
New-Item -ItemType Directory -Force $runtime | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'x64/Release/roms') -Destination $runtime -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'x64/Release/pet.ini') -Destination $runtime -Force
$env:PETGPT_DATA_DIR = $runtime
$exe = Join-Path $build 'bin/Release/pet-gpt.exe'
foreach ($model in @('-pet2001','-basic2','-basic4','-4032','-8032')) {
    $image = Join-Path $runtime ($model.TrimStart('-') + '.png')
    $arguments = @('--smoke-test=' + $image, $model, '-window')
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    if (-not $process.WaitForExit(30000)) { $process.Kill(); throw "Smoke timed out: $model" }
    if ($process.ExitCode -ne 0) { throw "Smoke failed: $model, exit $($process.ExitCode)" }
    if (-not (Test-Path -LiteralPath $image)) { throw "Screenshot missing: $model" }
    Copy-Item -LiteralPath (Join-Path $runtime 'petemu-log.txt') -Destination (Join-Path $runtime ($model.TrimStart('-') + '.log')) -Force
    Write-Output "PASS: $model"
}
