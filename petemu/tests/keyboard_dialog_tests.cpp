// Native wx controls and key events, exercised on Windows and GTK (Xvfb).
#include <wx/wx.h>
#include <cstdio>
#include "pet_kbd_input.h"
#include "pet2001io.h"
#include "sys_log.h"
#include "../sys_input/rawinput.cpp"
namespace Log { void write(Level,const char*,const char*,int,const char*,...) {} }
void Pet2001IO::setKeyrows(const uint8_t[10]) {}
#include "../petsrc/pet_keyboard_dialog.cpp"
class TestApp:public wxApp {public:bool OnInit() override{return true;}};
wxIMPLEMENT_APP_NO_MAIN(TestApp);
static int failures;
static void check(const char* name,bool ok){printf("%s: %s\n",name,ok?"PASS":"FAIL");failures+=!ok;}
static wxButton* cap(Editor& e,int vk){return wxDynamicCast(e.FindWindow(keyBase+vk),wxButton);}
static void click(Editor& e,int vk){wxCommandEvent event(wxEVT_BUTTON,keyBase+vk);cap(e,vk)->GetEventHandler()->ProcessEvent(event);}
static void capture(Editor& e,int code){wxKeyEvent event(wxEVT_CHAR_HOOK);event.m_keyCode=code;e.GetEventHandler()->ProcessEvent(event);}
int main(int argc,char** argv){
 if(!wxEntryStart(argc,argv))return 1;wxTheApp->CallOnInit();
 {
 Editor e(nullptr);
 bool all=true;for(int vk=VK_F1;vk<=VK_F12;++vk)all &= cap(e,vk)!=nullptr;
 check("All twelve function keys",all);
 check("System and lock keys",cap(e,VK_SNAPSHOT)&&cap(e,VK_SCROLL)&&cap(e,VK_NUMLOCK)&&cap(e,VK_CAPITAL));
 check("Full numpad",cap(e,VK_NUMPAD0)&&cap(e,VK_SEPARATOR)&&cap(e,VK_ADD));
 click(e,'A');check("Click starts physical capture",e.capturing);
 capture(e,'B');check("Capture only changes selected key",e.draft['A']=='B'&&e.draft['B']==PET_KEY_DEFAULT);
 check("Draft does not publish before Apply",pet_keyboard_bindings()['A']==PET_KEY_DEFAULT);
 check("Selected caption updates",cap(e,'A')->GetLabel().Contains("A: B"));
 auto saved=e.draft;click(e,'A');capture(e,WXK_ESCAPE);check("Escape cancels capture",!e.capturing&&e.draft==saved);
 click(e,'A');capture(e,WXK_F12);check("Reserved capture rejected",e.capturing&&e.draft==saved);
 capture(e,WXK_RETURN);check("Return captured without closing",e.draft['A']==PET_KEY_RETURN);
 check("Other key caption unchanged",cap(e,'B')->GetLabel().Contains("B: B"));
 click(e,'B');capture(e,WXK_TAB);check("Tab consumed during capture",e.capturing);
 capture(e,WXK_NUMPAD_ENTER);check("Numpad Enter maps return",e.draft['B']==PET_KEY_RETURN);
 e.apply();check("Apply publishes mappings",pet_keyboard_bindings()['A']==PET_KEY_RETURN&&pet_keyboard_bindings()['B']==PET_KEY_RETURN);
 click(e,'A');capture(e,WXK_NUMPAD_LEFT);check("NumLock off keypad maps digit",e.draft['A']=='4');
 click(e,'A');capture(e,WXK_LEFT);check("Dedicated cursor maps cursor",e.draft['A']==PET_KEY_LEFT);
 wxCommandEvent restoreEvent(wxEVT_BUTTON,e.restore->GetId());e.restore->GetEventHandler()->ProcessEvent(restoreEvent);
 check("Restore only selected key",e.draft['A']==PET_KEY_DEFAULT&&e.draft['B']==PET_KEY_RETURN);
 wxCommandEvent clearEvent(wxEVT_BUTTON,e.clear->GetId());e.clear->GetEventHandler()->ProcessEvent(clearEvent);
 check("Clear only selected key",e.draft['A']==PET_KEY_NONE&&e.draft[VK_RETURN]==PET_KEY_DEFAULT);
 click(e,VK_F1);capture(e,'A');check("F1 can be assigned",e.draft[VK_F1]=='A');
 click(e,VK_F12);check("Reserved key remains visible",!e.capturing&&e.draft[VK_F12]==PET_KEY_DEFAULT);
 e.SetSize(960,600);e.Layout();check("Resizable dialog",e.HasFlag(wxRESIZE_BORDER));
 }
 pet_keyboard_reset_bindings();set_pet_business_kbd(true);
 {Editor e(nullptr);check("Business lowercase default",cap(e,'A')->GetLabel().Contains("A: a"));click(e,'A');capture(e,WXK_TAB);check("Business Tab capture",e.draft['A']==PET_KEY_TAB);}
 check("Cancel discards draft",pet_keyboard_bindings()['A']==PET_KEY_DEFAULT);
 check("wx punctuation identity",pet_key_from_wx(';')==VK_OEM_1&&pet_key_from_wx('?')==VK_OEM_2);
 wxKeyEvent press(wxEVT_KEY_DOWN);press.m_keyCode='A';pet_keyboard_handle_wx(press,true);check("wx keydown published",IsKeyDown('A'));
 wxKeyEvent shortcut(wxEVT_KEY_DOWN);shortcut.m_keyCode='O';shortcut.SetControlDown(true);pet_keyboard_handle_wx(shortcut,true);check("Shortcut releases PET input",IsKeyUp('A')&&IsKeyUp('O'));
 RawInput_SetKey(VK_SHIFT,true);RawInput_ReleaseAll();check("Focus release clears modifiers",IsKeyUp(VK_SHIFT));
 wxTheApp->OnExit();wxEntryCleanup();printf("Keyboard dialog failures: %d\n",failures);return failures?1:0;
}

