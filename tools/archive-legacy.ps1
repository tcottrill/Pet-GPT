$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$archive = Join-Path $root 'backups/legacy-win32'
$files = @(
 'PetEmu.sln','petemu/petemu.vcxproj','petemu/petemu.vcxproj.filters',
 'petemu/pet_host_app.cpp','petemu/petemu.rc','petemu/resource.h','petemu/targetver.h',
 'petemu/system/host_window.cpp','petemu/system/host_window.h','petemu/system/host_app.h','petemu/system/host_resource.h',
 'petemu/system/monitor_select.cpp','petemu/system/monitor_select.h','petemu/system/FrameLimiter.h',
 'petemu/system/wintimer.cpp','petemu/system/wintimer.h','petemu/system/utf8conv.h','petemu/system/path_helper.cpp',
 'petemu/sys_general/sys_gl.cpp','petemu/sys_general/sys_gl.h','petemu/sys_general/fast_poly.cpp','petemu/sys_general/fast_poly.h',
 'petemu/sys_general/debug_draw.h','petemu/sys_general/fileio.cpp','petemu/sys_general/fileio.h',
 'petemu/sys_audio/helper_functions.cpp','petemu/sys_audio/helper_functions.h',
 'petemu/sys_audio/mixer.cpp.legacy','petemu/sys_audio/mixer.h.legacy',
 'petemu/sys_input/Joystick.cpp.legacy','petemu/sys_input/joystick.h.legacy',
 'tools/port-core.py','tools/port-runtime.py','tools/stage-roms.py'
)
foreach ($relative in $files) {
 $source = [IO.Path]::GetFullPath((Join-Path $root $relative))
 $destination = [IO.Path]::GetFullPath((Join-Path $archive $relative))
 if (-not $source.StartsWith($root + [IO.Path]::DirectorySeparatorChar) -or
     -not $destination.StartsWith($archive + [IO.Path]::DirectorySeparatorChar)) { throw 'Path outside workspace' }
 if (Test-Path -LiteralPath $source) {
   New-Item -ItemType Directory -Force (Split-Path -Parent $destination) | Out-Null
   Move-Item -LiteralPath $source -Destination $destination
 }
}
Write-Output 'Archived obsolete Win32 frontend and original mixer/controller backups.'
