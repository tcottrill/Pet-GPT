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
#include "monitor_select.h" // stable multi-monitor selection (vendored from shared/monitor_select)
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
static UINT         g_savedDpi = 96;        // window DPI when g_savedRect was captured
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

// Per-Monitor V2 awareness is declared in petemu.manifest (the recommended way).
// This is only a fallback for an exe without that manifest: try V2, then V1.
// Harmless (fails with ACCESS_DENIED) when the manifest already set it.
static void HostEnableDpiAwareness()
{
    HMODULE u32 = GetModuleHandleW(L"user32");
    if (!u32) return;
    typedef BOOL(WINAPI* PFN)(DPI_AWARENESS_CONTEXT);
    PFN p = (PFN)GetProcAddress(u32, "SetProcessDpiAwarenessContext");
    if (!p) return;
    if (!p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE);
}

// ---- DPI helpers (Win10 1607+ APIs loaded dynamically; 96 dpi fallbacks) ----
static UINT HostWindowDpi(HWND wnd)
{
    typedef UINT(WINAPI* PFN)(HWND);
    static PFN p = (PFN)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow");
    UINT dpi = (p && wnd) ? p(wnd) : 0;
    return dpi ? dpi : 96;
}

// Effective DPI of a monitor (shcore GetDpiForMonitor, Win8.1+); 96 if unavailable.
static UINT HostMonitorDpi(HMONITOR mon)
{
    typedef HRESULT(WINAPI* PFN)(HMONITOR, int, UINT*, UINT*);
    static HMODULE sh = LoadLibraryW(L"shcore.dll");
    static PFN p = sh ? (PFN)GetProcAddress(sh, "GetDpiForMonitor") : nullptr;
    UINT dx = 0, dy = 0;
    if (p && mon && SUCCEEDED(p(mon, 0 /*MDT_EFFECTIVE_DPI*/, &dx, &dy)) && dx) return dx;
    return 96;
}

static void HostAdjustRectForDpi(RECT* rc, DWORD style, BOOL menu, DWORD exStyle, UINT dpi)
{
    typedef BOOL(WINAPI* PFN)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static PFN p = (PFN)GetProcAddress(GetModuleHandleW(L"user32"), "AdjustWindowRectExForDpi");
    if (p) p(rc, style, menu, exStyle, dpi);
    else   AdjustWindowRectEx(rc, style, menu, exStyle);
}

// Title-bar (small) and Alt-Tab/taskbar (big) icons, each rasterized from the
// best-matching size in the .ico at the given DPI rather than scaled.
static void HostSetWindowIcons(HWND wnd, HINSTANCE inst, UINT dpi)
{
    typedef int(WINAPI* PFN)(int, UINT);
    static PFN p = (PFN)GetProcAddress(GetModuleHandleW(L"user32"), "GetSystemMetricsForDpi");
    auto metric = [&](int idx) { return p ? p(idx, dpi) : GetSystemMetrics(idx); };
    if (HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                      metric(SM_CXICON), metric(SM_CYICON), 0))
        if (HICON old = (HICON)SendMessageW(wnd, WM_SETICON, ICON_BIG, (LPARAM)big)) DestroyIcon(old);
    if (HICON sm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                     metric(SM_CXSMICON), metric(SM_CYSMICON), 0))
        if (HICON old = (HICON)SendMessageW(wnd, WM_SETICON, ICON_SMALL, (LPARAM)sm)) DestroyIcon(old);
}

// Outer window size for scale preset n (1..3) at the given DPI: the client is
// base*N/2 logical pixels (1x = 320x240 for a 640x480 base) converted to
// physical pixels, plus frame and menu bar.
static void HostPresetWindowSize(int n, DWORD style, DWORD exStyle, UINT dpi, int* ww, int* wh)
{
    RECT wr{ 0, 0, MulDiv(g_app.base_w * n, (int)dpi, 192), MulDiv(g_app.base_h * n, (int)dpi, 192) };
    HostAdjustRectForDpi(&wr, style, TRUE /*has menu*/, exStyle, dpi);
    *ww = wr.right - wr.left;
    *wh = wr.bottom - wr.top;
}

// Does preset n fit inside work area wa at this DPI (frame and menu included)?
static bool HostPresetFits(int n, const RECT& wa, DWORD style, DWORD exStyle, UINT dpi)
{
    int ww, wh;
    HostPresetWindowSize(n, style, exStyle, dpi, &ww, &wh);
    return ww <= wa.right - wa.left && wh <= wa.bottom - wa.top;
}

// The preset actually used: the largest one <= the chosen g_scale that fits wa
// at this DPI; 0 (Fit) when g_scale is Fit or not even 1x fits.
static int HostEffectiveScale(const RECT& wa, DWORD style, DWORD exStyle, UINT dpi)
{
    for (int n = (g_scale > 3 ? 3 : g_scale); n >= 1; --n)
        if (HostPresetFits(n, wa, style, exStyle, dpi)) return n;
    return 0;
}

// Largest window with the base 4:3 client aspect whose outer rect (frame and
// menu included) fits wa at this DPI, centered in wa.
static RECT HostFitWindowRect(const RECT& wa, DWORD style, DWORD exStyle, UINT dpi)
{
    RECT z{ 0, 0, 0, 0 };
    HostAdjustRectForDpi(&z, style, TRUE /*has menu*/, exStyle, dpi);
    const int extraW = z.right - z.left, extraH = z.bottom - z.top;
    int availW = (wa.right - wa.left) - extraW;
    int availH = (wa.bottom - wa.top) - extraH;
    if (availW < 1) availW = 1;
    if (availH < 1) availH = 1;
    const int bw = g_app.base_w > 0 ? g_app.base_w : 640;
    const int bh = g_app.base_h > 0 ? g_app.base_h : 480;
    int cw = MulDiv(availH, bw, bh);
    int ch = availH;
    if (cw > availW) { cw = availW; ch = MulDiv(availW, bh, bw); }
    const int ww = cw + extraW, wh = ch + extraH;
    const int x = wa.left + ((wa.right - wa.left) - ww) / 2;
    const int y = wa.top + ((wa.bottom - wa.top) - wh) / 2;
    return RECT{ x, y, x + ww, y + wh };
}

// Style the window has (or will have again) while windowed.
static DWORD HostWindowedStyle()
{
    return g_fullscreen ? g_savedStyle : (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
}

// Tick the EFFECTIVE scale in the Video menu (radio style): Fit in fit mode or
// when no preset fits the current monitor.
static void HostUpdateScaleChecks()
{
    if (!g_menu) return;
    UINT items[4] = { IDM_SCALE_FIT, IDM_SCALE_1X, IDM_SCALE_2X, IDM_SCALE_3X };
    int eff = 0;
    if (g_scale >= 1 && hWnd)
        eff = HostEffectiveScale(monsel::FromWindow(hWnd).workRect, HostWindowedStyle(),
                                 (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE), HostWindowDpi(hWnd));
    CheckMenuRadioItem(g_menu, IDM_SCALE_1X, IDM_SCALE_FIT, items[eff], MF_BYCOMMAND);
}

// Grey the presets that do not fit the window's current monitor.
static void HostUpdateScaleEnables(HMENU menu)
{
    if (!hWnd) return;
    const RECT wa = monsel::FromWindow(hWnd).workRect;
    const DWORD st = HostWindowedStyle(), ex = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    const UINT dpi = HostWindowDpi(hWnd);
    const UINT ids[3] = { IDM_SCALE_1X, IDM_SCALE_2X, IDM_SCALE_3X };
    for (int n = 1; n <= 3; ++n)
        EnableMenuItem(menu, ids[n - 1],
                       MF_BYCOMMAND | (HostPresetFits(n, wa, st, ex, dpi) ? MF_ENABLED : MF_GRAYED));
}

static void HostLogScaleFallback(int eff)
{
    if (g_scale >= 1 && eff != g_scale) {
        if (eff >= 1) LOG_INFO("scale %dx does not fit this monitor, using %dx", g_scale, eff);
        else          LOG_INFO("scale %dx does not fit this monitor, using Fit", g_scale);
    }
}

// ---- System ROM set (BASIC 2 / BASIC 4) ----
static int g_basic = 2; // 2 or 4 (current system ROM set)
static void HostUpdateBasicChecks() {
    if (!g_menu) return;
    CheckMenuRadioItem(g_menu, IDM_BASIC2, IDM_BASIC1,
                       (g_basic == 1) ? IDM_BASIC1 :
                       (g_basic == 12) ? IDM_BASIC4032 :
                       (g_basic == 8) ? IDM_BASIC8032 :
                       (g_basic == 4) ? IDM_BASIC4 : IDM_BASIC2, MF_BYCOMMAND);
}

// ---- RAM size (4 / 8 / 16 / 32 KB) ----
static int g_ram = 32; // KB
static void HostUpdateRamChecks() {
    if (!g_menu) return;
    for (UINT id = IDM_RAM4; id <= IDM_RAM32; ++id)
        EnableMenuItem(g_menu, id, MF_BYCOMMAND | ((g_basic == 8 && id != IDM_RAM32) || (g_basic == 1 && id != IDM_RAM8) ? MF_GRAYED : MF_ENABLED));
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
// Apply the user's chosen scale n (0 = Fit, 1..3 = preset; persisted at exit via
// g_scale). The window uses the EFFECTIVE preset: the largest <= n that fits the
// work area of the monitor it is on (preset = base*N/2 logical pixels, at that
// monitor's DPI). If none fits, or n is Fit, the window is sized to the largest
// base-aspect rectangle that fits and centered. A click is never ignored.
static void HostApplyScale(int n)
{
    g_scale = n;
    HostUpdateScaleChecks();
    if (g_fullscreen) return; // applied when leaving fullscreen

    // Work area of the monitor the window is on (not necessarily the primary).
    const RECT wa = monsel::FromWindow(hWnd).workRect;
    const DWORD st = (DWORD)GetWindowLongPtr(hWnd, GWL_STYLE);
    const DWORD ex = (DWORD)GetWindowLongPtr(hWnd, GWL_EXSTYLE);
    const UINT dpi = HostWindowDpi(hWnd);
    const int eff = HostEffectiveScale(wa, st, ex, dpi);
    HostLogScaleFallback(eff);

    if (eff < 1) {
        const RECT f = HostFitWindowRect(wa, st, ex, dpi);
        SetWindowPos(hWnd, NULL, f.left, f.top, f.right - f.left, f.bottom - f.top, SWP_NOZORDER);
        HostUpdateViewport();
        return;
    }

    int ww, wh;
    HostPresetWindowSize(eff, st, ex, dpi, &ww, &wh);

    // Keep the resized window inside the work area: shift it back if it would
    // hang off the right/bottom edge (and never above/left of the work area).
    RECT cur{};
    GetWindowRect(hWnd, &cur);
    int nx = cur.left, ny = cur.top;
    if (nx + ww > wa.right)  nx = wa.right - ww;
    if (ny + wh > wa.bottom) ny = wa.bottom - wh;
    if (nx < wa.left) nx = wa.left;
    if (ny < wa.top)  ny = wa.top;

    SetWindowPos(hWnd, NULL, nx, ny, ww, wh, SWP_NOZORDER);
    HostUpdateViewport();
}

// Toggle borderless fullscreen on the window's current monitor.
static void HostToggleFullscreen()
{
    if (!g_fullscreen) {
        GetWindowRect(hWnd, &g_savedRect);
        g_savedDpi = HostWindowDpi(hWnd);
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
        // Restore onto the monitor the fullscreen window is on NOW. Its DPI may
        // differ from the one the rect was saved at (window was moved while
        // fullscreen, or Windows scaling changed), so rescale the saved size;
        // if the saved spot is on another monitor, center in this work area.
        const monsel::MonitorDesc mon = monsel::FromWindow(hWnd);
        const UINT dpiNow = HostMonitorDpi(mon.handle);
        int rw = MulDiv(g_savedRect.right - g_savedRect.left, (int)dpiNow, (int)g_savedDpi);
        int rh = MulDiv(g_savedRect.bottom - g_savedRect.top, (int)dpiNow, (int)g_savedDpi);
        RECT r;
        if (MonitorFromRect(&g_savedRect, MONITOR_DEFAULTTONEAREST) == mon.handle) {
            r = { g_savedRect.left, g_savedRect.top, g_savedRect.left + rw, g_savedRect.top + rh };
            // keep the (possibly larger) rect inside the work area
            if (r.right  > mon.workRect.right)  OffsetRect(&r, mon.workRect.right  - r.right, 0);
            if (r.bottom > mon.workRect.bottom) OffsetRect(&r, 0, mon.workRect.bottom - r.bottom);
            if (r.left < mon.workRect.left) OffsetRect(&r, mon.workRect.left - r.left, 0);
            if (r.top  < mon.workRect.top)  OffsetRect(&r, 0, mon.workRect.top - r.top);
        } else {
            r = monsel::CenterInWorkArea(rw, rh, mon);
        }
        // Clear the flag first: the move below may raise WM_DPICHANGED, which
        // must take the windowed path, not re-fit fullscreen.
        g_fullscreen = false;
        SetWindowLongPtr(hWnd, GWL_STYLE, g_savedStyle);
        SetMenu(hWnd, g_menu);
        SetWindowPos(hWnd, HWND_NOTOPMOST,
                     r.left, r.top, r.right - r.left, r.bottom - r.top,
                     SWP_FRAMECHANGED | SWP_SHOWWINDOW);

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
    int  monitor    = -1;     // -monitor N: 1 = primary, 2.. by position (-1 = unset)
    std::string monitorId;    // -monitorid <id>: stable device id (wins over the number)
    bool monitorIdSet = false;
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
//   -rom <file>  -fullscreen  -window  -scale <1|2|3|fit>  -monitor <n>  -monitorid <id>  -h
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
        else if (a == "-monitor" && i + 1 < argc) {
            g_cmd.monitor = atoi(argv[++i]);
            if (g_cmd.monitor <= 0) g_cmd.monitor = 1;   // 0/negative/garbage -> primary
            g_cmd.monitorIdSet = false;                  // an explicit number clears the id
            g_cmd.monitorId.clear();
        }
        else if (a == "-monitorid" && i + 1 < argc) {
            g_cmd.monitorId = argv[++i];                 // keep original case (ids are matched case-insensitively)
            g_cmd.monitorIdSet = true;
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
        L"  -monitor <n>      Start on monitor n (1 = primary)\n"
        L"  -monitorid <id>   Start on the monitor with this device id (see the log)\n"
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
        case IDM_TAPE_PLAY: case IDM_TAPE_STOP: case IDM_TAPE_REWIND: case IDM_TAPE_EJECT:
            if (g_app.tape_command) g_app.tape_command(LOWORD(wParam) - IDM_TAPE_PLAY);
            return 0;
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
        case IDM_BASIC1:
        case IDM_BASIC2:
        case IDM_BASIC4:
        case IDM_BASIC4032:
        case IDM_BASIC8032: {
            int want = (LOWORD(wParam) == IDM_BASIC1) ? 1 :
                       (LOWORD(wParam) == IDM_BASIC4032) ? 12 :
                       (LOWORD(wParam) == IDM_BASIC8032) ? 8 :
                       (LOWORD(wParam) == IDM_BASIC4) ? 4 : 2;
            if (want != g_basic && g_app.set_basic) {
                if (!g_app.set_basic(want)) {
                    MessageBoxA(wnd, "Unable to load model ROMs. Run download-roms.ps1 to install them.",
                                "PET ROMs missing", MB_OK | MB_ICONERROR);
                    return 0;
                }
                g_basic = want;
                if (g_basic == 8) g_ram = 32;
                if (g_basic == 1) g_ram = 8;
                set_config_int("machine", "basic", g_basic);
                HostUpdateBasicChecks();
                HostUpdateRamChecks();
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
        case IDM_KEYBOARD:
            if (g_app.show_keyboard) g_app.show_keyboard(wnd);
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
        HostUpdateScaleEnables((HMENU)wParam);
        HostUpdateScaleChecks();
        if (g_app.tape_state) {
            const int state = g_app.tape_state();
            const HMENU menu = (HMENU)wParam;
            EnableMenuItem(menu, IDM_TAPE_PLAY, MF_BYCOMMAND | ((state & 1) && !(state & 6) ? MF_ENABLED : MF_GRAYED));
            EnableMenuItem(menu, IDM_TAPE_STOP, MF_BYCOMMAND | (state & 2 ? MF_ENABLED : MF_GRAYED));
            EnableMenuItem(menu, IDM_TAPE_REWIND, MF_BYCOMMAND | (state & 1 ? MF_ENABLED : MF_GRAYED));
            EnableMenuItem(menu, IDM_TAPE_EJECT, MF_BYCOMMAND | (state & 1 ? MF_ENABLED : MF_GRAYED));
            CheckMenuItem(menu, IDM_TAPE_PLAY, MF_BYCOMMAND | (state & 2 ? MF_CHECKED : MF_UNCHECKED));
        }
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

    case WM_GETDPISCALEDSIZE: {
        // Scale preset: tell Windows the outer size of the chosen preset for the
        // new DPI so the drag preview matches. The destination monitor is not
        // known here, so this ignores whether it fits; WM_DPICHANGED then steps
        // down to the effective preset for the monitor the window landed on.
        if (g_fullscreen || g_scale < 1) return FALSE; // default scaling
        SIZE* sz = (SIZE*)lParam;
        int ww, wh;
        HostPresetWindowSize(g_scale > 3 ? 3 : g_scale, (DWORD)GetWindowLongPtr(wnd, GWL_STYLE),
                             (DWORD)GetWindowLongPtr(wnd, GWL_EXSTYLE), (UINT)wParam, &ww, &wh);
        sz->cx = ww;
        sz->cy = wh;
        return TRUE; }

    case WM_DPICHANGED: {
        // Window moved to a monitor with different scaling (or the scale changed).
        const UINT newDpi = LOWORD(wParam);
        const RECT* sug = (const RECT*)lParam;   // Windows' suggested rect for the new DPI
        LOG_INFO("WM_DPICHANGED: dpi=%u suggested (%ld,%ld)-(%ld,%ld)", newDpi,
                 sug->left, sug->top, sug->right, sug->bottom);
        if (g_fullscreen) {
            const RECT r = monsel::FromWindow(wnd).monitorRect;
            SetWindowPos(wnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        } else {
            RECT r = *sug;
            const DWORD st = (DWORD)GetWindowLongPtr(wnd, GWL_STYLE);
            const DWORD ex = (DWORD)GetWindowLongPtr(wnd, GWL_EXSTYLE);
            MONITORINFO mi{ sizeof(mi) };
            if (GetMonitorInfo(MonitorFromRect(sug, MONITOR_DEFAULTTONEAREST), &mi)) {
                const RECT& wa = mi.rcWork;
                const int eff = (g_scale >= 1) ? HostEffectiveScale(wa, st, ex, newDpi) : 0;
                if (g_scale >= 1) HostLogScaleFallback(eff);
                int ww = sug->right - sug->left, wh = sug->bottom - sug->top;
                bool place = false;
                if (eff >= 1) {
                    // Preset: exact size for the new DPI and monitor.
                    HostPresetWindowSize(eff, st, ex, newDpi, &ww, &wh);
                    place = true;
                } else if (ww > wa.right - wa.left || wh > wa.bottom - wa.top) {
                    // Fit mode (or no preset fits) and Windows' rect is too big
                    // for this monitor: shrink to the Fit rect.
                    r = HostFitWindowRect(wa, st, ex, newDpi);
                } else {
                    place = true; // Fit mode: keep the suggested size
                }
                if (place) {
                    // Suggested top-left, clamped into the work area.
                    int nx = sug->left, ny = sug->top;
                    if (nx + ww > wa.right)  nx = wa.right - ww;
                    if (ny + wh > wa.bottom) ny = wa.bottom - wh;
                    if (nx < wa.left) nx = wa.left;
                    if (ny < wa.top)  ny = wa.top;
                    r = { nx, ny, nx + ww, ny + wh };
                }
            }
            SetWindowPos(wnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        HostSetWindowIcons(wnd, (HINSTANCE)GetWindowLongPtr(wnd, GWLP_HINSTANCE), newDpi);
        HostUpdateViewport();
        HostUpdateScaleChecks();
        return 0; }

    case WM_DISPLAYCHANGE: {
        // Monitors were added/removed/re-arranged or a mode changed. Keep the
        // window on screen.
        RECT cur{};
        if (!GetWindowRect(wnd, &cur)) return 0;

        if (g_fullscreen) {
            const RECT r = monsel::FromWindow(wnd).monitorRect;
            if (r.left != cur.left || r.top != cur.top || r.right != cur.right || r.bottom != cur.bottom) {
                LOG_INFO("WM_DISPLAYCHANGE: re-fitting fullscreen to (%ld,%ld)-(%ld,%ld)",
                         r.left, r.top, r.right, r.bottom);
                SetWindowPos(wnd, NULL, r.left, r.top, r.right - r.left, r.bottom - r.top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                HostUpdateViewport();
            }
            // The windowed rect we restore to may now be off-screen.
            if (!monsel::IsRectVisible(g_savedRect)) {
                const RECT s = monsel::CenterInWorkArea(g_savedRect.right - g_savedRect.left,
                                                        g_savedRect.bottom - g_savedRect.top,
                                                        monsel::Primary());
                LOG_INFO("WM_DISPLAYCHANGE: saved windowed rect off-screen, recentered on primary at (%ld,%ld)",
                         s.left, s.top);
                g_savedRect = s;
            }
        } else if (!monsel::IsRectVisible(cur)) {
            const RECT r = monsel::CenterInWorkArea(cur.right - cur.left, cur.bottom - cur.top, monsel::Primary());
            LOG_INFO("WM_DISPLAYCHANGE: window no longer visible, moving to primary monitor at (%ld,%ld)",
                     r.left, r.top);
            SetWindowPos(wnd, NULL, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

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

    const DWORD style = WS_OVERLAPPEDWINDOW;

    // Pick the starting monitor: command line wins over ini. Numbering and the
    // stable device id come from the shared monitor_select module (see its README).
    int startMonitor = (g_cmd.monitor > 0) ? g_cmd.monitor : get_config_int("video", "starting_monitor", 1);
    if (startMonitor <= 0) startMonitor = 1;
    std::string startMonitorId;
    if (g_cmd.monitor > 0)            startMonitorId.clear();   // explicit number beats the ini id
    else if (g_cmd.monitorIdSet)      startMonitorId = g_cmd.monitorId;
    else                              startMonitorId = get_config_string(std::string("video"), std::string("starting_monitor_id"), std::string());

    // Log every monitor once so the user can copy a device id into the ini.
    {
        std::string list = monsel::DescribeAll();
        size_t pos = 0;
        while (pos < list.size()) {
            size_t nl = list.find('\n', pos);
            if (nl == std::string::npos) nl = list.size();
            LOG_INFO("Monitor list: %s", list.substr(pos, nl - pos).c_str());
            pos = nl + 1;
        }
    }
    std::string monReason;
    const monsel::MonitorDesc startMon = monsel::Select(startMonitor, startMonitorId, &monReason);
    LOG_INFO("Starting monitor %d selected: (%ld,%ld)-(%ld,%ld) name=\"%s\" id=%s (%s)",
             startMon.number,
             startMon.monitorRect.left, startMon.monitorRect.top,
             startMon.monitorRect.right, startMon.monitorRect.bottom,
             startMon.friendlyName.c_str(), startMon.deviceId.c_str(), monReason.c_str());

    // Initial size: the effective preset (largest <= the chosen scale that fits)
    // for the start monitor, or its Fit rect, in physical pixels at that
    // monitor's DPI (the window does not exist yet, so ask the monitor);
    // HostApplyScale refines it.
    g_scale = (g_cmd.scale >= 0) ? g_cmd.scale : get_config_int("video", "scale", 2);
    if (g_scale < 0 || g_scale > 3) g_scale = 2;
    const UINT startDpi = HostMonitorDpi(startMon.handle);
    const int startEff = HostEffectiveScale(startMon.workRect, style, 0, startDpi);
    int ww, wh;
    RECT startRect;
    if (startEff >= 1) {
        HostPresetWindowSize(startEff, style, 0, startDpi, &ww, &wh);
        startRect = monsel::CenterInWorkArea(ww, wh, startMon);
    } else {
        startRect = HostFitWindowRect(startMon.workRect, style, 0, startDpi);
        ww = startRect.right - startRect.left;
        wh = startRect.bottom - startRect.top;
    }
    LOG_INFO("Start monitor DPI %u, scale %d (effective %s%d), initial window %dx%d", startDpi, g_scale,
             startEff >= 1 ? "" : "Fit/", startEff, ww, wh);
    HostLogScaleFallback(startEff);
    const int px = startRect.left;
    const int py = startRect.top;

    hWnd = CreateWindowW(L"EmulatorHost", app->title, style,
                         px, py, ww, wh, NULL, g_menu, hInstance, NULL);
    if (!hWnd) {
        MessageBoxW(NULL, L"Window creation failed", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // Set the title-bar (small) and Alt-Tab/taskbar (big) icons explicitly so each
    // is rasterized from the best-matching size in the .ico rather than scaled.
    HostSetWindowIcons(hWnd, hInstance, HostWindowDpi(hWnd));

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
    if (g_app.get_basic) g_basic = g_app.get_basic();
    if (g_basic != 1 && g_basic != 2 && g_basic != 4 && g_basic != 8 && g_basic != 12) g_basic = 2;
    HostUpdateBasicChecks();

    // Restore the saved RAM size and tick the menu (emu_init already applied it).
    g_ram = get_config_int("machine", "ram", 32);
    if (g_ram != 4 && g_ram != 8 && g_ram != 16 && g_ram != 32) g_ram = 32;
    if (g_basic == 8) g_ram = 32;
                if (g_basic == 1) g_ram = 8;
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
                    RawInput_ReleaseKey(KEY_ESC); // consume so it doesn't re-trigger
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
