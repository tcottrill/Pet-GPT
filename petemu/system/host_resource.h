#pragma once
// Reuse the existing app icon defined in petemu.rc.
#include "Resource.h"            // brings in the existing app icon symbol
#define IDI_APPICON       IDI_PETEMU   // IDI_PETEMU = 107

#define IDR_HOST_MENU     101     // free (102 is taken by IDD_PETEMU_DIALOG)
#define IDR_HOST_ACCEL    129     // free (102 taken, next free above IDR_MAINFRAME=128)

#define IDM_LOADROM       40001   // Load Program/Disk...
#define IDM_RESET         40002
#define IDM_EXIT          40003
#define IDM_FULLSCREEN    40004
#define IDM_SCALE_1X      40005
#define IDM_SCALE_2X      40006
#define IDM_SCALE_3X      40007
#define IDM_SCALE_FIT     40008
#define IDM_ABOUT         40009
#define IDM_EJECT         40010   // File > Eject Disk (unmount .d64/.d71 -> ./files vdrive)
#define IDM_BASIC2        40030   // Machine > BASIC 2 (radio)
#define IDM_BASIC4        40031   // Machine > BASIC 4 (radio)
#define IDM_BASIC8032     40032   // Machine > 8032 80-column (radio, contiguous with BASIC2/4)
#define IDM_RAM4          40050   // Machine > Memory > 4K  (radio)
#define IDM_RAM8          40051   // Machine > Memory > 8K  (radio)
#define IDM_RAM16         40052   // Machine > Memory > 16K (radio)
#define IDM_RAM32         40053   // Machine > Memory > 32K (radio)
#define IDM_CRT           40040   // View > CRT (mono monitor shader) toggle
#define IDM_MONITOR_GREEN 40041   // View > Monitor > Green phosphor (radio)
#define IDM_MONITOR_BW    40042   // View > Monitor > Black & white  (radio)
#define IDM_SPEED2X       40060   // Machine > 2x Speed (checkbox)
#define IDM_KBDGFX        40061   // Machine > Graphics Keyboard (checkbox, also F12)
#define IDM_SNES          40062   // Machine > SNES Adapter (checkbox; [input] snes_adapter)

// View > CRT Monitor: shader knob controls. Value items are grayed labels
// refreshed on WM_INITMENUPOPUP; Up/Dn adjust live and save to pet.ini.
// Knob index order matches pet_gl.cpp k_knobs: 0 blur_h, 1 blur_v,
// 2 halation, 3 halation_radius, 4 scanline, 5 contrast, 6 brightness.
#define IDM_CRT_SETTINGS  40044   // View > CRT Monitor Settings... (modeless dialog)
#define IDD_CRTSETTINGS   200     // the settings dialog template
// Dialog controls: 7 knob rows (slider + edit + spin), indexed 0..6 in the
// same order as pet_gl.cpp k_knobs.
#define IDC_KNOB_SLIDER0  2001    // ..2007
#define IDC_KNOB_EDIT0    2011    // ..2017
#define IDC_KNOB_SPIN0    2021    // ..2027
#define IDC_CRT_ENABLE    2030
#define IDC_MON_GREEN     2031
#define IDC_MON_BW        2032
#define IDC_CRT_DEFAULTS  2033
#define IDM_CRT_DEFAULTS  40070   // (menu id retired; dialog uses IDC_CRT_DEFAULTS)
#define IDM_KNOBVAL0      40071   // ..40077 (7 value-display items)
#define IDM_KNOBUP0       40081   // ..40087
#define IDM_KNOBDN0       40091   // ..40097
#define IDM_KNOBVAL1 40072
#define IDM_KNOBVAL2 40073
#define IDM_KNOBVAL3 40074
#define IDM_KNOBVAL4 40075
#define IDM_KNOBVAL5 40076
#define IDM_KNOBVAL6 40077
#define IDM_KNOBUP1 40082
#define IDM_KNOBUP2 40083
#define IDM_KNOBUP3 40084
#define IDM_KNOBUP4 40085
#define IDM_KNOBUP5 40086
#define IDM_KNOBUP6 40087
#define IDM_KNOBDN1 40092
#define IDM_KNOBDN2 40093
#define IDM_KNOBDN3 40094
#define IDM_KNOBDN4 40095
#define IDM_KNOBDN5 40096
#define IDM_KNOBDN6 40097

#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif
