#pragma once
extern void emu_init(int argc, char** argv);
extern bool emu_run_frame();   // run + present one frame into the current GL viewport; false = quit
extern void emu_end();
extern void pet_load_software(const char* utf8_path); // .prg/.d64
extern void pet_eject_disk();                         // unmount .d64 -> ./files vdrive
extern int  pet_get_disk_mounted();                   // 1 if a .d64 is mounted, else 0
extern void pet_reset();
extern void pet_set_basic(int which);                 // 2 or 4: reload ROM set + reset
extern void pet_set_ram(int kb);                      // 4/8/16/32: resize RAM + reset
extern void pet_set_crt(int on);                      // CRT look (mono monitor shader) on/off
extern int  pet_get_crt();                            // 1 if CRT look enabled, else 0
extern void pet_set_monitor(int green);               // shader tint: 1 = green phosphor, 0 = B&W
extern int  pet_get_monitor();                        // 1 if green tint active, else 0
extern void pet_set_speed(int mult);                  // emulation speed: 1 = authentic, 2 = double
extern int  pet_get_speed();                          // current speed multiplier
extern void pet_set_gfx_kbd(int on);                  // 1 = Shift+letter types graphics chars
extern int  pet_get_gfx_kbd();                        // current graphics-keyboard mode
extern void pet_set_snes(int on);                     // 1 = SNES user-port adapter enabled
extern int  pet_get_snes();                           // current SNES adapter enable state
extern float pet_shader_get(int idx);                 // CRT shader knob value
extern void  pet_shader_set(int idx, float v);        // set (clamps + saves ini)
extern void  pet_shader_range(int idx, float* lo, float* hi, float* step);
extern void  pet_shader_defaults();                   // restore all shader knobs
