"""Verify installed PET bundles use the supported runtime and native Wayland."""
import configparser
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
APP = 'io.github.tcottrill.Pet-GPT'
# Runs with the session's own environment. Do not override XDG_RUNTIME_DIR:
# Flatpak portals and sandboxed image loaders resolve instance metadata there,
# and under WSLg pointing it at /mnt/wslg/runtime-dir breaks portals.

for branch in ('preview', 'testing'):
    ref = f'{APP}//{branch}'
    runtime = subprocess.check_output(
        ['flatpak', 'info', '--user', '--show-runtime', ref], text=True).strip()
    if runtime != 'org.freedesktop.Platform/x86_64/26.08':
        raise SystemExit(f'{branch}: unexpected runtime {runtime}')
    location = Path(subprocess.check_output(
        ['flatpak', 'info', '--user', '--show-location', ref], text=True).strip())
    metadata = configparser.ConfigParser()
    metadata.read(location / 'metadata')
    if metadata['Application']['sdk'] != 'org.freedesktop.Sdk/x86_64/26.08':
        raise SystemExit(f'{branch}: unexpected SDK')
    bundled_roms = location / 'files/share/pet-gpt/roms'
    if branch == 'preview' and bundled_roms.exists():
        raise SystemExit('ROM-free preview unexpectedly contains ROMs')
    if branch == 'testing':
        for folder in ('pet2001', 'pet2001n', 'pet4000-9', 'pet4000-12', 'cbm8032'):
            if not list((bundled_roms / folder).glob('*.bin')):
                raise SystemExit(f'testing: missing bundled ROM set {folder}')
    # Use isolated writable data, never the installed app's user settings.
    data = ROOT / 'build' / 'flatpak-smoke' / branch
    data.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / 'x64/Release/roms', data / 'roms', dirs_exist_ok=True)
    shutil.copy2(ROOT / 'x64/Release/pet.ini', data / 'pet.ini')
    for model in ('pet2001', 'basic2', 'basic4', '4032', '8032'):
        screenshot = data / f'{model}.png'
        screenshot.unlink(missing_ok=True)
        result = subprocess.run(
            ['flatpak', 'run', '--user', f'--filesystem={data}',
             f'--env=PETGPT_DATA_DIR={data}', '--env=GDK_BACKEND=wayland',
             ref, f'--smoke-test={screenshot}', f'-{model}', '-window'],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=45)
        (data / f'{model}-console.log').write_text(result.stdout)
        if result.returncode:
            raise SystemExit(f'{branch}/{model}: exit {result.returncode}\n{result.stdout}')
        log = (data / 'petemu-log.txt').read_text()
        (data / f'{model}.log').write_text(log)
        if 'Display backend: native Wayland' not in log or not screenshot.exists():
            raise SystemExit(f'{branch}/{model}: missing native Wayland proof or screenshot')
        print(f'PASS {branch}/{model}: runtime 26.08, native Wayland', flush=True)
