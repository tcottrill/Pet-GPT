<p align="center">
  <img src="images/pet-gpt-logo.png" alt="Pet-GPT" width="240">
</p>

<h1 align="center">Pet-GPT — Commodore PET/CBM Emulator</h1>

<p align="center">
  A modern, modular Commodore PET emulator for Windows and Linux — cycle-driven 6502, accurate
  VIA&nbsp;6522 / PIA&nbsp;6520 I/O, full CB2 sound, a mono-monitor CRT shader,
  <strong>five PET/CBM models</strong> from the original 2001 to the 80-column 8032,
  <strong>.tap cassette playback</strong> and <strong>.t64</strong> loading, SNES&nbsp;user-port
  gamepad support, and a bug-fixed HLE IEEE-488 disk drive with <strong>.d64 and .d71</strong>
  images. Now <strong>version&nbsp;2.7</strong>.
</p>

<p align="center">
  <a href="LICENSE"><img alt="License: GPL v3" src="https://img.shields.io/badge/License-GPLv3-blue.svg"></a>
  <img alt="Platform" src="https://img.shields.io/badge/platform-Windows%20%7C%20Linux-0078D6">
  <img alt="Renderer" src="https://img.shields.io/badge/renderer-OpenGL%203.3%20core-5586A4">
  <img alt="C++" src="https://img.shields.io/badge/C%2B%2B-17-00599C">
</p>

<p align="center">
  <img src="images/shot-boot.png" alt="Commodore BASIC 4.0 boot screen" width="620">
</p>

<p align="center">
  <img src="images/pet_menu_lr.gif" alt="Pet-GPT running — menus, CRT look, and BASIC" width="620">
</p>

**▶ The _Faulty Robots_ CB2 sound demo running in Pet-GPT (video, with audio):**

https://github.com/user-attachments/assets/0e11d07d-d96a-43b2-86fe-575940d97524

<sub>If the player above doesn't load (some mirrors strip it), <a href="images/PetGPT-Robots.mp4">click here to watch the video</a>.</sub>

---

## 🚀 Quick start

1. Download the latest release zip from the [GitHub Releases page](https://github.com/tcottrill/Pet-GPT/releases) and unzip it anywhere.
2. Run the program.
3. **ROMs are not included.** On the first start, when no PET ROM set can be loaded, a **ROM setup** window opens. It shows the folder Pet-GPT reads ROMs from. **Download ROMs** fetches them for you from zimmers.net (this needs an internet connection and PowerShell), or use **Open ROM folder** and copy your own ROM files into the per-model sub-folders (the exact file names are listed [below](#roms--your-commodore-rom-dumps-pet-gpt-ships-none)). Then press **Try again**.
4. Use **File ▸ Load Program / Disk / Tape...** (`Ctrl+O`), or drag a `.prg`, `.d64` or `.tap` file (`.t64`, `.d71` also work) onto the window. A BASIC program starts by itself; for a disk or tape image the title bar tells you what to type next.

<p align="center">
  Note to anyone actually stumbling on this repository, this actually is a halfway decent
  pet emulator, and the hle disk drive handles about 95% of the use cases. I am really happy with
  it and if you give it a couple of minutes I think you will be too. The 6502 core and via6522
  have been highly debugged and are suitable for using in just about any other C++ project.
</p>

---

The **Commodore PET 2001** (1977) was Commodore's first personal computer: a 6502 running
Microsoft BASIC, a 40×25 green-phosphor monochrome display, an IEEE-488 disk bus, and a
chunky chiclet keyboard. **Pet-GPT** recreates it in clean, modular C++17 with a reusable
wxWidgets/OpenGL desktop frontend.

**Version 2.0** was a ground-up rewrite of the hardware the original release only roughed
in: a cycle-driven **6502**, an accurate **VIA 6522** with **CB2 shift-register sound**, a
rewritten **PIA 6520** I/O core, **full PCM sound**, **SNES user-port gamepad** support, and
a **bug-fixed HLE disk drive**.

**Version 2.5** builds on that with the business machines and the mono CRT look:

- **PET 4000 / CBM 8032** support — selectable **40- and 80-column** machines with a functional **MOS 6545
  CRTC**,
- a **mono-monitor CRT shader** (VICE-style softness + halation, green or B&W, live-tunable),
- **.d71** (1571 double-sided) disk images, read **and** write, plus DOS **block/direct-access
  commands** (the Zork/Infocom disk API),
- a proper **4:3** display (fullscreen no longer stretches), a **2× speed** toggle, and the
  **SNES adapter** exposed as a menu option.

**Version 2.6** — *the one with the features* — fills out the family tree and adds tape:

- **Five machine models** — the **original PET 2001** (BASIC 1, 8 KB), **2001N / 3000**
  (BASIC 2), **4000 9-inch** (BASIC 4, discrete video), the new **4000 12-inch** (40-column
  **CRTC**, 60 Hz), and the **CBM 8032** (80-column) — all from **Machine ▸ Model**.
- **TAP cassette playback** — attach a `.tap` to cassette port 1 and drive it from
  **File ▸ Tape** (Play / Stop / Rewind / Eject); the real PET ROM does the loading.
- **T64 archive loading** — single-program archives load directly; multi-program archives
  show a picker.
- **ROM sets reorganized** into one folder per model, with an updated `download-roms.ps1`
  that fetches all five (old set names still accepted).
- A broad accuracy and robustness pass across the 6502, VIA, CRTC video and the disk drive.

**Version 2.7** — *the one with the configurable keyboard* — makes the PC side yours:

- **Keyboard mapping editor** — **Machine ▸ Keyboard Mapping...** shows a resizable keyboard
  diagram; click a PC key, press the key whose PET function you want, and it's remapped
  (saved to `pet.ini`). Shift-layer preview, per-key and full restore.
- **Per-monitor DPI and multi-monitor** — sharp menus and dialogs on any scaling, DPI-aware
  1×/2×/3× presets (presets too big for the monitor are grayed out), a **Fit** that fills
  the work area at 4:3, and a choosable start monitor (`-monitor N` / `[video]
  starting_monitor`).

> ⚠️ **ROMs are not included.** Pet-GPT ships no Commodore ROM images. You must supply your
> own legally-obtained PET BASIC / EDIT / KERNAL / character ROMs. On a first run without ROMs
> Pet-GPT shows a **ROM setup** window with the exact folder, a **Download ROMs** button (runs the
> bundled script) and an **Open ROM folder** button. If the chosen model's ROMs are missing but another
> set is present, that one starts instead, and models without ROMs are greyed out in **Machine**.

---

## ✨ Features

- 🧠 **6502 CPU core** — full documented + undocumented opcode set, IRQ/NMI, decimal mode
  (synced byte-for-byte with the AAE core).
- 🎛️ **Accurate VIA 6522** — timers, shift register, CA/CB handshakes, interrupt logic,
  datasheet-accurate (tier-2, test-first).
- 🔌 **Rewritten PIA 6520 ×2** — PET keyboard scan, IEEE handshake lines, screen-retrace IRQ.
- 🔊 **Full CB2 sound** — the VIA shift-register / CB2 line is reconstructed to PCM in real
  time, so the classic PET sound demos (e.g. *Faulty Robots*) play correctly.
- 🖳 **PET models** — **original 2001 (BASIC 1, 8 KB)**, **2001N/3000 (BASIC 2)**,
  **4000 9-inch (BASIC 4)**, **4000 12-inch CRTC**, and **8032 (80-column)**; the CRTC models
  use a functional **MOS 6545 CRTC** with CRTC-derived frame timing.
- 📼 **TAP cassette playback** — `.tap` v0/v1 images on cassette port 1, read by the real PET
  ROM at tape speed, with motor control and a **File ▸ Tape** transport.
- 📦 **T64 archives** — load a program straight into RAM, with a picker for multi-program
  archives.
- 📺 **Mono CRT shader** — VICE-style horizontal softness + halation glow, **green phosphor**
  or **black & white**, with beam-overdrive contrast and black-level lift. Live-tunable
  (F9 / PgUp-PgDn / F8) and saved to `pet.ini`.
- 🎮 **SNES gamepad support** — an emulated SNES user-port adapter maps a supported native controller to PETSCII-Robots-style games. Toggle in **Machine ▸ SNES Adapter**.
- 💾 **HLE IEEE-488 disk drive (device 8), fully bug-fixed**
  - Mounts **.d64** (1541, 35-track) **and .d71** (1571 double-sided, 70-track) images —
    read **and** write (SAVE / SCRATCH / RENAME / COPY / NEW).
  - **DOS block / direct-access commands** — `OPEN "#"` buffer channels, `U1`/`U2` block
    read/write, `B-P`, `B-A`/`B-F` — the API Zork-style Infocom interpreters use.
  - SEQ / PRG / USR named-file reads with CBM wildcards; append (`,A`) and `@`-replace.
  - **Virtual drive**: serves a host folder (`./files`) as device 8 — `LOAD"NAME",8`,
    `LOAD"$",8`, with a real 1541-style directory listing.
  - On the original PET 2001 (BASIC 1), whose ROM disk routines never worked on the real
    machine, Pet-GPT handles `LOAD`, `VERIFY` and `SAVE` on device 8 itself.
  - Persists across resets; ejectable back to the virtual drive at any time.
- 🖥️ **Real desktop app** — native menus, integer & free window scaling, proper **4:3**
  presentation, Alt-Enter fullscreen, drag-and-drop loading, and `.ini`-persisted settings.
- ⚡ **2× speed** toggle for slow text adventures; **direct PRG loading** (BASIC programs are
  re-linked so `RUN` just works; the machine reboots cleanly first).

---

## 🧰 Requirements

| | |
|---|---|
| **OS** | Windows 10 / 11 x64, or Linux desktop with GTK3 |
| **Toolchain** | CMake 3.24+, C++17; Visual Studio 2022 on Windows, GCC/Clang on Linux |
| **GUI** | wxWidgets 3.3.3 (fetched by CMake, or `PET_WX_SOURCE`) |
| **GPU** | OpenGL 3.3 core; GTK3/EGL with native Wayland preferred on Linux |
| **Audio / input** | Native Windows backends; ALSA and evdev on Linux, optional PipeWire |
| **Bundled** | GLEW, stb_image and miniz; no SDL or GLFW dependency |

## 🔨 Building

CMake is the authoritative build for the shared Windows/Linux application. The old
Visual Studio/WinMain frontend is historical code, not an alternate supported build.

Windows, from the source root:

```powershell
cmake -S . -B build/windows -A x64
cmake --build build/windows --config Release --parallel
ctest --test-dir build/windows -C Release --output-on-failure
```

Linux, after installing the packages listed in [porting and build notes](docs/porting.md):

```sh
cmake -S . -B build/linux -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux --parallel
ctest --test-dir build/linux --output-on-failure
```

The helper scripts `tools/build-windows.ps1` and `tools/build-linux.sh` configure,
build, and run CTest. GUI tests need a desktop session; use Xvfb for headless Linux
control tests. Native Wayland rendering needs a separate Wayland smoke run.

Executables: `build/windows/bin/Release/pet-gpt.exe` and `build/linux/bin/pet-gpt`.
CMake fetches wxWidgets 3.3.3 on the first GUI configuration; an existing checkout
can be supplied with `-DPET_WX_SOURCE=/path/to/wxWidgets`.

## ▶️ Running

Resource and writable paths are resolved independently of the launch directory.
For an explicit portable data root containing `roms/`, `files/`, and `pet.ini`:

```powershell
$env:PETGPT_DATA_DIR = 'C:\PET-data'
.\build\windows\bin\Release\pet-gpt.exe
```

```sh
PETGPT_DATA_DIR="$HOME/PET-data" ./build/linux/bin/pet-gpt
```

Without this override, an existing writable `pet.ini` beside the resources enables
portable mode. Otherwise:

| | Settings (`pet.ini`) and log | Programs, ROMs, screenshots |
|---|---|---|
| **Windows** | `%APPDATA%\Pet-GPT` | `%APPDATA%\Pet-GPT\files`, `roms`, `screenshots` |
| **Linux** | `~/.local/share/pet-gpt` (`$XDG_DATA_HOME/pet-gpt`) | `~/Pet-GPT/files`, `roms`, `screenshots` |

On Linux the folders you add to are in plain sight in `~/Pet-GPT` (the program's
own folder, and dot-folders, are hidden on SteamOS), and a `README.txt` there says
what goes where. Resetting the settings folder never touches them. A folder from an
older build, `~/.Pet-GPT`, is moved into this layout on first start; nothing that
already exists is overwritten, and anything left over is listed in the log.
User-supplied ROMs override installed resources. See
[path selection](docs/porting.md#resources-settings-and-user-files) for details.

### `roms/` — your Commodore ROM dumps (Pet-GPT ships none)

ROM sets live in **per-model subfolders**, each self-contained:

```text
PET-data/roms/
├─ pet2001\     Original PET 2001, BASIC 1, 8 KB, original character ROM
├─ pet2001n\    PET 2001N / 3000, BASIC 2, discrete video
├─ pet4000-9\   PET 4000, 9-inch, BASIC 4, discrete video
├─ pet4000-12\  PET 4000, 12-inch, BASIC 4, 40-column CRTC, 60 Hz
└─ cbm8032\     CBM 8032, BASIC 4, 80-column CRTC, 60 Hz
```

**ROM fetch script.** ROMs are never included in builds or installed packages. The ROM
setup window's **Download ROMs** button runs `download-roms.ps1` (kept next to the executable,
or in a `tools` folder beside it) with PowerShell (`powershell.exe` on Windows, `pwsh` on Linux)
and fetches the sets from zimmers.net into the folder Pet-GPT reads. You can also run it by
hand; without `-RomsDir` it uses a `roms` folder beside the script when a `pet.ini` or `roms`
folder is there (portable layout), otherwise `%APPDATA%\Pet-GPT\roms` on Windows and
`~/Pet-GPT/roms` elsewhere. From the source root in PowerShell:

```powershell
.\petemu\tools\download-roms.ps1
# Another destination:
# .\petemu\tools\download-roms.ps1 -RomsDir 'C:\PET-data\roms'
```

**By hand** — download the plain `.bin` files from the
[zimmers PET folder](https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/) and drop
them into the matching subfolder (no extraction needed). Each set also needs the two shared
character ROMs (`characters-1.901447-08.bin`, `characters-2.901447-10.bin`):

| Folder | ROMs |
|---|---|
| `roms\pet2001\` | Seven `rom-1-*.901447-0*.bin` ROMs (C000, C800, D000, D800, E000, F000, F800) plus `characters-1.901447-08.bin` |
| `roms\pet2001n\` | `basic-2-c000.901465-01.bin`, `basic-2-d000.901465-02.bin`, `edit-2-n.901447-24.bin`, `kernal-2.901465-03.bin` + chars |
| `roms\pet4000-9\` | `basic-4-b000.901465-23.bin`, `basic-4-c000.901465-20.bin`, `basic-4-d000.901465-21.bin`, `edit-4-n.901447-29.bin`, `kernal-4.901465-22.bin` + chars |
| `roms\pet4000-12\` | Same BASIC/KERNAL/character ROMs as pet4000-9, with `edit-4-40-n-60Hz.901499-01.bin` replacing the editor |
| `roms\cbm8032\` | `basic-4-b000.901465-19.bin`, `basic-4-c000.901465-20.bin`, `basic-4-d000.901465-21.bin`, `edit-4-80-b-60Hz.901474-03.bin`, `kernal-4.901465-22.bin` + chars |

Pick the model at runtime with **Machine ▸ Model**, `-pet2001` / `-basic2` / `-basic4` / `-4032` / `-8032`, or `[machine] basic`
in `pet.ini`. Existing values 2/4/8 retain their meaning; 12 selects the new CRTC model.

The 4000 models cover 4016/4032 configurations according to RAM size (default 32 KB; smaller
sizes are custom configurations). The original PET 2001 uses BASIC 1 and 8 KB RAM. Only the
12-inch 4000 and the 8032 have the CRTC. Banked 8096/8296 models are not implemented.

> **Upgrading from 2.5?** Inside `roms`, rename `basic2` → `pet2001n`, `basic4` → `pet4000-9`,
> and `8032` → `cbm8032` — or just run `download-roms.ps1` again. The downloader still accepts
> the old set names as aliases.

### `files/` — your programs and disks (the virtual drive root)

Drop `.prg`, `.d64`, and `.d71` files here, then `LOAD"NAME",8` / `LOAD"$",8` from BASIC, or
use **File ▸ Load** (or drag-and-drop onto the window). File ▸ Load also accepts `.t64`
archives and `.tap` cassette images (see [TAP cassette playback](#-tap-cassette-playback)).

Then just launch:

```powershell
build\windows\bin\Release\pet-gpt.exe
```

### 📦 T64 archives

**File ▸ Load** and drag-and-drop accept `.t64` archives. A single-program archive loads
directly; an archive with several programs shows a picker. A BASIC program is run for you
(`[machine] autorun`); for machine code, type the `SYS` address shown in the title bar. Programs keep their original load address and must be PET-compatible. T64 loading
extracts the program straight into RAM — it has no cassette timing and isn't mounted on the
disk drive; use TAP playback for software that reads more from tape.

### 📼 TAP cassette playback

Use **File ▸ Load** or drag a `.tap` onto the window to attach it to cassette port 1.
Type `LOAD` and press Return, then select **File ▸ Tape (cassette 1) ▸ Play**.
The PET ROM reads the tape at emulated tape speed; type `RUN` when loading finishes.
Stop pauses at the current position. Rewind stops and returns to the beginning;
press Play to resume. Eject removes the image. Reset stops playback and preserves
the tape position. Tape motor control pauses playback automatically between reads.

Read-only TAP versions 0 and 1 are supported, including extended v1 pulses and
legacy C64-clock captures of PET tapes. The contents must be PET-compatible.
TAP v2 half-wave images, recording/SAVE, and cassette port 2 are not implemented.
An invalid image leaves the previous tape and running machine intact.
TAP is separate from the IEEE disk drive and the `-disk` option.

### Command line (optional)

```text
pet-gpt [program] [options]
  -pet2001 | -basic2 | -basic4 | -4032 | -8032   select the machine profile
  -disk <file>          mount a .d64 / .d71, or prime a .prg from ./files
  -rom <file>           load a program/disk at startup
  -scale <1|2|3|fit>    initial window scale       -fullscreen | -window
  -monitor <n>          start on monitor n (1 = primary; clears the ini id)
  -monitorid <id>       Windows: physical monitor ID from the log; Linux: display name
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
| Cursor ← / → | `←` / `→` (sent as the PET's shifted/unshifted cursor key) |
| `HOME` / `CLR` | `Home` / `Shift`+`Home` |
| Graphics ⇄ business charset | `F12`* |

\* In graphics mode, `Shift`+letter emits the PETSCII graphic for that key. Also toggleable
in **Machine ▸ Graphics Keyboard**.

Open **Machine ▸ Keyboard Mapping...** for a full PC keyboard diagram.
The window fits the current monitor and is resizable. The full diagram scrolls on
smaller displays, and each key shows its PC name and PET assignment. **Preview Shift**
shows shifted functions.

Click the PC key you want to change to select it and start capture.
Press the key whose default PET function you want—for example, click A and press B
to make physical A type PET B. Only the clicked key changes, and its label updates
immediately. Escape cancels capture. The assignment dropdown also remains available
for selecting PET functions directly.
Use **Apply** or **OK** to save to `pet.ini`; **Cancel** discards changes since the
last Apply. You can clear a key's assignment, restore its default, or
restore all defaults. Physical Shift remains a modifier; reserved host/system keys
stay visible with their host functions and cannot be remapped. Ctrl+O/E/R and Alt+Enter remain emulator shortcuts.

### 🎮 Gamepad (SNES user-port adapter)

Plug in a supported controller (XInput/DirectInput on Windows, evdev on Linux) and Pet-GPT presents it to the PET
as an **SNES adapter on the user port** — the scheme PETSCII Robots and similar games use.
D-Pad and the face/shoulder/start/select buttons are mapped automatically. Enable or disable
it in **Machine ▸ SNES Adapter** (persisted as `[input] snes_adapter`); the data-line invert
lives in `pet.ini` (`[input] snes_invert`).

---

## ⚡ Hotkeys

| Key | Action |
|---|---|
| `Ctrl+O` | Load program / disk… |
| `Ctrl+E` | Eject disk (back to the `./files` virtual drive) |
| `Ctrl+R` | Reset |
| `Pause/Break` / `Ctrl+P` | Pause or resume emulation and audio |
| `F6` | Save a PNG screenshot (also works while paused) |
| `F10` | Toggle CRT look (mono monitor shader) |
| `F9` / `PgUp` / `PgDn` | Select a CRT shader knob / adjust it |
| `F8` | Dump the current CRT shader values to the log |
| `F12` | Toggle graphics ⇄ business keyboard |
| `F11` / `Alt+Enter` | Toggle fullscreen |
| `Esc` | Leave fullscreen. In a window it is a normal PET key (ESC on the 8032 keyboard); it never quits. Use **File ▸ Exit** or the window's close button. |

Screenshots include the current CRT appearance and save automatically under
`screenshots` (`~/Pet-GPT/screenshots` on Linux, the data folder on Windows, beside
the executable in a portable installation). Each capture gets a unique filename; the saved path appears in
the window title. Pausing shows `[Paused]` in the title and keeps the current
picture visible. Plain `P` remains available to PET software.

In fullscreen, the mouse cursor hides after three seconds of inactivity, even
while paused; move the mouse to show it again. On native Wayland, **Fit to Screen**
uses compositor maximization so panels, decorations, and display scaling are
handled by the desktop. The PET picture retains its 4:3 aspect ratio.

---

## 📂 Menus

```text
File ─┬─ Load Program/Disk/Tape…   (Ctrl+O)
      ├─ Eject Disk                (Ctrl+E)
      ├─ Tape (cassette 1) ▸ Play / Stop / Rewind / Eject Tape
      ├─ Save Screenshot           (F6)
      ├─ Reset                     (Ctrl+R)
      └─ Exit
Machine ─┬─ Pause                    (Ctrl+P / Pause)                   (checkbox)
         ├─ Model ▸ PET 2001 / PET 2001N/3000 / PET 4000 9-inch / PET 4000 12-inch CRTC / CBM 8032   (radio)
         ├─ Memory ▸ 4K / 8K / 16K / 32K                              (radio)
         ├─ 2× Speed                                                  (checkbox)
         ├─ Graphics Keyboard    (F12)                                (checkbox)
         ├─ Keyboard Mapping…
         └─ SNES Adapter                                              (checkbox)
View ─┬─ Scale 1× / 2× / 3× / Fit
      ├─ CRT Monitor Settings…   (shader enable, green/B&W, and knobs)
      └─ Fullscreen              (Alt+Enter)
Help ─── About
```

Files can also be **drag-and-dropped** onto the window. After loading a BASIC program Pet-GPT
types `RUN` for you (`[machine] autorun`); loading a disk or tape image shows a hint with the
next command to type in the title bar.

---

## ⚙️ Configuration

Settings are written to `pet.ini` in the selected writable data directory on exit. Portable installations can keep this beside the executable; see [path selection](docs/porting.md#resources-settings-and-user-files).

On Windows, monitor selection uses the same `monitor_select` module as AAE:
`1` is the OS primary display, and the remaining displays are numbered by their
left-to-right position (then top-to-bottom). This avoids driver enumeration
changes when G-Sync is toggled. To pin a physical display, copy its `id=` value
from the startup log's **Monitor list** into `[video] starting_monitor_id` or
pass it with `-monitorid`. A missing ID falls back to the configured number,
then to the primary display if that number is unavailable. Old `\\.\DISPLAYn`
names are not stable IDs; replace them with the logged physical ID.

| Section / key | Values | Notes |
|---|---|---|
| `[machine] basic` | `1`, `2`, `4`, `8`, `12` | Model: original 2001 / 2001N / 4000 9-inch / 8032 / 4000 12-inch |
| `[machine] ram` | `4`, `8`, `16`, `32` | RAM size in KB (default 32; 2001 is fixed at 8; 8032 is fixed at 32) |
| `[machine] speed2x` | `0`/`1` | 2× emulation speed |
| `[machine] autorun` | `0`/`1` | After loading a BASIC `.prg`/`.t64` (load address `$0401`), type `RUN` for you (default `1`). Machine-language programs are never auto-run; the title bar shows the `SYS` address to type. |
| `[input] snes_adapter` | `0`/`1` | emulate the user-port SNES pad |
| `[input] snes_invert` | `0`/`1` | invert the adapter data line |
| `[input] graphics_kbd` | `0`/`1` | Shift+letter types PET graphics chars |
| `[video] scale` | `0`=Fit, `1`/`2`/`3` | window scale preset |
| `[video] fullscreen` | `0`/`1` | start fullscreen |
| `[video] starting_monitor` | int (default `1`) | monitor to open on: `1` = primary, `2..N` = the others left-to-right |
| `[video] starting_monitor_id` | string | Windows: physical monitor device path from the startup log; Linux: wx display name. Takes priority when connected. |
| `[video] crt` | `0`/`1` (default `1`) | mono-monitor CRT shader; off draws plain white on black |
| `[video] crt_tint` | `0`/`1` (default `1`) | green phosphor (`1`) vs black & white (`0`); needs the CRT shader |
| `[video] mono_blur_h` · `mono_blur_v` | float | horizontal / vertical softness |
| `[video] mono_halation` · `mono_halation_radius` | float | glow amount / radius |
| `[video] mono_contrast` · `mono_brightness` | float | beam overdrive / black-level lift |
| `[video] mono_scanline` | float | soft raster-line ripple (0 = off) |
| `[paths] lastromdir` | path | remembered Load… directory |

The CRT knobs are best set live from **View ▸ CRT Monitor Settings** (or F9 / PgUp-PgDn); F8
dumps the current values to the log as a paste-ready block.

---

## 🗂️ Project layout

```text
Pet-GPT/
├─ CMakeLists.txt              # authoritative Windows/Linux build and CTest
├─ CONTRIBUTING.md             # building, testing, reporting bugs
├─ THIRD-PARTY-NOTICES.md      # third-party code and its licences
├─ changelog.txt               # version history
├─ .github/                   # CI workflow, issue and pull request templates
├─ cmake/                     # dependency configuration
├─ tools/                     # build, packaging and smoke-test helpers
├─ docs/                      # porting.md (dependencies, paths, installation, verification), design notes
├─ images/                    # logo, screenshots, demo video
├─ packaging/                 # Linux desktop integration, icons, Flatpak metadata
├─ petemu/
│  ├─ wx/                     # shared wx application, GL canvas, dialogs, key events
│  ├─ emulator.cpp            # machine wiring, ROM loading, frame execution
│  ├─ system/                 # host services, paths, viewport helpers
│  ├─ petsrc/                 # PET devices, video, IEEE/D64/D71, keyboard
│  ├─ cpu_cores/              # existing 6502 interpreter
│  ├─ sys_audio/ sys_input/   # native host audio and controller backends
│  ├─ sys_general/            # logging, ini file, shared helpers
│  ├─ tools/                 # optional ROM downloader
│  ├─ thirdparty/            # GLEW, stb_image, miniz
│  └─ tests/                 # CTest regression sources
└─ build/                     # generated builds; ROMs are user-supplied
```

---

## 👥 Contributors

- **Tim Cottrill** ([@tcottrill](https://github.com/tcottrill)) — author, integrator, maintainer.
- Built with AI pair-programming: **ChatGPT** (original 1.0) and **Claude** (the 2.0 rewrite
  through 2.7 — VIA/PIA, sound, SNES, HLE disk, the PET/CBM models and CRTC, TAP/T64, the
  CRT shader, the keyboard mapper, and the host shell).

## 🙏 Acknowledgements

- **Thomas Skibo** — his JavaScript Commodore PET emulator was the architectural starting point.
- **MAME / MESS** developers — PET timing, IEEE-488 structure, and memory-map validation.
- **Michael Steil** — [cbmbus](https://github.com/mist64/cbmbus_doc) IEEE-488 / Commodore DOS notes.
- **unusedino.de** — the canonical [D64 format reference](http://unusedino.de/ec64/technical/formats/d64.html).
- **zimmers.net** — the canonical Commodore firmware archive the ROM downloader pulls from.
- The PET community for ROM documentation and the sound/joystick demos used as test cases.

---

## 📜 License

Released under the **GNU General Public License v3.0**.

```text
Pet-GPT — Commodore PET/CBM Emulator
Copyright (C) 2026 Tim Cottrill

This program is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software Foundation,
either version 3 of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.
```

Commodore ROM images are **not** distributed with this project and remain the property of
their respective rights holders. Bundled third-party libraries (GLEW, stb_image, miniz) retain
their own licenses; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).


