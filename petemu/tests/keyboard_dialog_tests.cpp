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
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
    Editor editor;
    HWND dlg=CreateDialogParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_KEYBOARD),nullptr,dialog_proc,(LPARAM)&editor);
    check("Native keyboard dialog created",dlg!=nullptr);
    if(!dlg) return 1;
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
    printf("Keyboard dialog failures: %d\n",failures);
    return failures ? 1 : 0;
}
