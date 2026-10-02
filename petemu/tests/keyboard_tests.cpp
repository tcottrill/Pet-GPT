#include <windows.h>
#include <cstdio>
#include <cstring>
#include "pet_kbd_input.h"
#include "pet2001io.h"
#include "sys_log.h"
// Include the implementation to inject raw keyboard packets without a window.
#include "../sys_input/rawinput.cpp"
namespace Log { void write(Level,const char*,const char*,int,const char*,...) {} }
// The probe only builds rows; no emulator I/O instance is used.
void Pet2001IO::setKeyrows(const uint8_t[10]) {}
static int failures;
static void check(const char* name, bool ok) {
    printf("%s: %s\n",name,ok ? "PASS" : "FAIL"); failures += !ok;
}
static bool pressed(const uint8_t* rows,int row,int col) { return !(rows[row] & (1<<col)); }
int main() {
    uint8_t rows[10];
    set_pet_business_kbd(false);
    key[VK_LEFT]=1;
    build_pet_rows_from_vk(rows);
    check("Graphics Left = cursor-right + shift",pressed(rows,0,7) && pressed(rows,8,0));
    check("Graphics Left does not type left-arrow glyph",!pressed(rows,0,5));
    memset(key,0,sizeof(key));
    key[VK_HOME]=key[VK_LSHIFT]=1;
    build_pet_rows_from_vk(rows);
    check("Graphics Shift+Home",pressed(rows,0,6) && pressed(rows,8,0));
    set_pet_business_kbd(true);
    build_pet_rows_from_vk(rows);
    check("Business Shift+Home",pressed(rows,8,4) && pressed(rows,6,0));
    test_clr();
    RAWINPUT event = {};
    event.header.dwType=RIM_TYPEKEYBOARD;
    event.data.keyboard.VKey='A';
    RawInput_ProcessInternal(event);
    unsigned char snapshot[256];
    RawInput_GetKeyboardState(snapshot);
    check("Raw keydown is published",snapshot['A']==1 && IsKeyDown('A'));
    event.data.keyboard.Flags=RI_KEY_BREAK;
    RawInput_ProcessInternal(event);
    check("Snapshot remains stable after keyup",snapshot['A']==1 && IsKeyUp('A'));
    event.data.keyboard.Flags=0;
    RawInput_ProcessInternal(event);
    RawInput_ReleaseKey('A');
    check("Consumed key clears held state",IsKeyUp('A') && isKeyHeld('A')==0);
    printf("Keyboard probe failures: %d\n",failures);
    return failures ? 1 : 0;
}
