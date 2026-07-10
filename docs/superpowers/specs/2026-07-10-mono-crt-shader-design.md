# Mono Monitor Shader (VICE-style softness) — Design

Date: 2026-07-10
Status: approved (user: "sure, lets try it and see what we get")

## Goal

Replace the current CRT look (green tint only; old hard-scanline pass disabled)
with a black-and-white / green-screen monitor simulation similar in spirit to
VICE's CRT emulation, but restrained. Must be easy to backport to the AAE
emulator, so the whole effect lives in **one fragment shader** with no FBOs.

## Look components (user-selected)

1. **Horizontal softness** — video-bandwidth blur along the scanline. This is
   the core of the "monitor, not LCD" look.
2. **Halation / glow** — bright areas bleed a faint halo outward.
3. **Soft scanline ripple** — knob exists (Gaussian/cosine beam profile, NOT
   the old hard multiplied dark lines) but **defaults to 0 / off** per user
   preference.
4. Existing phosphor tint stays unchanged, applied last.

Explicitly out of scope: shadow mask / grille, curvature, brightness/contrast/
gamma knobs, vignette.

## Architecture (approach A: single-pass, mip-based halation)

- PET framebuffer (640x400 RGBA) is already uploaded to a GL texture each
  frame. When CRT is ON:
  - texture filter switches to `GL_LINEAR_MIPMAP_LINEAR` / `GL_LINEAR` and
    `glGenerateMipmap` runs after each upload (640x400 — cheap);
  - a new fragment shader (`CRT_FS_SRC`) renders the quad:
    1. Gaussian spot: 7 horizontal x 3 vertical taps in **source-pixel space**
       (blur defined in PET pixels, so it scales with window size). Sigmas from
       `uBlurH` / `uBlurV`; near-zero sigma degenerates to sharp.
    2. Halation: 4 `textureLod()` taps at mip level `log2(radius)`, offset by
       ~radius/2, screen-blended over the image with strength `uHalation`.
    3. Optional beam ripple keyed to the 200 doubled raster lines, strength
       `uScanline` (default 0).
    4. Phosphor tint multiply (existing `uTintOn`/`uTint`).
- When CRT is OFF: existing plain NEAREST passthrough shader, pixel-sharp,
  unchanged.
- The old procedural grille/scanline pass, its shaders, the 1x2 scanline
  texture, and the scanlines.png loader are **removed** (failed earlier
  attempt, dead weight).

## Knobs (pet.ini [video])

| key                  | default | range      | meaning                          |
|----------------------|---------|------------|----------------------------------|
| mono_blur_h          | 0.8     | 0..3       | horizontal spot sigma, PET px    |
| mono_blur_v          | 0.35    | 0..2       | vertical spot sigma, PET px      |
| mono_halation        | 0.15    | 0..1       | glow strength                    |
| mono_halation_radius | 4.0     | 1..16      | glow radius, PET px              |
| mono_scanline        | 0.0     | 0..1       | beam ripple strength (0 = off)   |

`crt`, `crt_tint`, `crt_tint_r/g/b` keep their current meaning. The
`crt_scanline_*` / `crt_grille_*` keys are retired.

## Live tuning (this app only; not part of the AAE backport)

Polled per frame in `emu_run_frame()` via `GetAsyncKeyState` edge detection
(keys chosen to not collide with the PET keyboard map):

- **F9** — cycle selected knob
- **PgUp / PgDn** — nudge selected knob up/down (fine step); **Shift** = coarse
- **F8** — dump all knobs to the log as a paste-ready `[video]` ini block

Each change is logged (`[CRT] mono_blur_h=0.85`) and shown in the window title
via `win_get_window()` + `SetWindowTextA`. Only active while CRT mode is on.

## Files touched

- `petemu/petsrc/pet_gl.cpp` / `.h` — new shader, mipmap upload, knob state,
  tune API (`tuneCycle`, `tuneAdjust`, `tuneStatus`, `tuneDumpIni`); old
  scanline pass removed.
- `petemu/emulator.cpp` — tuning key polling + title update.
- `petemu/petemu.rc`, `pet_host_app.cpp` — "CRT (scanlines)" label/about text
  → "CRT (mono monitor)".
- `PetGPT/pet.ini` and `x64/Release/pet.ini` — new knob defaults + comments,
  grille keys removed (preserve the user's other edits in the Release ini).

## Testing / acceptance

- Build Release x64, run: text screen (READY prompt) and a game.
- F10 toggles cleanly between pixel-sharp and mono-monitor look.
- Knobs adjust live, clamp to ranges, and F8 emits a valid ini block.
- Non-CRT path renders byte-identical behavior to today (NEAREST passthrough).
- Final look approval is by eyeball (user).

## AAE backport notes

Everything needed downstream is: the `CRT_FS_SRC` shader string, LINEAR+mipmap
filtering on the source texture, `glGenerateMipmap` after upload, and the five
uniforms. No FBOs, no extra textures, no vertex-shader changes.
