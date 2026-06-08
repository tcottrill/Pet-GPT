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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow)
{
    HostApp app{};
    app.title       = L"Commodore PET";
    app.base_w      = 640;
    app.base_h      = 400;
    app.rom_filter  = L"PET software\0*.prg;*.d64\0All Files\0*.*\0";
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
    app.about_text  = "Commodore PET Emulator\n\nF11 / Alt+Enter: fullscreen\nF10: CRT (scanlines)\nFile > Load: .prg / .d64";
    // All vector/overlay/audio-slider hooks remain null.
    return host_run(hInstance, nCmdShow, &app);
}
