// Exercise real native controls without showing a window or requiring capture.
#include <windows.h>
#include <cstdio>
#include "pet_kbd_input.h"
#include "pet2001io.h"
#include "sys_log.h"
#include "../sys_input/rawinput.cpp"
namespace Log { void write(Level,const char*,const char*,int,const char*,...) {} }
void Pet2001IO::setKeyrows(const uint8_t[10]) {}
#include "../petsrc/pet_keyboard_dialog.cpp"

static int failures;
static void check(const char* name,bool ok) {
    printf("%s: %s\n",name,ok ? "PASS" : "FAIL");failures+=!ok;
}

static bool controls_fit(HWND dlg) {
    RECT client{}; GetClientRect(dlg,&client);
    for(HWND child=GetWindow(dlg,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT r{}; GetWindowRect(child,&r);
        MapWindowPoints(nullptr,dlg,reinterpret_cast<POINT*>(&r),2);
        if(r.left<0 || r.top<0 || r.right>client.right || r.bottom>client.bottom) return false;
    }
    return true;
}

int main() {
    // Match the production host's per-monitor awareness mode.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Editor editor;
    HWND dlg=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_KEYBOARD),nullptr,dialog_proc,(LPARAM)&editor);
    check("Native keyboard dialog created",dlg!=nullptr);
    if(!dlg) return 1;
    bool functionKeys=true;
    for(int vk=VK_F1;vk<=VK_F12;++vk) functionKeys=functionKeys && GetDlgItem(dlg,keyBase+vk)!=nullptr;
    check("Original diagram includes all twelve function keys",functionKeys);
    check("Original diagram includes system and lock keys",GetDlgItem(dlg,keyBase+VK_SNAPSHOT) && GetDlgItem(dlg,keyBase+VK_SCROLL) && GetDlgItem(dlg,keyBase+VK_NUMLOCK) && GetDlgItem(dlg,keyBase+VK_CAPITAL));
    check("Original diagram includes full numpad",GetDlgItem(dlg,keyBase+VK_NUMPAD0) && GetDlgItem(dlg,keyBase+VK_SEPARATOR) && GetDlgItem(dlg,keyBase+VK_ADD));
    HWND a=GetDlgItem(dlg,keyBase+'A');
    SendMessageW(a,BM_CLICK,0,0);
    wchar_t description[1024]{};
    GetDlgItemTextW(dlg,IDC_KBD_SELECTED,description,1024);
    check("Click asks for a physical key",wcsstr(description,L"Press")!=nullptr);
    check("Capture receives dialog navigation keys",(SendMessageW(a,WM_GETDLGCODE,0,0)&DLGC_WANTALLKEYS)!=0);
    SendMessageW(a,WM_KEYDOWN,'B',1);
    check("Click A then B changes only A to PET B",editor.draft['A']=='B' && editor.draft['B']==PET_KEY_DEFAULT);
    check("Capture remains a draft until Apply",pet_keyboard_bindings()['A']==PET_KEY_DEFAULT);
    wchar_t caption[1024]{};GetWindowTextW(a,caption,1024);
    check("Selected PC A immediately displays PET B",wcsstr(caption,L"A: B")!=nullptr);
    auto saved=editor.draft;
    SendMessageW(a,BM_CLICK,0,0);
    SendMessageW(a,WM_KEYDOWN,VK_ESCAPE,1);
    check("Escape cancels capture without closing",IsWindow(dlg) && editor.draft==saved);
    check("Escape ends capture",(SendMessageW(a,WM_GETDLGCODE,0,0)&DLGC_WANTALLKEYS)==0);
    SendMessageW(a,BM_CLICK,0,0);
    SendMessageW(a,WM_KEYDOWN,VK_F12,1);
    check("Reserved keys cannot be rebound",editor.draft==saved);
    SendMessageW(a,WM_KEYDOWN,VK_RETURN,1);
    check("Enter binds instead of accepting dialog",IsWindow(dlg) && editor.draft['A']==PET_KEY_RETURN);
    HWND b=GetDlgItem(dlg,keyBase+'B');
    GetWindowTextW(b,caption,1024);
    check("Other PC keys keep their original assignment",wcsstr(caption,L"B: B")!=nullptr);
    SendMessageW(b,BM_CLICK,0,0);
    MSG tab{};tab.hwnd=b;tab.message=WM_KEYDOWN;tab.wParam=VK_TAB;tab.lParam=1;
    if(!IsDialogMessageW(dlg,&tab)) DispatchMessageW(&tab);
    check("Tab routed to capture instead of navigating",GetFocus()==b && IsWindow(dlg));
    SendMessageW(b,BM_CLICK,0,0);
    SendMessageW(b,WM_KEYDOWN,VK_RETURN,1 | (1L<<24));
    check("Numpad Enter assigns PET Return to selected key",editor.draft['B']==PET_KEY_RETURN && editor.draft[VK_SEPARATOR]==PET_KEY_DEFAULT);
    SendMessageW(b,BM_CLICK,0,0);
    SendMessageW(b,WM_KEYDOWN,'C',1 | (1L<<30));
    check("Held-key repeats ignored during capture",editor.draft['C']==PET_KEY_DEFAULT);
    SendMessageW(b,WM_KEYDOWN,VK_ESCAPE,1);
    SendMessageW(dlg,WM_COMMAND,IDC_KBD_APPLY,0);
    check("Apply publishes captured mappings",pet_keyboard_bindings()['A']==PET_KEY_RETURN && pet_keyboard_bindings()['B']==PET_KEY_RETURN);
    SendMessageW(dlg,WM_COMMAND,IDC_KBD_KEY_DEFAULT,0);
    check("Restore defaults changes only selected PC key",editor.draft['B']==PET_KEY_DEFAULT && editor.draft['A']==PET_KEY_RETURN);
    SendMessageW(a,BM_CLICK,0,0);
    SendMessageW(a,WM_KEYDOWN,VK_ESCAPE,1);
    SendMessageW(dlg,WM_COMMAND,IDC_KBD_CLEAR,0);
    check("Clear changes only selected PC key",editor.draft['A']==PET_KEY_NONE && editor.draft[VK_RETURN]==PET_KEY_DEFAULT);
    SendMessageW(a,BM_CLICK,0,0);
    SendMessageW(a,WM_KEYDOWN,VK_LEFT,1);
    check("Num Lock off numpad 4 selects PET digit 4",editor.draft['A']=='4' && editor.draft[VK_LEFT]==PET_KEY_DEFAULT);
    SendMessageW(a,BM_CLICK,0,0);
    SendMessageW(a,WM_KEYDOWN,VK_LEFT,1 | (1L<<24));
    check("Dedicated Left selects PET cursor left",editor.draft['A']==PET_KEY_LEFT);
    HWND f1=GetDlgItem(dlg,keyBase+VK_F1);
    SendMessageW(f1,BM_CLICK,0,0);
    SendMessageW(f1,WM_KEYDOWN,'A',1);
    check("Unassigned F1 can be changed directly",editor.draft[VK_F1]=='A');
    GetWindowTextW(f1,caption,1024);
    check("F1 displays its new PET function immediately",wcsstr(caption,L"F1: A")!=nullptr);
    HWND f12=GetDlgItem(dlg,keyBase+VK_F12);
    SendMessageW(f12,BM_CLICK,0,0);
    check("Reserved F12 stays visible without starting capture",!editor.capturing && editor.draft[VK_F12]==PET_KEY_DEFAULT);
    check("Keyboard dialog is resizable",(GetWindowLongPtrW(dlg,GWL_STYLE)&WS_THICKFRAME)!=0);
    SetWindowPos(dlg,nullptr,0,0,960,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    check("All controls fit a 960 x 600 window",controls_fit(dlg));
    SetWindowPos(dlg,nullptr,0,0,1800,940,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    check("All controls fit a 1080p work area",controls_fit(dlg));
    SetWindowPos(dlg,nullptr,0,0,820,480,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    check("All controls fit after shrinking again",controls_fit(dlg));
    RECT primary={0,0,1920,1040},large={100,100,2800,1500};
    auto fitted=fit_to_work_area(large,primary);
    check("Oversized high-DPI window fits 1080p work area",
          fitted.left>=0 && fitted.top>=0 && fitted.right<=1920 && fitted.bottom<=1040);
    RECT secondary={-1920,-100,0,940};
    fitted=fit_to_work_area(large,secondary);
    check("Secondary monitor with negative coordinates",
          fitted.left>=-1920 && fitted.top>=-100 && fitted.right<=0 && fitted.bottom<=940);
    RECT suggested={0,0,2400,1500};
    SendMessageW(dlg,WM_DPICHANGED,MAKELONG(192,192),(LPARAM)&suggested);
    check("DPI transition keeps controls inside dialog",controls_fit(dlg));
    DestroyWindow(dlg);
    pet_keyboard_reset_bindings();
    set_pet_business_kbd(true);
    Editor business;
    dlg=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_KEYBOARD),nullptr,dialog_proc,(LPARAM)&business);
    check("Business PET keyboard created",dlg!=nullptr);
    check("Business view keeps original PC layout",GetDlgItem(dlg,keyBase+VK_TAB) && GetDlgItem(dlg,keyBase+VK_ESCAPE) && GetDlgItem(dlg,keyBase+VK_F12));
    SetWindowPos(dlg,nullptr,0,0,960,600,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    check("Business controls fit small window",controls_fit(dlg));
    GetWindowTextW(GetDlgItem(dlg,keyBase+'A'),caption,1024);
    check("Business lowercase default recognized",wcsstr(caption,L"Unassigned")==nullptr);
    DestroyWindow(dlg);
    printf("Keyboard dialog failures: %d\n",failures);
    return failures ? 1 : 0;
}
