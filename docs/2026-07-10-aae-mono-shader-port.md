# Porting the PET mono monitor shader to AAE — B/W RASTER path only

Scope: AAE's raster (bitmap) game rendering. Vector games are explicitly OUT —
they need different treatment (line-based glow) and get their own plan later.

## 1. What to take from the PET repo (the whole effect, no FBOs of its own)

Source of truth: `Pet-GPT-2026/petemu/petsrc/pet_gl.cpp`

- **`CRT_FS_SRC`** fragment shader string (GLSL 330 core). Pipeline inside:
  7x3 Gaussian beam spot -> beam overdrive (`min(col*uContrast,1)`) ->
  mip-pyramid halation (4x `textureLod`, screen blend) -> optional scanline
  ripple -> black-level lift (`+uBright`) -> tint multiply.
- The plain vertex shader (`VS_SRC`) or AAE's existing textured-quad vertex
  shader — the frag only needs `vUV`.
- Uniforms (all of them): `uTex, uSrcSize, uBlurH, uBlurV, uHalation,
  uHalRadius, uScanline, uContrast, uBright, uTintOn, uTint`.
- Texture state requirements: source texture sampled LINEAR (mag) +
  LINEAR_MIPMAP_LINEAR (min), and `glGenerateMipmap(GL_TEXTURE_2D)` after the
  game image is produced each frame (that's the no-FBO halation trick).
- Accepted PET defaults as starting values: blur_h 0.8, blur_v 0.35,
  halation 0.15, halation_radius 4.0, scanline 0.0, contrast 1.0, bright 0.0.

## 2. Where it lands in AAE (`aae/aae_video/`)

AAE's compositing (see header comment in `opengl_renderer.cpp`): game renders
into **FBO1 / img1a (1024x1024)** -> `final_render()` draws that texture to
the window. Raster games: `raster_poly_update()` + optional
**scanlineMultiply** overlay pass, selected per game by a **`raster_effect`
name check** (strcmp in `opengl_renderer.cpp`).

- `shader_definitions.h`: add `monoMonitorVert/monoMonitorFrag` (paste
  `CRT_FS_SRC`, keep uniform names identical to the PET's).
- `gl_shader.cpp`: compile/link it like the existing programs; cache uniform
  locations once.
- `opengl_renderer.cpp` `final_render()`: when the active game is raster AND
  its `raster_effect == "mono"` (new name; drivers opt in per game), draw the
  FBO texture with the mono program instead of the basic textured quad.
  B/W-only gate falls out naturally: only give `"mono"` to B/W drivers.
- `gl_fbo.cpp`: the FBO color texture needs a mip chain — call
  `glGenerateMipmap` on img1a right before `final_render` binds it (first
  call allocates the chain), and set its MIN filter to
  `GL_LINEAR_MIPMAP_LINEAR` while the mono effect is active. Restore plain
  LINEAR/NEAREST for other effects.

## 3. The one real difference vs the PET: source-pixel units in a 1024 atlas

On the PET, `uSrcSize` = the 640x400 framebuffer and the game fills it. In
AAE the game occupies a SUB-RECT of the 1024x1024 FBO, so "1 source pixel"
must mean **1 native game pixel**, not 1 FBO texel:

- pass `uSrcSize = vec2(1024,1024) / (gameNativeSize_in_FBO_texels /
  gameNativeSize_in_pixels)` — i.e. FBO size divided by the scale factor the
  raster path used (`raster scale factor` global in opengl_renderer.cpp).
  Net effect: `1.0/uSrcSize` in the shader = one game pixel in UV space.
- halation `textureLod` levels are relative to the 1024 texture; with the
  radius expressed in game pixels the existing `log2(radius)` math still
  works because the offsets use the same px unit. Verify visually with a
  known game; if glow looks too tight/wide, scale `uHalRadius` by the same
  factor.
- The scanline ripple term keys off `vUV.y * uSrcSize.y` and assumes the PET's
  line-doubled framebuffer; for AAE raster games (native-resolution rows) use
  pitch = 1 row instead of 2 if the term is ever enabled. Default it to 0 and
  prefer AAE's existing scanlineMultiply overlay for the "mappy look".

## 4. Knobs: AAE's menu system (user decision 2026-07-10, not hotkeys)

- Global defaults in aae.ini `[monomonitor]` (same key names as the PET's
  `mono_*` for sanity), per-game override via the driver's `raster_effect`
  parameters if AAE supports per-effect args — else per-game ini sections.
- Menu: one "Mono Monitor" submenu with the 7 knobs + a monitor-color preset
  list. Presets (tint RGB): **P4 white** (1.0, 1.0, 1.0), **green P1**
  (0.30, 1.0, 0.40 — the PET's), **amber P3** (1.0, 0.75, 0.20). The PET's
  View > Monitor pattern (host_window.cpp) is the reference implementation
  for check/radio persistence.
- The PET's F9/PgUp/PgDn/F8 hotkey tuning scaffold does NOT port — menu only.

## 5. Order of work + verification

1. Shader compiles + renders identity-ish (all knobs neutral: blur 0.02,
   halation 0, contrast 1) on one B/W raster game — proves plumbing without
   changing the look.
2. Turn on PET defaults; screenshot compare at 2x and fullscreen.
3. Mipmap/halation check: bright sprite on black shows soft halo, no blocky
   artifacts (if blocky, the mip chain isn't complete — check the
   glGenerateMipmap call site and MIN filter).
4. Menu knobs live-adjust; presets switch tint correctly; settings persist.
5. Regression: color raster games and vector games UNCHANGED (effect gated on
   `raster_effect=="mono"`); scanlineMultiply games unchanged.

## 6. Known pitfalls

- GL state leakage: AAE's renderer swaps programs/textures aggressively —
  set all mono uniforms every frame (cheap) rather than caching state.
- `glGenerateMipmap` on an FBO-attached texture is legal but do it AFTER the
  FBO pass ends (unbind first) or some drivers stall.
- Don't reuse the PET's per-frame filter toggling wholesale — AAE keeps other
  effects on the same texture; scope filter changes to the mono branch.
- 1024x1024 atlas + LINEAR sampling can bleed neighboring FBO content into
  wide halation taps at the game's edge; if visible, clamp the halation
  sample UVs to the game sub-rect.
