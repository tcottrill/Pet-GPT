// Vectrex-Emu
// Copyright (C) 2026 Tim Cottrill and Claude Code
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once
#include <windows.h>

// The entire contract between the reusable host shell and a specific emulator.
// Nothing in the host references any specific machine; an emulator fills this in.
struct HostApp {
    const wchar_t* title;        // window title, e.g. L"Vectrex"
    int   base_w;                // 1x client width
    int   base_h;                // 1x client height; aspect = base_w / base_h
    const wchar_t* rom_filter;   // OPENFILENAME filter, double-null terminated:
                                 // L"Vectrex ROMs\0*.vec;*.bin;*.gam\0All Files\0*.*\0"
    double target_fps;           // 0 = emulator self-paces; > 0 = host frame-limits

    bool (*init)(int argc, char** argv);     // one-time init; return false to abort
    void (*load_rom)(const char* utf8_path); // load cart/rom, then reset
    void (*eject_disk)(void);                 // unmount mounted disk image (may be null)
    int  (*get_disk_mounted)(void);           // 1 if a disk image is mounted, else 0 (may be null)
    void (*reset)(void);                      // reset the machine
    bool (*run_frame)(void);                  // run ONE frame, draw into current
                                              // GL viewport; return false to quit
    void (*shutdown)(void);                   // one-time teardown
    const char* about_text;                   // shown in the About box (may be null)

    // --- Optional overlay + video-effect hooks (any may be null = unsupported) ---
    const wchar_t* overlay_filter;            // OPENFILENAME filter for overlay images
    void (*load_overlay)(const char* utf8_path);  // load a translucent overlay image
    void (*clear_overlay)(void);                  // remove the current overlay
    int  (*has_overlay)(void);                    // 1 if an overlay is loaded, else 0
    int  (*toggle_video)(int which);          // toggle effect; returns new state 0/1, -1 = n/a
    int  (*get_video)(int which);             // current state of effect: 0/1, -1 = n/a
    void (*set_video)(int which, int on);     // set effect on/off directly (for restoring settings)

    // Slider-controlled video params (any may be null = unsupported).
    int   (*get_glow)(void);                  // glow amount, 0..15
    void  (*set_glow)(int amt);
    float (*get_line_width)(void);            // line stroke width, 1.0..10.0
    void  (*set_line_width)(float w);
    float (*get_point_size)(void);            // line endpoint size, 1.0..10.0
    void  (*set_point_size)(float w);
    float (*get_dot_size)(void);              // single-dot size, 1.0..10.0
    void  (*set_dot_size)(float w);

    // Called once (before init) with the host's chosen present rate -- the
    // monitor refresh, unless target_fps is non-zero. The emulator scales its
    // per-frame work to this so game speed stays correct. May be null.
    void  (*set_frame_rate)(double hz);

    // Select the system ROM set (2 = BASIC 2, 4 = BASIC 4): reload ROMs + reset.
    void (*set_basic)(int which);   // may be null

    // Select RAM size in KB (4/8/16/32): resize + reset. May be null.
    void (*set_ram)(int kb);

    // CRT look (scanlines + phosphor tint): on/off. Any may be null.
    void (*set_crt)(int on);
    int  (*get_crt)(void);

    // Monitor color for the CRT shader: 1 = green phosphor tint, 0 = black &
    // white (no tint). Any may be null.
    void (*set_monitor)(int green);
    int  (*get_monitor)(void);

    // Emulation speed: 1 = authentic, 2 = double. Any may be null.
    void (*set_speed)(int mult);
    int  (*get_speed)(void);

    // CRT shader knobs (View > CRT Monitor Settings dialog). idx = knob index
    // 0..6 (see host_resource.h). set clamps and persists. Any may be null.
    float (*shader_get)(int idx);
    void  (*shader_set)(int idx, float v);
    void  (*shader_range)(int idx, float* lo, float* hi, float* step);
    void  (*shader_defaults)(void);

    // Graphics keyboard mode: 1 = Shift+letter types PET graphics chars,
    // 0 = business typing. Can also be toggled by a hotkey inside the
    // emulator, so the host re-reads get_gfx_kbd when menus open. May be null.
    void (*set_gfx_kbd)(int on);
    int  (*get_gfx_kbd)(void);

    // SNES user-port adapter (gamepad input): 1 = enabled, 0 = disabled.
    // Machine-menu checkbox; persisted to [input] snes_adapter. May be null.
    void (*set_snes)(int on);
    int  (*get_snes)(void);

    // Master audio volume, 0..100 (emulator-wide, not per-game). May be null.
    int   (*get_volume)(void);
    void  (*set_volume)(int percent);

    // Optional ambient sound (e.g. the flyback buzz), emulator-wide. Any may be null.
    int   (*has_ambient)(void);            // 1 if an ambient sample is available
    int   (*get_ambient_enabled)(void);
    void  (*set_ambient_enabled)(int on);
    int   (*get_ambient_volume)(void);     // 0..100
    void  (*set_ambient_volume)(int pct);
};

// Values for HostApp::toggle_video(which).
enum { HOST_VID_GLOW = 0, HOST_VID_TRAIL = 1, HOST_VID_OVERLAY = 2 };
