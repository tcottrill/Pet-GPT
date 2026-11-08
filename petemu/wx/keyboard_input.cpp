#include "keyboard_input.h"

#include <wx/window.h>
#include <wx/event.h>
#include <map>
#include <utility>
#include "../sys_input/rawinput.h"
#include "../petsrc/pet_kbd_input.h"
int pet_key_from_wx(int c) {
 if(c>='a' && c<='z') return c-32;
 if((c>='A' && c<='Z') || (c>='0' && c<='9')) return c;
 if(c>=WXK_F1 && c<=WXK_F12) return VK_F1+c-WXK_F1;
 if(c>=WXK_NUMPAD0 && c<=WXK_NUMPAD9) return VK_NUMPAD0+c-WXK_NUMPAD0;
 const char* shifted=")!@#$%^&*(";
 for(int i=0;i<10;++i)if(c==shifted[i])return '0'+i;
 switch(c) {
 case WXK_BACK: return VK_BACK; case WXK_TAB: return VK_TAB;
 case WXK_RETURN: return VK_RETURN; case WXK_NUMPAD_ENTER: return VK_SEPARATOR;
 case WXK_ESCAPE: return VK_ESCAPE; case WXK_SPACE: return VK_SPACE;
 case WXK_SHIFT: return VK_SHIFT; case WXK_CONTROL: return VK_CONTROL; case WXK_ALT: return VK_MENU;
 case WXK_CAPITAL: return VK_CAPITAL; case WXK_NUMLOCK: return VK_NUMLOCK; case WXK_SCROLL: return VK_SCROLL;
 case WXK_HOME:return VK_HOME; case WXK_END:return VK_END; case WXK_INSERT:return VK_INSERT; case WXK_DELETE:return VK_DELETE;
 case WXK_LEFT:return VK_LEFT; case WXK_RIGHT:return VK_RIGHT; case WXK_UP:return VK_UP; case WXK_DOWN:return VK_DOWN;
 case WXK_PAGEUP:return VK_PRIOR; case WXK_PAGEDOWN:return VK_NEXT;
 case WXK_NUMPAD_INSERT:return VK_NUMPAD0; case WXK_NUMPAD_END:return VK_NUMPAD1;
 case WXK_NUMPAD_DOWN:return VK_NUMPAD2; case WXK_NUMPAD_PAGEDOWN:return VK_NUMPAD3;
 case WXK_NUMPAD_LEFT:return VK_NUMPAD4; case WXK_NUMPAD_BEGIN:return VK_NUMPAD5;
 case WXK_NUMPAD_RIGHT:return VK_NUMPAD6; case WXK_NUMPAD_HOME:return VK_NUMPAD7;
 case WXK_NUMPAD_UP:return VK_NUMPAD8; case WXK_NUMPAD_PAGEUP:return VK_NUMPAD9;
 case WXK_NUMPAD_DELETE: case WXK_NUMPAD_DECIMAL:return VK_DECIMAL;
 case WXK_NUMPAD_ADD:return VK_ADD; case WXK_NUMPAD_SUBTRACT:return VK_SUBTRACT;
 case WXK_NUMPAD_MULTIPLY:return VK_MULTIPLY; case WXK_NUMPAD_DIVIDE:return VK_DIVIDE;
 case ';':case ':':return VK_OEM_1; case '=':case '+':return VK_OEM_PLUS;
 case ',':case '<':return VK_OEM_COMMA; case '-':case '_':return VK_OEM_MINUS;
 case '.':case '>':return VK_OEM_PERIOD; case '/':case '?':return VK_OEM_2;
 case '`':case '~':return VK_OEM_3; case '[':case '{':return VK_OEM_4;
 case '\\':case '|':return VK_OEM_5; case ']':case '}':return VK_OEM_6;
 case '\'':case '"':return VK_OEM_7;
 default:return 0;
 }
}

namespace {
// AltGr on international layouts (Windows reports it as Ctrl+Alt) composes
// characters such as '@' on a German keyboard. The key event only carries the
// unmodified key, so the key-down is skipped to let wx produce the composed
// character, which becomes the PET key that types it. The physical key code is
// remembered so its key-up releases what was pressed.
int altgr_pending=0;
std::map<int,std::pair<int,bool>> altgr_held;   // wx key code -> (vk, forced shift)

bool is_non_shift_modifier(int code) {
 switch(code) {
 case WXK_CONTROL: case WXK_ALT: case WXK_WINDOWS_LEFT: case WXK_WINDOWS_RIGHT:
 case WXK_WINDOWS_MENU: return true;
 default: return false;
 }
}
}

void pet_keyboard_handle_wx(wxKeyEvent& e,bool down) {
 const int code=e.GetKeyCode();
 const int vk=pet_key_from_wx(code);
 if(!down) {
  const auto held=altgr_held.find(code);
  if(held!=altgr_held.end()) {
   RawInput_ReleaseKey(held->second.first);
   if(held->second.second) RawInput_SetKey(VK_SHIFT,e.ShiftDown());
   altgr_held.erase(held);
   return;
  }
  RawInput_ReleaseKey(vk);
  return;
 }
 altgr_held.erase(code);   // a stale AltGr entry must not capture this key's release
 // Ctrl/Alt/Win on their own mean nothing to the PET; they must not drop keys
 // that are already held (a game key held while Ctrl is pressed).
 if(is_non_shift_modifier(code)) return;
 if(e.ControlDown() && e.AltDown() && !e.MetaDown()) { altgr_pending=code; e.Skip(); return; }
 // Menu accelerators and host shortcuts must never leak a PET letter, RETURN,
 // or custom binding.
 if(e.ControlDown() || e.AltDown() || e.MetaDown()) return;
 altgr_pending=0;
 RawInput_SetKey(VK_SHIFT,e.ShiftDown());
 RawInput_SetKey(vk,true);
}

void pet_keyboard_handle_wx_char(wxKeyEvent& e) {
 const int code=altgr_pending;
 altgr_pending=0;
 if(!code) { e.Skip(); return; }
 const wxChar ch=e.GetUnicodeKey();
 int vk=0; bool shift=false;
 if(ch<32 || ch>126 || !pet_keyboard_vk_for_char(static_cast<unsigned char>(ch),&vk,&shift)) return;
 altgr_held[code]={vk,shift};
 RawInput_SetKey(VK_SHIFT,shift);
 RawInput_SetKey(vk,true);
}

void pet_keyboard_wx_release_all() {
 altgr_pending=0;
 altgr_held.clear();
 RawInput_ReleaseAll();
}

void wx_bind_pet_keyboard(wxWindow* window) {
 window->Bind(wxEVT_KEY_DOWN,[](wxKeyEvent& e){pet_keyboard_handle_wx(e,true);});
 window->Bind(wxEVT_CHAR,[](wxKeyEvent& e){pet_keyboard_handle_wx_char(e);});
 window->Bind(wxEVT_KEY_UP,[](wxKeyEvent& e){pet_keyboard_handle_wx(e,false);});
 window->Bind(wxEVT_KILL_FOCUS,[](wxFocusEvent& e){pet_keyboard_wx_release_all();e.Skip();});
}
