#pragma once
extern bool emu_init(int argc, char** argv);
extern bool emu_run_frame();   // run + present one frame into the current GL viewport; false = quit (none today: Escape no longer quits)
extern void emu_end();
extern bool pet_load_software(const char* utf8_path); // .prg/.p00-.p99/.t64/.tap/.d64/.d71; true if loaded, mounted or attached
extern void pet_eject_disk();                         // unmount .d64 -> ./files vdrive
extern int  pet_get_disk_mounted();                   // 1 if a .d64 is mounted, else 0
extern void pet_reset();
extern void pet_tape_command(int command); // 0 play, 1 stop, 2 rewind, 3 eject
extern int pet_tape_state(); // bit 0 mounted, bit 1 playing, bit 2 end
extern bool pet_set_basic(int which); // Model IDs: 2, 4, 8, 12
extern int pet_get_basic();
extern int pet_peek(int addr);                        // RAM/ROM/screen byte without side effects (I/O reads $FF), for tests
extern bool pet_rom_set_available(int which);         // model's ROM files are present; changes nothing
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

void emu_present();
int pet_get_ram();
