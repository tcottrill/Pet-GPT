
#pragma once
// Add these keycode definitions if not already present
#ifndef KEY_DELETE
#define KEY_DELETE 0x2E // Virtual-Key code for Delete
#endif
#include <Windows.h>
#include "glew.h"
#include "wglew.h"
#include "sys_log.h"

extern void allegro_message(const char *title, const char *message);
extern HWND win_get_window();
extern int SCREEN_W;
extern int SCREEN_H;

