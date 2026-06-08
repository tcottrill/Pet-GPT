<p align="center">
  <img src="images/pet-gpt-logo.png" alt="Pet-GPT" width="240">
</p>

<h1 align="center">Pet-GPT — Commodore PET 2001 Emulator</h1>

<p align="center">
  A modern, modular Commodore PET emulator for Windows — cycle-driven 6502, accurate
  VIA&nbsp;6522 / PIA&nbsp;6520 I/O, full CB2 sound, SNES&nbsp;user-port gamepad support, and a
  bug-fixed HLE IEEE-488 disk drive. Now <strong>version&nbsp;2.0</strong>.
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: GPL v3" src="https://img.shields.io/badge/License-GPLv3-blue.svg"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%20x64-0078D6">
  <img alt="Build" src="https://img.shields.io/badge/build-passing-brightgreen">
  <img alt="Renderer" src="https://img.shields.io/badge/renderer-OpenGL%203.3%20core-5586A4">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599C">
</p>

<p align="center">
  <img src="images/shot-boot.png" alt="Commodore BASIC 4.0 boot screen" width="620">
</p>

---

The **Commodore PET 2001** (1977) was Commodore's first personal computer: a 6502 running
Microsoft BASIC, a 40×25 green-phosphor monochrome display, an IEEE-488 disk bus, and a
chunky chiclet keyboard. **Pet-GPT** recreates it in clean, modular C++17 with a reusable
Win32/OpenGL host shell.

**Version 2.0** is a major overhaul focused on the hardware that the original release only
roughed in:

- a rewritten **VIA 6522** with accurate **CB2 shift-register sound**,
- a rewritten **PIA 6520** I/O core,
- **full sound support** that drives the popular PET sound demos,
- **SNES user-port gamepad** emulation, and
- a **fully bug-fixed HLE disk drive** (D64 images *and* a host-folder virtual drive).

> ⚠️ **ROMs are not included.** Pet-GPT ships no Commodore ROM images. You must supply your
> own legally-obtained PET BASIC / EDIT / KERNAL / character ROMs (see [Running](#-running)).

---

## ✨ Features

- 🧠 **6502 CPU core** — full documented + undocumented opcode set, IRQ/NMI, decimal mode.
- 🎛️ **Accurate VIA 6522** — timers, shift register, CA/CB handshakes, interrupt logic.
- 🔌 **Rewritten PIA 6520 ×2** — PET keyboard scan, IEEE handshake lines, screen-retrace IRQ.
- 🔊 **Full CB2 sound** — the VIA shift-register / CB2 line is reconstructed to PCM in real
  time, so the classic PET sound demos (e.g. *Faulty Robots*) play correctly.
- 🎮 **SNES gamepad support** — an emulated SNES user-port adapter maps an Xbox/XInput pad
  (or WinMM joystick) to PETSCII-Robots-style games. Toggleable in `pet.ini`.
- 💾 **HLE IEEE-488 disk drive (device 8), fully bug-fixed**
  - Mounts **.d64** images (35-track, read **and** write — SAVE/SCRATCH/RENAME/COPY/NEW).
  - **Virtual drive**: serves a host folder (`./files`) as device 8 — `LOAD"NAME",8`,
    `LOAD"$",8`, SEQ read/write, with a real 1541-style directory listing.
  - Persists across resets; ejectable back to the virtual drive at any time.
- 🟢 **CRT look** — authentic green-phosphor tint (the PET was a mono green screen),
  toggleable. (A proper scanline/CRT shader is planned.)
- 🖥️ **Real desktop app** — native menus, integer & free window scaling, Alt-Enter
  fullscreen, drag-and-drop loading, and `.ini`-persisted settings.
- 🧩 **BASIC 2.0 / 4.0** and selectable **RAM size** (4K / 8K / 16K / 32K), from the menu.
- ⚡ **Direct PRG loading** — pick a `.prg` and it's injected into RAM (BASIC programs are
  re-linked so `RUN` just works); the machine reboots cleanly first so a running program
  can't be corrupted.

---

## 🧰 Requirements

| | |
|---|---|
| **OS** | Windows 10 / 11 (x64) |
| **Toolchain** | Visual Studio 2022 (Desktop C++ workload), C++17 |
| **GPU** | OpenGL 3.3 core profile |
| **Audio** | XAudio2 (ships with Windows) |
| **Bundled** | GLEW, stb_image (in `petemu/thirdparty/`) |

---

## 🔨 Building

Open the solution in Visual Studio 2022 and build **Release · x64**:

```text
PetEmu.sln  →  Configuration: Release  ·  Platform: x64  →  Build
```

…or from a Developer PowerShell:

```powershell
msbuild PetEmu.sln /t:Build /p:Configuration=Release /p:Platform=x64 /m
```

The binary is produced at `x64\Release\PetEmu.exe`.

To run the unit tests (VIA, SNES adapter, CB2 sound, D64 disk, PRG relink, host viewport):

```powershell
petemu\tests\run_tests.bat
```

---

## ▶️ Running

Pet-GPT runs from the executable's directory and needs two folders next to `PetEmu.exe`:

**`roms/`** — your Commodore ROM dumps. Pet-GPT ships none. Download the seven files for the
default **BASIC 4.0** set from **zimmers.net** (the canonical Commodore firmware archive) and
drop them into the **`roms/`** folder beside `PetEmu.exe` — i.e. `x64\Release\roms\`:

| ROM | Download (→ `x64\Release\roms\`) | Size |
|---|---|---|
| BASIC 4 `$B000` | [basic-4-b000.901465-23.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-23.bin) | 4 KB |
| BASIC 4 `$C000` | [basic-4-c000.901465-20.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-c000.901465-20.bin) | 4 KB |
| BASIC 4 `$D000` | [basic-4-d000.901465-21.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-d000.901465-21.bin) | 4 KB |
| EDIT 4 (40-col, N) `$E000` | [edit-4-n.901447-29.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-n.901447-29.bin) | 2 KB |
| KERNAL 4 `$F000` | [kernal-4.901465-22.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-4.901465-22.bin) | 4 KB |
| Character ROM 1 | [characters-1.901447-08.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-1.901447-08.bin) | 2 KB |
| Character ROM 2 | [characters-2.901447-10.bin](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-2.901447-10.bin) | 2 KB |

> They're plain `.bin` files — no extraction needed; just save them straight into `roms\`.
> A **BASIC 2.0** set (`basic-2-c000.901465-01.bin`, `basic-2-d000.901465-02.bin`,
> `edit-2-n.901447-24.bin`, `kernal-2.901465-03.bin` + the same two character ROMs) lives in
> [the same zimmers folder](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/);
> select it with **Machine ▸ BASIC 2** or `-basic2`.

**`files/`** — your programs and disks (the virtual drive root): drop `.prg` and `.d64`
files here, then `LOAD"NAME",8` / `LOAD"$",8` from BASIC, or use **File ▸ Load**.

Then just launch:

```powershell
x64\Release\PetEmu.exe
```

### Command line (optional)

```text
PetEmu.exe [program] [options]
  -basic2 | -basic4     select the ROM set
  -disk <file>          mount a .d64, or prime a .prg from ./files
  -rom <file>           load a program/disk at startup
  -scale <1|2|3|fit>    initial window scale       -fullscreen | -window
  -h                    help
```

---

## ⌨️ Controls

The PC keyboard maps onto the PET 8×10 key matrix. Highlights:

| PET key | PC key |
|---|---|
| `RUN/STOP` | `Caps Lock` |
| `STOP` + restore (**BREAK**) | `Caps Lock` + `Shift` |
| Cursor ↑ / ↓ | `↑` / `↓` |
| Cursor ← / → | `Shift +` `↑/↓` (PET has 2 cursor keys) |
| `CLR/HOME` | `Home` |
| Graphics ⇄ business charset | `F11`* |

\* In graphics mode, `Shift`+letter emits the PETSCII graphic for that key.

### 🎮 Gamepad (SNES user-port adapter)

Plug in an Xbox/XInput controller (or any WinMM joystick) and Pet-GPT presents it to the PET
as an **SNES adapter on the user port** — the scheme PETSCII Robots and similar games use.
D-Pad and the face/shoulder/start/select buttons are mapped automatically. Enable/disable or
invert the data line in `pet.ini` (`[input] snes_adapter`, `snes_invert`).

---

## ⚡ Hotkeys

| Key | Action |
|---|---|
| `Ctrl+O` | Load program / disk… |
| `Ctrl+E` | Eject disk (back to the `./files` virtual drive) |
| `Ctrl+R` | Reset |
| `F10` | Toggle CRT look (green phosphor) |
| `F11` / `Alt+Enter` | Toggle fullscreen |
| `Esc` | Leave fullscreen, or quit |

---

## 📂 Menus

```text
File ─┬─ Load Program/Disk…   (Ctrl+O)
      ├─ Eject Disk           (Ctrl+E)
      ├─ Reset                (Ctrl+R)
      └─ Exit
Machine ─┬─ BASIC 2 / BASIC 4         (radio)
         └─ Memory ▸ 4K / 8K / 16K / 32K   (radio)
View ─┬─ Scale 1× / 2× / 3× / Fit
      ├─ CRT (green phosphor)   (F10)
      └─ Fullscreen             (Alt+Enter)
Help ─── About
```

Files can also be **drag-and-dropped** onto the window.

---

## ⚙️ Configuration

Settings live in `pet.ini` next to the executable and are written back on exit:

| Section / key | Values | Notes |
|---|---|---|
| `[machine] basic` | `2`, `4` | BASIC ROM set |
| `[machine] ram` | `4`, `8`, `16`, `32` | RAM size in KB (default 32) |
| `[video] scale` | `0`=Fit, `1`/`2`/`3` | window scale preset |
| `[video] fullscreen` | `0`/`1` | start fullscreen |
| `[video] crt` | `0`/`1` | green-phosphor CRT look |
| `[video] crt_tint` | `0`/`1` | apply the green tint |
| `[input] snes_adapter` | `0`/`1` | emulate the user-port SNES pad |
| `[input] snes_invert` | `0`/`1` | invert the adapter data line |
| `[paths] lastromdir` | path | remembered Load… directory |

---

## 🗂️ Project layout

```text
Pet-GPT-2026/
├─ PetEmu.sln
├─ petemu/
│  ├─ pet_host_app.cpp         # thin WinMain → host shell
│  ├─ emulator.cpp             # machine wiring, ROM loading, frame loop
│  ├─ system/                  # reusable Win32/OpenGL host shell (window, menu, scaling)
│  ├─ petsrc/                  # PET hardware: 6502, VIA 6522, PIA 6520, video, IEEE/D64
│  ├─ sys_audio/               # mixer + CB2 → PCM reconstruction
│  ├─ sys_general/ sys_gl/     # logging, GL 3.3 core context
│  ├─ thirdparty/              # GLEW, stb_image
│  └─ tests/                   # standalone unit tests (run_tests.bat)
└─ x64/Release/
   ├─ roms/                    # ← put ROM dumps here
   └─ files/                   # ← put .prg / .d64 here (virtual drive)
```

---

## 👥 Contributors

- **Tim Cottrill** ([@tcottrill](https://github.com/tcottrill)) — author, integrator, maintainer.
- Built with AI pair-programming: **ChatGPT** (original 1.0) and **Claude** (the 2.0 rewrite —
  VIA/PIA, sound, SNES, HLE disk, and the host shell).

## 🙏 Acknowledgements

- **Thomas Skibo** — his JavaScript Commodore PET emulator was the architectural starting point.
- **MAME / MESS** developers — PET timing, IEEE-488 structure, and memory-map validation.
- **Michael Steil** — [cbmbus](https://github.com/mist64/cbmbus_doc) IEEE-488 / Commodore DOS notes.
- **unusedino.de** — the canonical [D64 format reference](http://unusedino.de/ec64/technical/formats/d64.html).
- The PET community for ROM documentation and the sound/joystick demos used as test cases.

---

## 📜 License

Released under the **GNU General Public License v3.0**.

```text
Pet-GPT — Commodore PET 2001 Emulator
Copyright (C) 2026 Tim Cottrill

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.
```

Commodore ROM images are **not** distributed with this project and remain the property of
their respective rights holders. Bundled third-party libraries (GLEW, stb_image) retain
their own licenses.
