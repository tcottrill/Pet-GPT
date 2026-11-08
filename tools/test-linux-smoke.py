"""Boot every PET model on native Wayland with isolated runtime data."""
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
build = root / 'build/linux'
runtime = build / 'smoke-data'
runtime.mkdir(parents=True, exist_ok=True)
shutil.copytree(root / 'x64/Release/roms', runtime / 'roms', dirs_exist_ok=True)
shutil.copy2(root / 'x64/Release/pet.ini', runtime / 'pet.ini')
env = os.environ.copy()
env['PETGPT_DATA_DIR'] = str(runtime)
env['GDK_BACKEND'] = 'wayland'
if Path('/mnt/wslg/runtime-dir/wayland-0').exists():
    env['XDG_RUNTIME_DIR'] = '/mnt/wslg/runtime-dir'
    env['WAYLAND_DISPLAY'] = 'wayland-0'
for model in ('-pet2001', '-basic2', '-basic4', '-4032', '-8032'):
    name = model.lstrip('-')
    screenshot = runtime / (name + '.png')
    result = subprocess.run([str(build / 'bin/pet-gpt'), '--smoke-test=' + str(screenshot), model, '-window'],
                            env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=40)
    (runtime / (name + '-console.log')).write_text(result.stdout)
    if result.returncode:
        print(result.stdout)
        raise SystemExit(f'FAIL {model}: exit {result.returncode}')
    log = (runtime / 'petemu-log.txt').read_text()
    (runtime / (name + '.log')).write_text(log)
    if 'Display backend: native Wayland' not in log or not screenshot.exists():
        print(log)
        raise SystemExit(f'FAIL {model}: missing native Wayland proof or screenshot')
    print(f'PASS native Wayland: {model}', flush=True)
