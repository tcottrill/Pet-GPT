#include "rawinput.h"
#include <cstring>
unsigned char key[256]{};
void RawInput_SetKey(int vk,bool down) { if(vk>0 && vk<256) key[vk]=down?1:0; }
void RawInput_GetKeyboardState(unsigned char out[256]) { std::memcpy(out,key,256); }
void RawInput_ReleaseKey(int vk) { RawInput_SetKey(vk,false); }
void RawInput_ReleaseAll() { std::memset(key,0,sizeof(key)); }
void RawInput_Shutdown() { RawInput_ReleaseAll(); }
bool IsKeyDown(int vk) { return vk>=0 && vk<256 && key[vk]!=0; }
bool IsKeyUp(int vk) { return !IsKeyDown(vk); }
int isKeyHeld(int vk) { return IsKeyDown(vk)?1:0; }
void test_clr() { RawInput_ReleaseAll(); }
int GetModifierFlags() {
 return ((key[VK_SHIFT]||key[VK_LSHIFT]||key[VK_RSHIFT])?RI_MOD_SHIFT:0)
 | ((key[VK_CONTROL]||key[VK_LCONTROL]||key[VK_RCONTROL])?RI_MOD_CONTROL:0)
 | ((key[VK_MENU]||key[VK_LMENU]||key[VK_RMENU])?RI_MOD_ALT:0)
 | ((key[VK_LWIN]||key[VK_RWIN])?RI_MOD_SUPER:0);
}
