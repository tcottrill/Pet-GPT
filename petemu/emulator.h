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
extern void pet_set_crt(int on);                      // CRT look (scanlines+tint) on/off
extern int  pet_get_crt();                            // 1 if CRT look enabled, else 0
