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
    pet_keyboard_reset_bindings();
    set_pet_business_kbd(false);
    test_clr();
    check("Assign A to RUN/STOP",pet_keyboard_set_binding('A', PET_KEY_RUNSTOP));
    key['A']=1;
    build_pet_rows_from_vk(rows);
    check("Custom key replaces original",pressed(rows,9,4) && !pressed(rows,4,0));
    key[VK_LSHIFT]=1;
    build_pet_rows_from_vk(rows);
    check("Custom RUN/STOP supports BREAK",pressed(rows,9,4) && pressed(rows,8,0));
    set_pet_business_kbd(true);
    build_pet_rows_from_vk(rows);
    check("Custom action follows business matrix",pressed(rows,9,4) && pressed(rows,6,0) && !pressed(rows,3,0));
    pet_keyboard_set_binding('A', PET_KEY_NONE);
    build_pet_rows_from_vk(rows);
    bool idle=true;
    for (auto r : rows) idle &= r==255;
    check("Clear assignment suppresses original",idle);
    check("Fullscreen key reserved",!pet_keyboard_set_binding(VK_F11,'A'));
    check("Modifier key reserved",!pet_keyboard_set_binding(VK_LCONTROL,'A'));
    check("Invalid assignment rejected",!pet_keyboard_set_binding('A',99999));
    pet_keyboard_reset_bindings();
    test_clr();
    set_pet_business_kbd(false);
    pet_keyboard_set_binding('A','B');
    key['A']=key['C']=1;
    build_pet_rows_from_vk(rows);
    check("Custom and default keys combine",pressed(rows,6,2) && pressed(rows,6,1) && !pressed(rows,4,0));
    pet_keyboard_reset_bindings();
    build_pet_rows_from_vk(rows);
    check("Defaults restore original key",pressed(rows,4,0) && !pressed(rows,6,2));
    check("Preview labels actual RUN/STOP",pet_keyboard_label(VK_CAPITAL,false)==L"RUN/STOP");
    check("Preview labels synthetic cursor",pet_keyboard_label(VK_LEFT,false)==L"LEFT");
    check("Preview labels shifted HOME",pet_keyboard_label(VK_HOME,true)==L"CLEAR");
    pet_keyboard_set_binding('A','B');
    check("Preview reflects custom assignment",pet_keyboard_label('A',false)==L"B");
    check("Preview reflects graphics shift",pet_keyboard_label('A',true)==L"Gfx B");
    set_pet_graphics_mode(false);
    check("Preview reflects typing mode",pet_keyboard_label('A',true)==L"B");
    auto draft=pet_keyboard_bindings();
    draft['A']=PET_KEY_RUNSTOP;
    check("Draft preview does not change active mapping",
          pet_keyboard_label('A',false,&draft)==L"RUN/STOP" && pet_keyboard_label('A',false)==L"B");
    pet_keyboard_set_binding(VK_CAPITAL,PET_KEY_NONE);
    auto saved=pet_keyboard_serialize();
    pet_keyboard_reset_bindings();
    pet_keyboard_deserialize(saved);
    check("Saved assignments round trip",pet_keyboard_label('A',false)==L"B" && pet_keyboard_label(VK_CAPITAL,false)==L"Unassigned");
    pet_keyboard_deserialize("65:128,999:65,66:99999,122:65,67:0garbage,68:0");
    check("Valid saved assignment restored",pet_keyboard_label('A',false)==L"RUN/STOP");
    check("Corrupt entries fall back to defaults",pet_keyboard_label('B',false)==L"B" && pet_keyboard_label('C',false)==L"C");
    check("Saved reserved-key override rejected",pet_keyboard_bindings()[VK_F11]==PET_KEY_DEFAULT);
    check("Saved clear assignment restored",pet_keyboard_label('D',false)==L"Unassigned");
    set_pet_business_kbd(true);
    pet_keyboard_set_binding('A','B');
    check("Business custom letter lower case",pet_keyboard_label('A',false)==L"b");
    check("Business custom letter shifted upper case",pet_keyboard_label('A',true)==L"B");
    pet_keyboard_set_binding('A','_');
    check("Business underscore preview",pet_keyboard_label('A',false)==L"_");
    pet_keyboard_set_binding(VK_SEPARATOR,PET_KEY_UP);
    test_clr(); key[VK_SEPARATOR]=1;
    build_pet_rows_from_vk(rows);
    check("Numpad Enter can be assigned",pressed(rows,5,4) && pressed(rows,6,0) && !pressed(rows,3,4));
    pet_keyboard_reset_bindings();
    printf("Keyboard probe failures: %d\n",failures);
    return failures ? 1 : 0;
}
