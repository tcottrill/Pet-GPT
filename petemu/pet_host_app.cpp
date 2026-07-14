#include <windows.h>
#include "system/host_window.h"
#include "system/host_app.h"
#include "emulator.h"

static bool   PetInit(int argc, char** argv) { emu_init(argc, argv); return true; }
static bool   PetRunFrame()                  { return emu_run_frame(); }
static void   PetLoadRom(const char* p)      { pet_load_software(p); }
static void   PetEjectDisk()                 { pet_eject_disk(); }
static int    PetGetDiskMounted()            { return pet_get_disk_mounted(); }
static void   PetReset()                     { pet_reset(); }
static void   PetShutdown()                  { emu_end(); }
static void   PetSetBasic(int which)         { pet_set_basic(which); }
static void   PetSetRam(int kb)              { pet_set_ram(kb); }
static void   PetSetCrt(int on)              { pet_set_crt(on); }
static int    PetGetCrt(void)                { return pet_get_crt(); }
static void   PetSetMonitor(int green)       { pet_set_monitor(green); }
static int    PetGetMonitor(void)            { return pet_get_monitor(); }
static void   PetSetSpeed(int mult)          { pet_set_speed(mult); }
static int    PetGetSpeed(void)              { return pet_get_speed(); }
static void   PetSetGfxKbd(int on)           { pet_set_gfx_kbd(on); }
static float  PetShaderGet(int i)            { return pet_shader_get(i); }
static void   PetShaderSet(int i, float v)   { pet_shader_set(i, v); }
static void   PetShaderRange(int i, float* a, float* b, float* c) { pet_shader_range(i, a, b, c); }
static void   PetShaderDefaults(void)        { pet_shader_defaults(); }
static int    PetGetGfxKbd(void)             { return pet_get_gfx_kbd(); }
static void   PetSetSnes(int on)             { pet_set_snes(on); }
static int    PetGetSnes(void)               { return pet_get_snes(); }

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    HostApp app{};
    app.title       = L"Commodore PET";
    // 4:3 like the PET's CRT: the 320x200 raster fills the tube with ~1.2x
    // tall pixels, so the 1x client is 640x480, not the framebuffer's 640x400
    // (16:10, which read as widescreen-stretched in fullscreen).
    app.base_w      = 640;
    app.base_h      = 480;
    app.rom_filter  = L"PET software\0*.prg;*.d64;*.d71\0All Files\0*.*\0";
    app.target_fps  = 60.0;            // authentic PET frame rate
    app.init        = PetInit;
    app.run_frame   = PetRunFrame;
    app.load_rom    = PetLoadRom;
    app.eject_disk  = PetEjectDisk;
    app.get_disk_mounted = PetGetDiskMounted;
    app.reset       = PetReset;
    app.shutdown    = PetShutdown;
    app.set_basic   = PetSetBasic;
    app.set_ram     = PetSetRam;
    app.set_crt     = PetSetCrt;
    app.get_crt     = PetGetCrt;
    app.set_monitor = PetSetMonitor;
    app.get_monitor = PetGetMonitor;
    app.set_speed   = PetSetSpeed;
    app.get_speed   = PetGetSpeed;
    app.set_gfx_kbd = PetSetGfxKbd;
    app.shader_get      = PetShaderGet;
    app.shader_set      = PetShaderSet;
    app.shader_range    = PetShaderRange;
    app.shader_defaults = PetShaderDefaults;
    app.get_gfx_kbd = PetGetGfxKbd;
    app.set_snes    = PetSetSnes;
    app.get_snes    = PetGetSnes;
    app.about_text  = "Commodore PET Emulator\n\nF11 / Alt+Enter: fullscreen\n"
                      "F12: graphics / business typing mode\n"
                      "View > CRT Monitor: shader, monitor color, and tuning\n"
                      "File > Load: .prg / .d64 / .d71";
    // All vector/overlay/audio-slider hooks remain null.
    return host_run(hInstance, nCmdShow, &app);
}
