# Beam Overdrive (Contrast / Brightness) — Design

Date: 2026-07-10
Status: approved

## Goal

Early mono monitors show "fat" text when brightness/contrast is turned up —
strokes widen without getting blurrier. Model this physically in the mono
monitor shader: video gain pushes the Gaussian beam spot's skirt past the
phosphor saturation point, widening strokes while the clamp keeps edges hard.

## Knobs (pet.ini [video], both ini copies, live-tunable via F9 cycle)

| key             | default | range   | step | meaning                            |
|-----------------|---------|---------|------|-------------------------------------|
| mono_contrast   | 1.0     | 1..3    | 0.05 | video gain after the beam spot; the "fat text" control |
| mono_brightness | 0.0     | 0..0.25 | 0.01 | black-level lift (misadjusted-tube background glow)    |

Defaults are neutral: the accepted look is unchanged until the user turns
them up. For very fat text, raise mono_blur_h slightly AND mono_contrast —
bigger spot + saturation = wide crisp strokes (the real-hardware mechanism).

## Shader pipeline (pet_gl.cpp CRT_FS_SRC)

Gaussian spot → `col = min(col * uContrast, 1.0)` → halation screen blend →
optional scanline ripple → `col += uBright` → phosphor tint.

- Clamp before halation keeps the screen blend valid (needs values <= 1).
- Brightness added before tint so lifted black glows phosphor-colored.

## Code changes

- `petsrc/pet_gl.cpp` — two uniforms (`uContrast`, `uBright`) + locations,
  ini reads, two `k_knobs` entries (append after mono_scanline), `knobPtr`
  cases, `draw()` uniform sets.
- `petsrc/pet_gl.h` — members `m_contrast = 1.0f`, `m_bright = 0.0f`,
  uniform location ints.
- `PetGPT/pet.ini` + `x64/Release/pet.ini` — new keys + comments.
- No host/menu changes; F9/PgUp/PgDn/F8 tuning picks the knobs up from the
  table automatically.

## Testing / acceptance

- Build Release x64, run.
- Screenshot neutral (contrast 1.0) vs overdriven (contrast ~2.0): strokes
  visibly wider, edges still crisp (compare zoomed crops).
- mono_brightness > 0 lifts the background with tint applied.
- Defaults produce the exact pre-change look; final approval by eyeball.
