# Monitor Color Choice (Green / Black & White) — Design

Date: 2026-07-10
Status: approved

## Goal

Many PETs shipped with black-and-white monitors. Add a View > **Monitor**
submenu with two radio items — **Green phosphor** (default) and **Black &
white** — that selects the tint used by the mono monitor shader. The choice
persists in pet.ini.

## Behavior

- Applies to shader (CRT) mode only: B&W = full softness/halation with no
  tint multiply; Green = existing `crt_tint_r/g/b` tint.
- CRT off remains the untouched pixel-sharp passthrough (never tinted).
- Selecting a color while CRT is off is allowed; it takes effect when CRT is
  turned on. F10 keeps its current meaning (CRT on/off).

## Persistence

Reuses the existing `crt_tint` key (1 = green, 0 = B&W): written via
`set_config_bool` immediately on menu change and again at exit, mirroring the
`crt` key. Menu radio state and the shader both initialize from it at startup.
No new ini keys.

## Plumbing (mirrors the existing IDM_CRT pattern)

- `system/host_resource.h` — `IDM_MONITOR_GREEN` 40041, `IDM_MONITOR_BW` 40042.
- `petemu.rc` — `POPUP "Monitor"` in the View menu (file is UTF-16; edit via
  PowerShell, not the Edit tool).
- `system/host_app.h` — optional callbacks `set_monitor(int green)` /
  `get_monitor(void)`.
- `system/host_window.cpp` — `g_monitorGreen` state, radio-check updater
  (`CheckMenuRadioItem`), `WM_COMMAND` cases, startup init from ini, exit save.
- `pet_host_app.cpp` — wire the two callbacks.
- `emulator.h` / `emulator.cpp` — `pet_set_monitor(int)` / `pet_get_monitor()`
  forwarding to PetGL.
- `petsrc/pet_gl.h/.cpp` — `setTintEnabled(bool)` / `getTintEnabled()` over
  the existing `m_tintOn`. The shader itself needs zero changes (`uTintOn`
  uniform already exists).

## Testing / acceptance

- Build Release x64; flip Green <-> B&W with CRT on: visible change, softness
  and halation retained in both (verify by screenshot).
- Choice survives restart (crt_tint in pet.ini).
- CRT off unaffected by the monitor choice.
