#include "pet_keyboard_dialog.h"
#include "pet_kbd_input.h"
#include "iniFile.h"
#include "rawinput.h"
#include "../system/host_resource.h"
#include "../system/host_view.h"
#include <algorithm>
#include <vector>
#include <string>

namespace {
constexpr int keyBase=5000;
struct Keycap { int vk; std::wstring pc, pet; HWND window; };
struct LayoutItem { HWND window; RECT bounds; int dropHeight; };
struct Editor {
    PetKeyBindings draft=pet_keyboard_bindings();
    std::vector<Keycap> caps;
    int selected=VK_CAPITAL;
    bool shift=false;
    HFONT keyFont=nullptr, labelFont=nullptr, uiFont=nullptr;
    LOGFONTW baseFont{};
    int designWidth=0, designHeight=0, fontHeight=0;
    std::vector<LayoutItem> layout;
    ~Editor() {
        if(keyFont) DeleteObject(keyFont);
        if(labelFont) DeleteObject(labelFont);
        if(uiFont) DeleteObject(uiFont);
    }
};

RECT fit_to_work_area(RECT window, const RECT& work) {
    // Physical-pixel safety margin; also works on monitors with negative origins.
    int margin=(std::min)(16L,(std::min)(work.right-work.left,work.bottom-work.top)/8);
    LONG w=(std::min)(window.right-window.left,work.right-work.left-2*margin);
    LONG h=(std::min)(window.bottom-window.top,work.bottom-work.top-2*margin);
    window.left=(std::max)(work.left+margin,(std::min)(window.left,work.right-margin-w));
    window.top=(std::max)(work.top+margin,(std::min)(window.top,work.bottom-margin-h));
    window.right=window.left+w;window.bottom=window.top+h;
    return window;
}

void capture_layout(HWND dlg,Editor& e) {
    RECT client{};GetClientRect(dlg,&client);
    e.designWidth=client.right;e.designHeight=client.bottom;
    for(HWND child=GetWindow(dlg,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT r{};GetWindowRect(child,&r);
        MapWindowPoints(nullptr,dlg,reinterpret_cast<POINT*>(&r),2);
        int dropHeight=0;
        if(GetDlgCtrlID(child)==IDC_KBD_ASSIGN) {
            RECT drop{};SendMessageW(child,CB_GETDROPPEDCONTROLRECT,0,(LPARAM)&drop);
            dropHeight=drop.bottom-drop.top;
        }
        e.layout.push_back({child,r,dropHeight});
    }
}

void layout_editor(HWND dlg,Editor& e) {
    if(!e.designWidth || !e.designHeight) return;
    RECT client{};GetClientRect(dlg,&client);
    if(client.right<=0 || client.bottom<=0) return;
    auto area=host_fit_viewport(client.right,client.bottom,e.designWidth,e.designHeight);
    int fontHeight=-(std::max)(1, MulDiv(-e.baseFont.lfHeight,area.w,e.designWidth));
    if(e.fontHeight!=fontHeight) {
        LOGFONTW font=e.baseFont;font.lfHeight=fontHeight;
        HFONT ui=CreateFontIndirectW(&font);
        font.lfWeight=FW_SEMIBOLD;HFONT key=CreateFontIndirectW(&font);
        font.lfWeight=FW_NORMAL;font.lfHeight=-(std::max)(1,-fontHeight*9/10);
        HFONT label=CreateFontIndirectW(&font);
        if(ui && key && label) {
            for(auto& item:e.layout) SendMessageW(item.window,WM_SETFONT,(WPARAM)ui,FALSE);
            if(e.uiFont) DeleteObject(e.uiFont);
            if(e.keyFont) DeleteObject(e.keyFont);
            if(e.labelFont) DeleteObject(e.labelFont);
            e.uiFont=ui;e.keyFont=key;e.labelFont=label;e.fontHeight=fontHeight;
        } else {
            if(ui) DeleteObject(ui);if(key) DeleteObject(key);if(label) DeleteObject(label);
        }
    }
    // Always scale from the original positions, never from rounded previous sizes.
    for(auto& item:e.layout) {
        auto r=item.bounds;
        int x=area.x+MulDiv(r.left,area.w,e.designWidth);
        int y=area.y+MulDiv(r.top,area.h,e.designHeight);
        int w=MulDiv(r.right-r.left,area.w,e.designWidth);
        int h=MulDiv(item.dropHeight ? item.dropHeight : r.bottom-r.top,area.h,e.designHeight);
        SetWindowPos(item.window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
    }
    RedrawWindow(dlg,nullptr,nullptr,RDW_INVALIDATE|RDW_ALLCHILDREN|RDW_ERASE);
}

void fit_window(HWND dlg,RECT desired,HMONITOR monitor,bool center) {
    MONITORINFO info{sizeof(info)};
    if(!GetMonitorInfoW(monitor,&info)) return;
    auto r=fit_to_work_area(desired,info.rcWork);
    int w=r.right-r.left,h=r.bottom-r.top;
    if(center) {
        r.left=info.rcWork.left+(info.rcWork.right-info.rcWork.left-w)/2;
        r.top=info.rcWork.top+(info.rcWork.bottom-info.rcWork.top-h)/2;
    }
    SetWindowPos(dlg,nullptr,r.left,r.top,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
}

std::wstring reserved_label(int vk) {
    switch (vk) {
    case VK_ESCAPE: return L"Exit";
    case VK_F8: return L"Log CRT";
    case VK_F9: return L"CRT knob";
    case VK_F10: return L"CRT";
    case VK_F11: return L"Fullscr";
    case VK_F12: return L"Gfx mode";
    case VK_PRIOR: return L"CRT +";
    case VK_NEXT: return L"CRT -";
    case VK_LSHIFT: case VK_RSHIFT: return L"Modifier";
    case VK_LCONTROL: case VK_RCONTROL: return L"Shortcuts";
    case VK_LMENU: case VK_RMENU: return L"Modifier";
    default: return L"System";
    }
}

// Dialog units keep key placement proportional to the native font and DPI.
void add_key(HWND dlg,Editor& e,int vk,const wchar_t* label,float x,float y,float w=1,float h=1) {
    RECT r={int(14+x*28),int(y),int(14+(x+w)*28)-2,int(y+h*26)-2};
    MapDialogRect(dlg,&r);
    HWND button=CreateWindowExW(0,L"BUTTON",label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
        r.left,r.top,r.right-r.left,r.bottom-r.top,dlg,(HMENU)(INT_PTR)(keyBase+vk),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(button,WM_SETFONT,(WPARAM)e.keyFont,FALSE);
    e.caps.push_back({vk,label,L"",button});
}

void make_keyboard(HWND dlg,Editor& e) {
    auto add=[&](int vk,const wchar_t* s,float x,float y,float w=1,float h=1) { add_key(dlg,e,vk,s,x,y,w,h); };
    add(VK_ESCAPE,L"Esc",0,48);
    for (int i=0;i<12;++i) {
        auto name=L"F"+std::to_wstring(i+1);
        add(VK_F1+i,name.c_str(),2.f+i+(i/4)*.5f,48);
    }
    add(VK_SNAPSHOT,L"PrtSc",15.5f,48); add(VK_SCROLL,L"Scroll",16.5f,48); add(VK_PAUSE,L"Pause",17.5f,48);
    add(VK_OEM_3,L"` ~",0,86);
    for (int i=0;i<10;++i) { wchar_t s[]={wchar_t('0'+(i+1)%10),0}; add('0'+(i+1)%10,s,1.f+i,86); }
    add(VK_OEM_MINUS,L"- _",11,86); add(VK_OEM_PLUS,L"= +",12,86); add(VK_BACK,L"Backspace",13,86,2);
    add(VK_TAB,L"Tab",0,112,1.5f);
    const wchar_t* q=L"QWERTYUIOP";
    for (int i=0;i<10;++i) { wchar_t s[]={q[i],0}; add(q[i],s,1.5f+i,112); }
    add(VK_OEM_4,L"[ {",11.5f,112); add(VK_OEM_6,L"] }",12.5f,112); add(VK_OEM_5,L"\\ |",13.5f,112,1.5f);
    add(VK_CAPITAL,L"Caps Lock",0,138,1.75f);
    const wchar_t* a=L"ASDFGHJKL";
    for (int i=0;i<9;++i) { wchar_t s[]={a[i],0}; add(a[i],s,1.75f+i,138); }
    add(VK_OEM_1,L"; :",10.75f,138); add(VK_OEM_7,L"' \"",11.75f,138); add(VK_RETURN,L"Enter",12.75f,138,2.25f);
    add(VK_LSHIFT,L"Shift",0,164,2.25f);
    const wchar_t* z=L"ZXCVBNM";
    for (int i=0;i<7;++i) { wchar_t s[]={z[i],0}; add(z[i],s,2.25f+i,164); }
    add(VK_OEM_COMMA,L", <",9.25f,164); add(VK_OEM_PERIOD,L". >",10.25f,164); add(VK_OEM_2,L"/ ?",11.25f,164);
    add(VK_RSHIFT,L"Shift",12.25f,164,2.75f);
    add(VK_LCONTROL,L"Ctrl",0,190,1.25f); add(VK_LWIN,L"Win",1.25f,190,1.25f); add(VK_LMENU,L"Alt",2.5f,190,1.25f);
    add(VK_SPACE,L"Space",3.75f,190,6.25f); add(VK_RMENU,L"Alt",10,190,1.25f); add(VK_RWIN,L"Win",11.25f,190,1.25f);
    add(VK_APPS,L"Menu",12.5f,190,1.25f); add(VK_RCONTROL,L"Ctrl",13.75f,190,1.25f);
    add(VK_INSERT,L"Ins",15.5f,86); add(VK_HOME,L"Home",16.5f,86); add(VK_PRIOR,L"PgUp",17.5f,86);
    add(VK_DELETE,L"Del",15.5f,112); add(VK_END,L"End",16.5f,112); add(VK_NEXT,L"PgDn",17.5f,112);
    add(VK_UP,L"Up",16.5f,164); add(VK_LEFT,L"Left",15.5f,190); add(VK_DOWN,L"Down",16.5f,190); add(VK_RIGHT,L"Right",17.5f,190);
    add(VK_NUMLOCK,L"Num",19,86); add(VK_DIVIDE,L"/",20,86); add(VK_MULTIPLY,L"*",21,86); add(VK_SUBTRACT,L"-",22,86);
    for (int row=0;row<3;++row) for (int col=0;col<3;++col) {
        int n=7-row*3+col; wchar_t s[]={wchar_t('0'+n),0}; add(VK_NUMPAD0+n,s,19.f+col,112.f+row*26);
    }
    add(VK_ADD,L"+",22,112,1,2); add(VK_SEPARATOR,L"Enter",22,164,1,2);
    add(VK_NUMPAD0,L"0",19,190,2); add(VK_DECIMAL,L".",21,190);
}

void refresh(HWND dlg,Editor& e) {
    for (auto& c:e.caps) {
        c.pet=pet_keyboard_editable(c.vk) ? pet_keyboard_label(c.vk,e.shift,&e.draft) : reserved_label(c.vk);
        // Accessible button names include the PC key and full PET function.
        auto name=c.pc+L": "+c.pet+(e.draft[c.vk]!=PET_KEY_DEFAULT ? L" (custom)" : L"");
        SetWindowTextW(c.window,name.c_str());
        InvalidateRect(c.window,nullptr,TRUE);
    }
    bool editable=pet_keyboard_editable(e.selected);
    std::wstring pc,pet;
    for (auto& c:e.caps) if(c.vk==e.selected) {pc=c.pc;pet=c.pet;break;}
    auto description=pc+L"  ->  "+pet+(editable ? L"   |   Choose its PET assignment below." : L"   |   Reserved host/system key.");
    SetDlgItemTextW(dlg,IDC_KBD_SELECTED,description.c_str());
    EnableWindow(GetDlgItem(dlg,IDC_KBD_ASSIGN),editable);
    EnableWindow(GetDlgItem(dlg,IDC_KBD_CLEAR),editable);
    EnableWindow(GetDlgItem(dlg,IDC_KBD_KEY_DEFAULT),editable);
    HWND combo=GetDlgItem(dlg,IDC_KBD_ASSIGN);
    for (int i=0;i<SendMessageW(combo,CB_GETCOUNT,0,0);++i)
        if (SendMessageW(combo,CB_GETITEMDATA,i,0)==e.draft[e.selected]) { SendMessageW(combo,CB_SETCURSEL,i,0); break; }
    EnableWindow(GetDlgItem(dlg,IDC_KBD_APPLY),e.draft!=pet_keyboard_bindings());
}

void draw_key(const DRAWITEMSTRUCT& d,Editor& e) {
    int vk=int(d.CtlID)-keyBase;
    Keycap* cap=nullptr;
    for (auto& c:e.caps) if(c.vk==vk) {cap=&c;break;}
    if (!cap) return;
    bool selected=vk==e.selected, custom=e.draft[vk]!=PET_KEY_DEFAULT, reserved=!pet_keyboard_editable(vk);
    COLORREF bg=selected ? RGB(219,235,254) : custom ? RGB(225,244,231) : reserved ? RGB(231,233,236) : RGB(255,255,255);
    COLORREF border=selected ? RGB(32,103,184) : RGB(186,193,201);
    HBRUSH brush=CreateSolidBrush(bg); HPEN pen=CreatePen(PS_SOLID,selected?2:1,border);
    auto oldBrush=SelectObject(d.hDC,brush); auto oldPen=SelectObject(d.hDC,pen);
    RoundRect(d.hDC,d.rcItem.left+1,d.rcItem.top+1,d.rcItem.right-1,d.rcItem.bottom-1,6,6);
    SelectObject(d.hDC,oldBrush); SelectObject(d.hDC,oldPen); DeleteObject(brush); DeleteObject(pen);
    SetBkMode(d.hDC,TRANSPARENT);
    RECT top=d.rcItem; top.left+=3;top.right-=3;top.top+=3;
    top.bottom=top.top+(d.rcItem.bottom-d.rcItem.top)/2-2;
    auto font=SelectObject(d.hDC,e.keyFont);
    SetTextColor(d.hDC,RGB(35,43,54));
    DrawTextW(d.hDC,cap->pc.c_str(),-1,&top,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
    RECT bottom=d.rcItem;bottom.left+=2;bottom.right-=2;bottom.top=top.bottom;bottom.bottom-=3;
    SelectObject(d.hDC,e.labelFont);
    SetTextColor(d.hDC,reserved ? RGB(103,109,119) : RGB(28,93,62));
    std::wstring text=cap->pet==L"Unassigned" ? L"--" : cap->pet;
    DrawTextW(d.hDC,text.c_str(),-1,&bottom,DT_CENTER|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX|DT_END_ELLIPSIS);
    SelectObject(d.hDC,font);
    if(d.itemState&ODS_FOCUS) {RECT r=d.rcItem;InflateRect(&r,-4,-4);DrawFocusRect(d.hDC,&r);}
}

void apply(Editor& e) {
    for (int vk=0;vk<256;++vk) if (pet_keyboard_editable(vk)) pet_keyboard_set_binding(vk,e.draft[vk]);
    set_config_string("input","keyboard_bindings",pet_keyboard_serialize().c_str());
}

INT_PTR CALLBACK dialog_proc(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(dlg,DWLP_USER));
    if (msg==WM_INITDIALOG) {
        e=reinterpret_cast<Editor*>(lp); SetWindowLongPtrW(dlg,DWLP_USER,lp);
        // We lay out both template controls and dynamically created keys. Stop
        // the dialog manager applying a second, template-only DPI transform.
        using SetDpiBehavior=BOOL(WINAPI*)(HWND,DIALOG_DPI_CHANGE_BEHAVIORS,DIALOG_DPI_CHANGE_BEHAVIORS);
        auto setDpiBehavior=reinterpret_cast<SetDpiBehavior>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"SetDialogDpiChangeBehavior"));
        if(setDpiBehavior) setDpiBehavior(dlg,DDC_DISABLE_ALL,DDC_DISABLE_ALL);
        LOGFONTW font{}; GetObjectW((HFONT)SendMessageW(dlg,WM_GETFONT,0,0),sizeof(font),&font);
        e->baseFont=font;
        font.lfWeight=FW_SEMIBOLD;e->keyFont=CreateFontIndirectW(&font);
        font.lfWeight=FW_NORMAL; font.lfHeight=font.lfHeight*9/10;e->labelFont=CreateFontIndirectW(&font);
        make_keyboard(dlg,*e);
        auto title=get_pet_business_kbd() ? L"CBM 8032 business keyboard" : get_pet_graphics_mode() ? L"PET keyboard - graphics typing" : L"PET keyboard - business typing";
        SetDlgItemTextW(dlg,IDC_KBD_MODEL,title);
        HWND combo=GetDlgItem(dlg,IDC_KBD_ASSIGN);
        for (int a=PET_KEY_DEFAULT;a<=PET_KEY_SHIFT;++a) if (pet_keyboard_valid_action(a)) {
            auto name=pet_keyboard_action_name(a);
            if (a=='_') name+=L" (8032)";
            LRESULT index=SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)name.c_str());
            SendMessageW(combo,CB_SETITEMDATA,index,a);
        }
        refresh(dlg,*e);
        capture_layout(dlg,*e);
        RECT window{};GetWindowRect(dlg,&window);
        fit_window(dlg,window,MonitorFromWindow(GetParent(dlg) ? GetParent(dlg) : dlg,MONITOR_DEFAULTTONEAREST),true);
        layout_editor(dlg,*e);
        SetFocus(GetDlgItem(dlg,keyBase+e->selected));
        return FALSE;
    }
    if(!e) return FALSE;
    switch(msg) {
    case WM_SIZE:
        if(wp!=SIZE_MINIMIZED) layout_editor(dlg,*e);
        return TRUE;
    case WM_GETMINMAXINFO: {
        MONITORINFO info{sizeof(info)};
        if(GetMonitorInfoW(MonitorFromWindow(dlg,MONITOR_DEFAULTTONEAREST),&info)) {
            auto* limits=reinterpret_cast<MINMAXINFO*>(lp);
            limits->ptMinTrackSize.x=(std::min)(800L,info.rcWork.right-info.rcWork.left-32);
            limits->ptMinTrackSize.y=(std::min)(440L,info.rcWork.bottom-info.rcWork.top-32);
        }
        return TRUE;
    }
    case WM_DPICHANGED: {
        RECT desired=*reinterpret_cast<RECT*>(lp);
        fit_window(dlg,desired,MonitorFromRect(&desired,MONITOR_DEFAULTTONEAREST),false);
        layout_editor(dlg,*e);
        return TRUE;
    }
    case WM_EXITSIZEMOVE: {
        if(!IsZoomed(dlg)) {
            RECT window{};GetWindowRect(dlg,&window);
            fit_window(dlg,window,MonitorFromWindow(dlg,MONITOR_DEFAULTTONEAREST),false);
        }
        return TRUE;
    }
    case WM_DRAWITEM:
        if(wp>=keyBase && wp<keyBase+256) {draw_key(*reinterpret_cast<DRAWITEMSTRUCT*>(lp),*e);return TRUE;} break;
    case WM_COMMAND: {
        int id=LOWORD(wp);
        if(id>=keyBase && id<keyBase+256) {e->selected=id-keyBase;refresh(dlg,*e);return TRUE;}
        switch(id) {
        case IDC_KBD_SHIFT: e->shift=IsDlgButtonChecked(dlg,id)==BST_CHECKED;refresh(dlg,*e);return TRUE;
        case IDC_KBD_ASSIGN:
            if(HIWORD(wp)==CBN_SELCHANGE) {
                HWND combo=GetDlgItem(dlg,id);int index=(int)SendMessageW(combo,CB_GETCURSEL,0,0);
                if(index!=CB_ERR) e->draft[e->selected]=(int)SendMessageW(combo,CB_GETITEMDATA,index,0);
                refresh(dlg,*e);
            } return TRUE;
        case IDC_KBD_CLEAR: e->draft[e->selected]=PET_KEY_NONE;refresh(dlg,*e);return TRUE;
        case IDC_KBD_KEY_DEFAULT: e->draft[e->selected]=PET_KEY_DEFAULT;refresh(dlg,*e);return TRUE;
        case IDC_KBD_DEFAULTS: e->draft.fill(PET_KEY_DEFAULT);refresh(dlg,*e);return TRUE;
        case IDC_KBD_APPLY: apply(*e);refresh(dlg,*e);return TRUE;
        case IDOK: apply(*e);EndDialog(dlg,IDOK);return TRUE;
        case IDCANCEL: EndDialog(dlg,IDCANCEL);return TRUE;
        } break;
    }
    case WM_CLOSE: EndDialog(dlg,IDCANCEL);return TRUE;
    }
    return FALSE;
}
}

void pet_keyboard_load_settings() {
    auto value=get_config_string("input","keyboard_bindings","default");
    pet_keyboard_deserialize(value);delete[] value;
}

void pet_keyboard_show_dialog(HWND owner) {
    Editor editor;
    // Modal: no emulator frames/hotkeys run while inspecting or changing keys.
    DialogBoxParamW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(IDD_KEYBOARD),owner,dialog_proc,(LPARAM)&editor);
    for(int vk=0;vk<256;++vk) RawInput_ReleaseKey(vk);
}
