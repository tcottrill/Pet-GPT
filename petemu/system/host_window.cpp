// Vectrex-Emu
// Copyright (C) 2026 Tim Cottrill and Claude Code
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

// Reusable Win32/OpenGL host shell. Owns the window, menu, fullscreen, scaling,
// viewport, GL/RawInput init, and the message + frame loop. Drives a specific
// emulator via HostApp; contains no machine-specific code.
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>  // trackbar + up-down controls (CRT settings dialog)
#include <shellapi.h>  // DragAcceptFiles / DragQueryFile (drag-drop ROM/disk)
#include <stdlib.h>    // __argc, __argv
#include <cstdio>      // swprintf_s
#include <cctype>      // tolower (command-line parsing)
#include <string>

#include "host_window.h"
#include "host_app.h"
#include "host_view.h"
#include "host_resource.h"

#include "framework.h"     // glew, SCREEN_W/H externs, win_get_window decl
#include "sys_gl.h"        // InitOpenGLContext, GLSwapBuffers, DeleteGLContext, SetvSync
#include "rawinput.h"      // RawInput_Initialize/ProcessInput/Shutdown, key[], KEY_ESC
#include "wintimer.h"      // TimerInit, TimerGetTimeMS
#include "FrameLimiter.h"  // frame pacing (all speed limiting goes through this)
#include "path_helper.h"   // getpathU
#include "iniFile.h"       // SetIniFile, get/set_config_*
#include "utf8conv.h"      // win32::Utf16ToUtf8 / Utf8ToUtf16
#include "sys_log.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

// ---- Globals expected by the rest of the codebase (moved from winmain.cpp) ----
HWND hWnd = nullptr;
int  SCREEN_W = 768;
int  SCREEN_H = 960;

HWND win_get_window() { return hWnd; }

void osMessage(int ID, const char* fmt, ...)
{
    char text[1024] = "";
    if (!fmt) return;
    va_list ap; va_start(ap, fmt); vsprintf_s(text, fmt, ap); va_end(ap);
    UINT icon = (ID == IDOK) ? MB_ICONASTERISK : MB_ICONERROR;
    MessageBoxA(hWnd, text, "Message", MB_OK | icon);
}

void allegro_message(const char* title, const char* message)
{
    MessageBoxA(NULL, message, title, MB_ICONEXCLAMATION | MB_OK);
}

// ---- Host state ----
static HostApp      g_app{};
static HMENU        g_menu = nullptr;
static bool         g_running = false;
static bool         g_fromCommandLine = false;  // launched by a front-end with args -> Esc exits
static HostViewRect g_vp{ 0, 0, 0, 0 };
static bool         g_fullscreen = false;
static RECT         g_savedRect{};
static DWORD        g_savedStyle = 0;
static int          g_scale = 2;            // 1,2,3 = preset; 0 = Fit (free resize)
static std::wstring g_lastRomDir;

// Base name of the running ROM/program (used for logging/title context).
static std::string  g_currentGame;

static std::string HostRomBaseName(const char* utf8_path);

// Recompute the centered, aspect-locked viewport from the current client size.
static void HostUpdateViewport()
{
    RECT rc{};
    GetClientRect(hWnd, &rc);
    SCREEN_W = rc.right - rc.left;
    SCREEN_H = rc.bottom - rc.top;
    g_vp = host_fit_viewport(SCREEN_W, SCREEN_H, g_app.base_w, g_app.base_h);
}

// Detect the refresh rate (Hz) of the monitor the window is on. VREFRESH can
// report 0/1 for "default"; clamp to a sane range and fall back to 60.
static int HostDetectRefreshHz(HWND wnd)
{
    int hz = 0;
    HDC dc = GetDC(wnd);
    if (dc) { hz = GetDeviceCaps(dc, VREFRESH); ReleaseDC(wnd, dc); }
    if (hz < 50 || hz > 244) hz = 60;
    return hz;
}

// Enable per-monitor DPI awareness when available (Win10+); harmless otherwise.
static void HostEnableDpiAwareness()
{
    HMODULE u32 = GetModuleHandleW(L"user32");
    if (!u32) return;
    typedef BOOL(WINAPI* PFN)(DPI_AWARENESS_CONTEXT);
    PFN p = (PFN)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (p) p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

// Tick the active scale preset in the Video menu (radio style).
static void HostUpdateScaleChecks()
{
    if (!g_menu) return;
    UINT items[4] = { IDM_SCALE_FIT, IDM_SCALE_1X, IDM_SCALE_2X, IDM_SCALE_3X };
    UINT active = (g_scale >= 1 && g_scale <= 3) ? items[g_scale] : IDM_SCALE_FIT;
    CheckMenuRadioItem(g_menu, IDM_SCALE_1X, IDM_SCALE_FIT, active, MF_BYCOMMAND);
}

// ---- System ROM set (BASIC 2 / BASIC 4) ----
static int g_basic = 2; // 2 or 4 (current system ROM set)
static void HostUpdateBasicChecks() {
    if (!g_menu) return;
    CheckMenuRadioItem(g_menu, IDM_BASIC2, IDM_BASIC8032,
                       (g_basic == 8) ? IDM_BASIC8032 :
                       (g_basic == 4) ? IDM_BASIC4 : IDM_BASIC2, MF_BYCOMMAND);
}

// ---- RAM size (4 / 8 / 16 / 32 KB) ----
static int g_ram = 32; // KB
static void HostUpdateRamChecks() {
    if (!g_menu) return;
    UINT active = (g_ram == 4)  ? IDM_RAM4  :
                  (g_ram == 8)  ? IDM_RAM8  :
                  (g_ram == 16) ? IDM_RAM16 : IDM_RAM32;
    CheckMenuRadioItem(g_menu, IDM_RAM4, IDM_RAM32, active, MF_BYCOMMAND);
}

// ---- CRT look (mono monitor shader) toggle ----
static int g_crt = 0;
static void HostUpdateCrtCheck() {
    if (g_menu) CheckMenuItem(g_menu, IDM_CRT,
                              MF_BYCOMMAND | (g_crt ? MF_CHECKED : MF_UNCHECKED));
}

// ---- Emulation speed (1 = authentic, 2 = double) ----
static int g_speed2x = 0;
static void HostUpdateSpeedCheck() {
    if (g_menu) CheckMenuItem(g_menu, IDM_SPEED2X,
                              MF_BYCOMMAND | (g_speed2x ? MF_CHECKED : MF_UNCHECKED));
}

// ---- Graphics keyboard mode (Shift+letter = PET graphics chars) ----
// F12 flips this inside the emulator too, so the check is resynced from
// get_gfx_kbd() whenever a menu opens (WM_INITMENUPOPUP).
static int g_gfxKbd = 1;
static void HostUpdateKbdGfxCheck() {
    if (g_menu) CheckMenuItem(g_menu, IDM_KBDGFX,
                              MF_BYCOMMAND | (g_gfxKbd ? MF_CHECKED : MF_UNCHECKED));
}

// ---- SNES user-port adapter enable (Machine > SNES Adapter) ----
static int g_snesEnabled = 1;
static void HostUpdateSnesCheck() {
    if (g_menu) CheckMenuItem(g_menu, IDM_SNES,
                              MF_BYCOMMAND | (g_snesEnabled ? MF_CHECKED : MF_UNCHECKED));
}

// ---- Monitor color for the CRT shader (1 = green phosphor, 0 = B&W) ----
static int g_monitorGreen = 1;
static void HostUpdateMonitorChecks() {
    if (g_menu) CheckMenuRadioItem(g_menu, IDM_MONITOR_GREEN, IDM_MONITOR_BW,
                                   g_monitorGreen ? IDM_MONITOR_GREEN : IDM_MONITOR_BW,
                                   MF_BYCOMMAND);
}

// ---- CRT Monitor Settings dialog (modeless; slider + edit/spin per knob) ----
// Values flow one way: control event -> g_app.shader_set (clamps + saves ini)
// -> re-read via shader_get to refresh the row. The emulator applies uniforms
// every frame, so changes are visible live while the dialog is open.
static HWND g_dlgCrt = nullptr;

static void CrtDlgSyncRow(HWND dlg, int i)
{
    float lo, hi, st; g_app.shader_range(i, &lo, &hi, &st);
    const float v = g_app.shader_get(i);
    const int pos = (st > 0) ? (int)((v - lo) / st + 0.5f) : 0;
    SendDlgItemMessage(dlg, IDC_KNOB_SLIDER0 + i, TBM_SETPOS, TRUE, pos);
    char buf[32]; sprintf_s(buf, "%.2f", v);
    SetDlgItemTextA(dlg, IDC_KNOB_EDIT0 + i, buf);
}

static void CrtDlgSyncAll(HWND dlg)
{
    for (int i = 0; i < 7; ++i) CrtDlgSyncRow(dlg, i);
    CheckDlgButton(dlg, IDC_CRT_ENABLE,
                   (g_app.get_crt && g_app.get_crt()) ? BST_CHECKED : BST_UNCHECKED);
    CheckRadioButton(dlg, IDC_MON_GREEN, IDC_MON_BW,
                     (g_app.get_monitor && g_app.get_monitor()) ? IDC_MON_GREEN : IDC_MON_BW);
}

static INT_PTR CALLBACK CrtDlgProc(HWND dlg, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INITDIALOG:
        if (!g_app.shader_range || !g_app.shader_get) { DestroyWindow(dlg); return TRUE; }
        for (int i = 0; i < 7; ++i) {
            float lo, hi, st; g_app.shader_range(i, &lo, &hi, &st);
            const int steps = (st > 0) ? (int)((hi - lo) / st + 0.5f) : 0;
            SendDlgItemMessage(dlg, IDC_KNOB_SLIDER0 + i, TBM_SETRANGE, TRUE, MAKELPARAM(0, steps));
        }
        CrtDlgSyncAll(dlg);
        return TRUE;

    case WM_HSCROLL: {
        const int id = GetDlgCtrlID((HWND)lParam);
        if (id >= IDC_KNOB_SLIDER0 && id < IDC_KNOB_SLIDER0 + 7 && g_app.shader_set) {
            const int i = id - IDC_KNOB_SLIDER0;
            float lo, hi, st; g_app.shader_range(i, &lo, &hi, &st);
            const int pos = (int)SendDlgItemMessage(dlg, id, TBM_GETPOS, 0, 0);
            g_app.shader_set(i, lo + pos * st);
            char buf[32]; sprintf_s(buf, "%.2f", g_app.shader_get(i));
            SetDlgItemTextA(dlg, IDC_KNOB_EDIT0 + i, buf);
        }
        return TRUE; }

    case WM_NOTIFY: {
        const NMHDR* nm = (const NMHDR*)lParam;
        if (nm->code == UDN_DELTAPOS && g_app.shader_set &&
            nm->idFrom >= IDC_KNOB_SPIN0 && nm->idFrom < (UINT)(IDC_KNOB_SPIN0 + 7)) {
            const int i = (int)nm->idFrom - IDC_KNOB_SPIN0;
            const NMUPDOWN* ud = (const NMUPDOWN*)lParam;
            float lo, hi, st; g_app.shader_range(i, &lo, &hi, &st);
            g_app.shader_set(i, g_app.shader_get(i) + (ud->iDelta > 0 ? st : -st));
            CrtDlgSyncRow(dlg, i);
            return TRUE;   // we own the value; block the control's internal pos
        }
        return FALSE; }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_CRT_ENABLE: {
            const int on = (IsDlgButtonChecked(dlg, IDC_CRT_ENABLE) == BST_CHECKED) ? 1 : 0;
            g_crt = on;
            if (g_app.set_crt) g_app.set_crt(on);
            set_config_int("video", "crt", on);
            return TRUE; }
        case IDC_MON_GREEN:
        case IDC_MON_BW: {
            const int green = (LOWORD(wParam) == IDC_MON_GREEN) ? 1 : 0;
            g_monitorGreen = green;
            if (g_app.set_monitor) g_app.set_monitor(green);
            set_config_int("video", "crt_tint", green);
            return TRUE; }
        case IDC_CRT_DEFAULTS:
            if (g_app.shader_defaults) g_app.shader_defaults();
            CrtDlgSyncAll(dlg);
            return TRUE;
        case IDCANCEL:
            DestroyWindow(dlg);
            g_dlgCrt = nullptr;
            return TRUE;
        default:
            // Hand-typed values apply when the edit loses focus (Tab/click away).
            if (HIWORD(wParam) == EN_KILLFOCUS && g_app.shader_set &&
                LOWORD(wParam) >= IDC_KNOB_EDIT0 && LOWORD(wParam) < IDC_KNOB_EDIT0 + 7) {
                const int i = LOWORD(wParam) - IDC_KNOB_EDIT0;
                char buf[32] = {};
                GetDlgItemTextA(dlg, LOWORD(wParam), buf, (int)sizeof(buf));
                g_app.shader_set(i, (float)atof(buf));   // clamped inside
                CrtDlgSyncRow(dlg, i);
                return TRUE;
            }
            break;
        }
        return FALSE;

    case WM_CLOSE:
        DestroyWindow(dlg);
        g_dlgCrt = nullptr;
        return TRUE;
    }
    return FALSE;
}

static void HostShowCrtSettings(HWND owner)
{
    static bool ccInit = false;
    if (!ccInit) {
        INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_UPDOWN_CLASS | ICC_BAR_CLASSES };
        InitCommonControlsEx(&icc);
        ccInit = true;
    }
    if (g_dlgCrt) { SetForegroundWindow(g_dlgCrt); return; }
    g_dlgCrt = CreateDialogW((HINSTANCE)GetWindowLongPtr(owner, GWLP_HINSTANCE),
                             MAKEINTRESOURCEW(IDD_CRTSETTINGS), owner, CrtDlgProc);
    if (g_dlgCrt) ShowWindow(g_dlgCrt, SW_SHOW);
    else LOG_ERROR("CRT settings dialog failed to create (err=%lu)", GetLastError());
}
// Resize the windowed client to base*N, clamped to the monitor work area.
static void HostApplyScale(int n)
{
    g_scale = n;
    HostUpdateScaleChecks();
    if (n < 1 || g_fullscreen) return; // Fit, or no-op while fullscreen

    int cw = g_app.base_w * n;
    int ch = g_app.base_h * n;

    RECT wa{};
    SystemParametersInfo(SPI_GETWORKAREA, 0, &wa, 0);
    int maxw = wa.right - wa.left;
    int maxh = wa.bottom - wa.top;

    RECT wr{ 0, 0, cw, ch };
    AdjustWindowRect(&wr, (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE), TRUE);
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;
    if (ww > maxw || wh > maxh) return; // too big for this screen; keep current

    SetWindowPos(hWnd, NULL, 0, 0, ww, wh, SWP_NOMOVE | SWP_NOZORDER);
    HostUpdateViewport();
}

// Toggle borderless fullscreen on the window's current monitor.
static void HostToggleFullscreen()
{
    if (!g_fullscreen) {
        GetWindowRect(hWnd, &g_savedRect);
        g_savedStyle = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);

        MONITORINFO mi{ sizeof(mi) };
        GetMonitorInfo(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST), &mi);

        SetMenu(hWnd, NULL);
        SetWindowLongPtr(hWnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hWnd, HWND_TOP,
                     mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = true;
    } else {
        SetWindowLongPtr(hWnd, GWL_STYLE, g_savedStyle);
        SetMenu(hWnd, g_menu);
        SetWindowPos(hWnd, HWND_NOTOPMOST,
                     g_savedRect.left, g_savedRect.top,
                     g_savedRect.right - g_savedRect.left,
                     g_savedRect.bottom - g_savedRect.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        g_fullscreen = false;

        // A scale preset picked WHILE fullscreen updated g_scale/menu/ini but
        // couldn't resize; apply it now so the restored window matches what
        // the menu (and the persisted ini) claim.
        if (g_scale >= 1)
            HostApplyScale(g_scale);
    }
    HostUpdateViewport();
}

// --- Command-line options (parsed in host_run; the command line wins over ini) ---
struct HostCmdLine {
    std::string rom;          // -rom <path|name>, or a bare non-option token
    int  fullscreen = -1;     // -fullscreen=1 / -window=0   (-1 = unset)
    int  scale      = -1;     // -scale: 0=fit, 1/2/3        (-1 = unset)
    bool help       = false;
};
static HostCmdLine g_cmd;

static std::string HostLowerA(const char* s)
{
    std::string r(s ? s : "");
    for (char& c : r) c = (char)tolower((unsigned char)c);
    return r;
}

// Front-end-friendly command line:  petemu [options] [romfile]
//   -rom <file>  -fullscreen  -window  -scale <1|2|3|fit>  -h
// A bare (non-dashed) token is taken as the ROM. Applied in host_run AFTER the
// ini is read, so anything here overrides the saved setting.
static void HostParseCommandLine(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        if (!argv[i] || !argv[i][0]) continue;
        std::string a = HostLowerA(argv[i]);
        if      (a == "-rom" && i + 1 < argc)   g_cmd.rom = argv[++i];
        else if (a == "-fullscreen")            g_cmd.fullscreen = 1;
        else if (a == "-window")                g_cmd.fullscreen = 0;
        else if (a == "-scale" && i + 1 < argc) {
            std::string v = HostLowerA(argv[++i]);
            g_cmd.scale = (v == "fit") ? 0 : atoi(v.c_str());
            if (g_cmd.scale < 0 || g_cmd.scale > 3) g_cmd.scale = 0;   // out of range -> fit
        }
        else if (a == "-h" || a == "-help" || a == "--help" || a == "-?" || a == "/?")
            g_cmd.help = true;
        else if (argv[i][0] != '-' && argv[i][0] != '/' && g_cmd.rom.empty())
            g_cmd.rom = argv[i];   // bare token = ROM
    }
}

static void HostShowUsage()
{
    MessageBoxW(NULL,
        L"petemu [options] [romfile]\n\n"
        L"  -rom <file>       Load a program/disk (full path or name)\n"
        L"  -fullscreen       Start in fullscreen\n"
        L"  -window           Start in a window\n"
        L"  -scale <n>        Window scale: 1, 2, 3, or fit\n"
        L"  -h                Show this help\n\n"
        L"Command-line options override the matching settings saved in the ini.",
        L"petemu", MB_OK | MB_ICONINFORMATION);
}

// Resolve a -rom value: use it as-is if it exists, else return unchanged.
static std::string HostResolveRomPath(const std::string& rom)
{
    if (rom.empty()) return rom;
    if (GetFileAttributesA(rom.c_str()) != INVALID_FILE_ATTRIBUTES) return rom;
    return rom;   // not found; let load_rom log the error
}

// Load a ROM by path. Shared by the File dialog, drag-drop, and -rom.
static void HostLoadRomPath(const char* utf8_path)
{
    if (g_app.load_rom) g_app.load_rom(utf8_path);
    g_currentGame = HostRomBaseName(utf8_path);
}

static void HostLoadRomDialog()
{
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hWnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = g_app.rom_filter;
    ofn.nFilterIndex = 1;
    ofn.lpstrInitialDir = g_lastRomDir.empty() ? L"." : g_lastRomDir.c_str();
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn)) {
        if (ofn.nFileOffset > 0) g_lastRomDir.assign(file, file + ofn.nFileOffset);
        std::string utf8 = win32::Utf16ToUtf8(file);
        HostLoadRomPath(utf8.c_str());
    }
}

// Show the menu items as a popup (so the menu is reachable in fullscreen).
static void HostShowPopupMenu(HWND wnd)
{
    POINT pt; GetCursorPos(&pt);
    HINSTANCE inst = (HINSTANCE)GetWindowLongPtr(wnd, GWLP_HINSTANCE);
    HMENU bar = LoadMenuW(inst, MAKEINTRESOURCEW(IDR_HOST_MENU));
    if (!bar) return;
    HMENU popup = CreatePopupMenu();
    int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; i++) {
        wchar_t name[64] = {};
        GetMenuStringW(bar, i, name, 64, MF_BYPOSITION);
        HMENU sub = GetSubMenu(bar, i);
        AppendMenuW(popup, MF_POPUP, (UINT_PTR)sub, name);
    }
    SetForegroundWindow(wnd);   // required so the popup dismisses on outside clicks
    TrackPopupMenu(popup, TPM_RIGHTBUTTON, pt.x, pt.y, 0, wnd, NULL);

    // The submenu handles are owned by BOTH menus after AppendMenuW(MF_POPUP):
    // destroying 'popup' would recursively destroy them, then DestroyMenu(bar)
    // would operate on dead handles. Detach them from 'popup' first so only
    // 'bar' destroys the shared submenus.
    for (int i = GetMenuItemCount(popup) - 1; i >= 0; --i)
        RemoveMenu(popup, i, MF_BYPOSITION);
    DestroyMenu(popup);
    DestroyMenu(bar);
}

// Base name (no directory, no extension) of a path. Used for logging context.
static std::string HostRomBaseName(const char* utf8_path)
{
    std::string p(utf8_path ? utf8_path : "");
    size_t slash = p.find_last_of("/\\");
    std::string f = (slash == std::string::npos) ? p : p.substr(slash + 1);
    size_t dot = f.find_last_of('.');
    return (dot == std::string::npos) ? f : f.substr(0, dot);
}

static LRESULT CALLBACK HostWndProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_INPUT:
        return RawInput_ProcessInput(wnd, wParam, lParam);

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_LOADROM:    HostLoadRomDialog(); return 0;
        case IDM_EJECT:      if (g_app.eject_disk) g_app.eject_disk(); return 0;
        case IDM_RESET:      if (g_app.reset) g_app.reset(); return 0;
        case IDM_EXIT:       PostMessage(wnd, WM_CLOSE, 0, 0); return 0;
        case IDM_FULLSCREEN: HostToggleFullscreen(); return 0;
        case IDM_SCALE_1X:   HostApplyScale(1); return 0;
        case IDM_SCALE_2X:   HostApplyScale(2); return 0;
        case IDM_SCALE_3X:   HostApplyScale(3); return 0;
        case IDM_SCALE_FIT:  HostApplyScale(0); return 0;
        case IDM_ABOUT:
            MessageBoxA(wnd, g_app.about_text ? g_app.about_text : "",
                        "About", MB_OK | MB_ICONINFORMATION);
            return 0;
        case IDM_BASIC2:
        case IDM_BASIC4:
        case IDM_BASIC8032: {
            int want = (LOWORD(wParam) == IDM_BASIC8032) ? 8 :
                       (LOWORD(wParam) == IDM_BASIC4) ? 4 : 2;
            if (want != g_basic && g_app.set_basic) {
                g_basic = want;
                g_app.set_basic(g_basic);
                set_config_int("machine", "basic", g_basic);
                HostUpdateBasicChecks();
            }
            return 0;
        }
        case IDM_RAM4:
        case IDM_RAM8:
        case IDM_RAM16:
        case IDM_RAM32: {
            int want = (LOWORD(wParam) == IDM_RAM4)  ? 4  :
                       (LOWORD(wParam) == IDM_RAM8)  ? 8  :
                       (LOWORD(wParam) == IDM_RAM16) ? 16 : 32;
            if (want != g_ram && g_app.set_ram) {
                g_ram = want;
                g_app.set_ram(g_ram);
                set_config_int("machine", "ram", g_ram);
                HostUpdateRamChecks();
            }
            return 0;
        }
        case IDM_CRT:
            g_crt = !g_crt;
            if (g_app.set_crt) g_app.set_crt(g_crt);
            set_config_int("video", "crt", g_crt);
            HostUpdateCrtCheck();
            return 0;
        case IDM_SPEED2X:
            g_speed2x = !g_speed2x;
            if (g_app.set_speed) g_app.set_speed(g_speed2x ? 2 : 1);
            set_config_int("machine", "speed2x", g_speed2x);
            HostUpdateSpeedCheck();
            return 0;
        case IDM_KBDGFX:
            g_gfxKbd = !g_gfxKbd;
            if (g_app.set_gfx_kbd) g_app.set_gfx_kbd(g_gfxKbd);
            set_config_int("input", "graphics_kbd", g_gfxKbd);
            HostUpdateKbdGfxCheck();
            return 0;
        case IDM_SNES:
            g_snesEnabled = !g_snesEnabled;
            if (g_app.set_snes) g_app.set_snes(g_snesEnabled);
            set_config_bool("input", "snes_adapter", g_snesEnabled != 0);
            HostUpdateSnesCheck();
            return 0;
        case IDM_CRT_SETTINGS:
            HostShowCrtSettings(wnd);
            return 0;
        case IDM_MONITOR_GREEN:
        case IDM_MONITOR_BW: {
            int want = (LOWORD(wParam) == IDM_MONITOR_GREEN) ? 1 : 0;
            if (want != g_monitorGreen) {
                g_monitorGreen = want;
                if (g_app.set_monitor) g_app.set_monitor(g_monitorGreen);
                set_config_int("video", "crt_tint", g_monitorGreen);
                HostUpdateMonitorChecks();
            }
            return 0;
        }
        }
        return 0;

    case WM_DROPFILES: {
        HDROP h = (HDROP)wParam;
        wchar_t path[MAX_PATH] = {};
        if (DragQueryFileW(h, 0, path, MAX_PATH)) {
            std::string utf8 = win32::Utf16ToUtf8(path);
            if (g_app.load_rom) g_app.load_rom(utf8.c_str());
        }
        DragFinish(h);
        return 0;
    }

    case WM_SYSCOMMAND:
        // Left Alt and F10 are game buttons (MAME default has P1 button 2 = Alt),
        // not menu activators. Swallow keyboard menu activation so pressing them
        // doesn't grab the menu bar and start eating keystrokes. The menu stays
        // reachable by mouse and the right-click popup; Alt+Enter / F11 fullscreen
        // still work (those fire via the accelerator table before this).
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        return DefWindowProc(wnd, msg, wParam, lParam);

    case WM_INITMENUPOPUP:
        // Grey "Eject Disk" unless a disk is mounted. Queried fresh each time a
        // popup opens; wParam is the submenu being shown (works for both the
        // menu bar's File popup and the right-click popup, which share the
        // File submenu's command ids).
        if (g_app.get_disk_mounted) {
            UINT flags = MF_BYCOMMAND | (g_app.get_disk_mounted() ? MF_ENABLED : MF_GRAYED);
            EnableMenuItem((HMENU)wParam, IDM_EJECT, flags);
        }
        // F12 flips the graphics-keyboard mode outside the menu; resync the
        // checkmark from the emulator's live state whenever a menu opens.
        if (g_app.get_gfx_kbd) {
            g_gfxKbd = g_app.get_gfx_kbd();
            HostUpdateKbdGfxCheck();
        }
        return 0;

    case WM_RBUTTONUP:
        HostShowPopupMenu(wnd);
        return 0;

    case WM_SIZE:
        HostUpdateViewport();
        return 0;

    case WM_ERASEBKGND:
        return 1; // GL clears every frame; skip GDI erase to avoid flicker.

    case WM_SETCURSOR:
        // Hide the pointer over the render (client) area; Windows still draws the
        // normal arrow over the menu bar, title bar, and borders.
        if (LOWORD(lParam) == HTCLIENT) { SetCursor(NULL); return TRUE; }
        return DefWindowProc(wnd, msg, wParam, lParam);

    case WM_CLOSE:
        g_running = false;
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProc(wnd, msg, wParam, lParam);
    }
}

int host_run(HINSTANCE hInstance, int nCmdShow, const HostApp* app)
{
    g_app = *app;

    LogOpen("petemu-log.txt");
    LOG_INFO("host_run: starting '%ls'", app->title);
    HostEnableDpiAwareness();

    // Front-end command line (applied below; overrides matching ini settings).
    HostParseCommandLine(__argc, __argv);
    if (g_cmd.help) { HostShowUsage(); LogClose(); return 0; }
    // Any argument beyond argv[0] means a front-end launched us; in that mode Esc
    // shuts the emulator down (returns control to the launcher) instead of just
    // minimizing the window the way a standalone GUI session does.
    g_fromCommandLine = (__argc > 1);
    LOG_INFO("cmdline: rom='%s' scale=%d fullscreen=%d fromCmdLine=%d",
             g_cmd.rom.c_str(), g_cmd.scale, g_cmd.fullscreen,
             g_fromCommandLine ? 1 : 0);

    // Run from the executable's directory (front-ends may launch elsewhere).
    {
        std::wstring exedir = getpathU(0, 0);
        SetCurrentDirectoryW(exedir.c_str());
    }
    SetIniFile("pet.ini");

    WNDCLASSW wc{};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = hInstance;
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"EmulatorHost";
    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Window registration failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    g_menu = LoadMenuW(hInstance, MAKEINTRESOURCEW(IDR_HOST_MENU));

    RECT wr{ 0, 0, app->base_w * 2, app->base_h * 2 }; // initial 2x; adjusted below
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&wr, style, TRUE); // TRUE: window has a menu
    const int ww = wr.right - wr.left;
    const int wh = wr.bottom - wr.top;
    const int px = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    const int py = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    hWnd = CreateWindowW(L"EmulatorHost", app->title, style,
                         px, py, ww, wh, NULL, g_menu, hInstance, NULL);
    if (!hWnd) {
        MessageBoxW(NULL, L"Window creation failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Set the title-bar (small) and Alt-Tab/taskbar (big) icons explicitly so each
    // is rasterized from the best-matching size in the .ico rather than scaled.
    if (HICON big = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                      IMAGE_ICON, 0, 0, LR_DEFAULTSIZE))
        SendMessageW(hWnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (HICON sm = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                     IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                     GetSystemMetrics(SM_CYSMICON), 0))
        SendMessageW(hWnd, WM_SETICON, ICON_SMALL, (LPARAM)sm);

    ShowWindow(hWnd, nCmdShow);
    DragAcceptFiles(hWnd, TRUE);   // accept dropped program/disk files
    UpdateWindow(hWnd);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);

    InitOpenGLContext(false, false, true);
    glewInit();
    // vsync OFF: FrameLimiter is the sole pacer. vsync + FrameLimiter both target
    // the refresh and fight each other, so they are mutually exclusive here.
    if (WGLEW_EXT_swap_control) SetvSync(false);
    RawInput_Initialize(hWnd);

    timeBeginPeriod(1);
    TimerInit();
    HostUpdateViewport();

    // Restore persisted view settings.
    g_scale = (g_cmd.scale >= 0) ? g_cmd.scale : get_config_int("video", "scale", 2);
    {
        char* dir = get_config_string("paths", "lastromdir", "");
        if (dir) { g_lastRomDir = win32::Utf8ToUtf16(dir); free(dir); }
    }
    HostApplyScale(g_scale);
    bool startFullscreen = (g_cmd.fullscreen >= 0) ? (g_cmd.fullscreen != 0)
                                                   : get_config_bool("video", "fullscreen", false);
    if (startFullscreen) HostToggleFullscreen();

    // Pace to the monitor's refresh rate so 50 Hz content doesn't judder/double on
    // a 60 Hz panel. target_fps (if set) overrides the detected refresh.
    int refreshHz = HostDetectRefreshHz(hWnd);
    double paceHz = (app->target_fps > 0.0) ? app->target_fps : (double)refreshHz;
    LOG_INFO("host_run: monitor %d Hz -> pacing %.2f fps", refreshHz, paceHz);

    if (app->init) app->init(__argc, __argv);

    // Restore the saved system ROM set (BASIC 2 / 4) and tick the menu.
    g_basic = get_config_int("machine", "basic", 2);
    if (g_basic != 2 && g_basic != 4 && g_basic != 8) g_basic = 2;
    HostUpdateBasicChecks();

    // Restore the saved RAM size and tick the menu (emu_init already applied it).
    g_ram = get_config_int("machine", "ram", 32);
    if (g_ram != 4 && g_ram != 8 && g_ram != 16 && g_ram != 32) g_ram = 32;
    HostUpdateRamChecks();

    // Restore the saved CRT look toggle and tick the menu.
    g_crt = get_config_int("video", "crt", 0);
    if (g_app.set_crt) g_app.set_crt(g_crt);
    HostUpdateCrtCheck();

    // Restore the saved monitor color (crt_tint: 1 = green, 0 = B&W) and tick
    // the menu. The emulator also read this key in its own init; this keeps
    // the menu radio and emulator in sync from one source of truth.
    g_monitorGreen = get_config_int("video", "crt_tint", 1) ? 1 : 0;
    if (g_app.set_monitor) g_app.set_monitor(g_monitorGreen);
    HostUpdateMonitorChecks();

    // Restore the saved speed toggle and tick the menu.
    g_speed2x = get_config_int("machine", "speed2x", 0) ? 1 : 0;
    if (g_app.set_speed) g_app.set_speed(g_speed2x ? 2 : 1);
    HostUpdateSpeedCheck();

    // Restore the saved graphics-keyboard mode and tick the menu.
    g_gfxKbd = get_config_int("input", "graphics_kbd", 1) ? 1 : 0;
    if (g_app.set_gfx_kbd) g_app.set_gfx_kbd(g_gfxKbd);
    HostUpdateKbdGfxCheck();

    // Restore the saved SNES adapter enable and tick the menu. (emu_init also
    // reads [input] snes_adapter; this keeps the menu checkmark in sync.)
    g_snesEnabled = get_config_bool("input", "snes_adapter", true) ? 1 : 0;
    if (g_app.set_snes) g_app.set_snes(g_snesEnabled);
    HostUpdateSnesCheck();

    // A -rom on the command line loads that program/disk at startup.
    if (!g_cmd.rom.empty()) HostLoadRomPath(HostResolveRomPath(g_cmd.rom).c_str());

    HACCEL accel = LoadAcceleratorsW(hInstance, MAKEINTRESOURCEW(IDR_HOST_ACCEL));
    MSG msg{};
    g_running = true;

    FrameLimiter::Init(paceHz);   // all frame pacing goes through FrameLimiter

    while (g_running) {
        if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_running = false;
            } else {
                if (g_dlgCrt && IsDialogMessage(g_dlgCrt, &msg)) continue;
                if (!TranslateAccelerator(hWnd, accel, &msg)) {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
        } else {
            glViewport(g_vp.x, g_vp.y, g_vp.w, g_vp.h);
            if (!app->run_frame()) {
                // Esc: a standalone fullscreen window drops back to windowed first;
                // every other case (any window, or a front-end-launched instance) quits.
                if (g_fullscreen && !g_fromCommandLine) {
                    HostToggleFullscreen(); // leave fullscreen, keep running
                    key[KEY_ESC] = 0;       // consume so it doesn't re-trigger
                } else {
                    g_running = false;
                }
            } else {
                GLSwapBuffers();
            }

            // Pace to the target rate. vsync (if on) prevents tearing; this caps
            // the emulation rate precisely and works even when vsync is unavailable.
            FrameLimiter::Throttle();
        }
    }

    // Persist view settings.
    set_config_int("video", "scale", g_scale);
    set_config_bool("video", "fullscreen", g_fullscreen);
    set_config_int("machine", "basic", g_basic);
    set_config_int("machine", "ram", g_ram);
    set_config_int("video", "crt", g_crt);
    set_config_int("video", "crt_tint", g_monitorGreen);
    set_config_int("machine", "speed2x", g_speed2x);
    // Save the LIVE mode (F12 may have flipped it since the last menu click).
    set_config_int("input", "graphics_kbd",
                   g_app.get_gfx_kbd ? g_app.get_gfx_kbd() : g_gfxKbd);
    if (!g_lastRomDir.empty())
        set_config_string("paths", "lastromdir", win32::Utf16ToUtf8(g_lastRomDir).c_str());

    if (app->shutdown) app->shutdown();
    FrameLimiter::Shutdown();
    DeleteGLContext();
    RawInput_Shutdown();
    LOG_INFO("host_run: exiting");
    LogClose();
    DestroyWindow(hWnd);
    return 0;
}
