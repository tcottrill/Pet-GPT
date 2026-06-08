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
#define IDM_EJECT         40010   // File > Eject Disk (unmount .d64 -> ./files vdrive)
#define IDM_BASIC2        40030   // Machine > BASIC 2 (radio)
#define IDM_BASIC4        40031   // Machine > BASIC 4 (radio)
#define IDM_RAM4          40050   // Machine > Memory > 4K  (radio)
#define IDM_RAM8          40051   // Machine > Memory > 8K  (radio)
#define IDM_RAM16         40052   // Machine > Memory > 16K (radio)
#define IDM_RAM32         40053   // Machine > Memory > 32K (radio)
#define IDM_CRT           40040   // View > CRT (scanlines + tint) toggle

#ifndef IDC_STATIC
#define IDC_STATIC (-1)
#endif
