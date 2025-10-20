// Includes

#include <windows.h>
#include "Resource.h"
#include "sys_gl.h"
#include "sys_log.h"
#include "rawinput.h"
#include "emulator.h"


//Globals
HWND hWnd;

int SCREEN_W = 672;
int SCREEN_H = 480;

// Function Declarations
LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

int KeyCheck(int keynum)
{
	int i;
	static int hasrun = 0;
	static int keys[256];
	//Init
	if (hasrun == 0) { for (i = 0; i < 256; i++) { keys[i] = 0; }	hasrun = 1; }

	if (!keys[keynum] && key[keynum]) //Return True if not in que
	{
		keys[keynum] = 1;	return 1;
	}
	else if (keys[keynum] && !key[keynum]) //Return False if in que
		keys[keynum] = 0;
	return 0;
}

//========================================================================
// Set the window title
//========================================================================
void set_window_title(const char* title)
{
	(void)SetWindowTextA(hWnd, title);
}

//========================================================================
// Return a std string with last error message
//========================================================================
std::string GetLastErrorStdStr()
{
	DWORD error = GetLastError();
	if (error)
	{
		LPVOID lpMsgBuf;
		DWORD bufLen = FormatMessage(
			FORMAT_MESSAGE_ALLOCATE_BUFFER |
			FORMAT_MESSAGE_FROM_SYSTEM |
			FORMAT_MESSAGE_IGNORE_INSERTS,
			NULL,
			error,
			MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
			(LPTSTR)&lpMsgBuf,
			0, NULL);
		if (bufLen)
		{
			LPCSTR lpMsgStr = (LPCSTR)lpMsgBuf;
			std::string result(lpMsgStr, lpMsgStr + bufLen);

			LocalFree(lpMsgBuf);

			return result;
		}
	}
	return std::string();
}

//========================================================================
// Popup a Windows Error Message, Allegro Style
//========================================================================
void allegro_message(const char* title, const char* message)
{
	MessageBoxA(NULL, message, title, MB_ICONEXCLAMATION | MB_OK);
}

//========================================================================
// Return the Window Handle
//========================================================================
HWND win_get_window()
{
	return hWnd;
}

// WinMain
int WINAPI WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE hPrevInstance, _In_ LPSTR lpCmdLine, _In_ int nShowCmd)
//int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int iCmdShow)
{
	WNDCLASS wc{};

	MSG msg;
	//HGLRC hRC;
	bool Quit = FALSE;
	DWORD       dwExStyle;                      // Window Extended Style
	DWORD       dwStyle;                        // Window Style

	// register window class
	wc.style = CS_OWNDC;
	wc.lpfnWndProc = WndProc;
	wc.cbClsExtra = 0;
	wc.cbWndExtra = 0;
	wc.hInstance = hInstance;
	wc.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_PETEMU));
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
	wc.lpszMenuName = NULL;
	wc.lpszClassName = L"EMULATOR";

	if (!RegisterClass(&wc)) {
		MessageBox(NULL, L"Window Registration Failed!", L"Error", MB_OK | MB_ICONERROR);
		return 1;
	}

	dwExStyle = WS_EX_APPWINDOW;   // Window Extended Style
	dwStyle = WS_OVERLAPPEDWINDOW | WS_THICKFRAME;                    // Windows Style
	RECT WindowRect;                                                 // Grabs Rectangle Upper Left / Lower Right Values
	WindowRect.left = (long)0;                                         // Set Left Value To 0
	WindowRect.right = (long)SCREEN_W;                                 // Set Right Value To Requested Width
	WindowRect.top = (long)0;                                          // Set Top Value To 0
	WindowRect.bottom = (long)SCREEN_H;                               // Set Bottom Value To Requested Height
	AdjustWindowRectEx(&WindowRect, dwStyle, FALSE, dwExStyle);      // Adjust Window To True Requested Size

	// Create The Window
	if (!(hWnd = CreateWindowEx(dwExStyle,							// Extended Style For The Window
		L"EMULATOR",						// Class Name
		L"Commodore Pet Emulator",			// Window Title
		dwStyle |							// Defined Window Style
		WS_CLIPSIBLINGS |					// Required Window Style
		WS_CLIPCHILDREN,					// Required Window Style
		CW_USEDEFAULT, 0,   				// Window Position
		WindowRect.right - WindowRect.left,	// Calculate Window Width
		WindowRect.bottom - WindowRect.top,	// Calculate Window Height
		NULL,								// No Parent Window
		NULL,								// No Menu
		hInstance,							// Instance
		NULL)))								// Dont Pass Anything To WM_CREATE
	{
		// Reset The Display
		MessageBox(NULL, L"Window Creation Error.", L"ERROR", MB_OK | MB_ICONEXCLAMATION);
		return FALSE;
	}

	ShowWindow(hWnd, SW_SHOW);
	UpdateWindow(hWnd);
	SetForegroundWindow(hWnd);
	SetFocus(hWnd);
	///////////////// Initialize everything here //////////////////////////////
	//set_icon("SpaceInvadersExample.ico");
	LogOpen("emulatorlog.txt");
	LOG_INFO("Opening Log");

	// enable OpenGL for the window
	OpenGL3Enable();
	// Enable vSync
	osWaitVsync(1);

	HRESULT i = RawInput_Initialize(hWnd);

	ViewOrtho(SCREEN_W, SCREEN_H);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	emu_init(__argc, __argv);

	/////////////////// END INITIALIZATION ////////////////////////////////////

	// program main loop
	while (!Quit)
	{
		// check for messages
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE))
		{
			// handle or dispatch messages
			if (msg.message == WM_QUIT)
			{
				Quit = TRUE;
			}
			else
			{
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}
		}
		else
		{
			emu_update();
		}
	}

	//End Program
	emu_end();
	// shutdown OpenGL
	OpenGLShutDown();

	//Shutdown logging
	LOG_INFO("Closing Log");
	LogClose();

	// destroy the window explicitly
	DestroyWindow(hWnd);
	return (int)msg.wParam;
}

// Window Procedure

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_CREATE:
		return 0;

	case WM_CLOSE:
		PostQuitMessage(0);
		return 0;

	case WM_INPUT: { return RawInput_ProcessInput(hWnd, wParam, lParam); return 0; }

	case WM_DESTROY:
		return 0;

	case WM_SETCURSOR:
		if (LOWORD(lParam) == HTCLIENT)
		{
			SetCursor(NULL);

			return TRUE;
		}
		return 0;

	case WM_SYSCOMMAND:
	{
		switch (wParam & 0xfff0)
		{
		case SC_SCREENSAVE:
		case SC_MONITORPOWER:
		{
			return 0;
		}
		/*
		case SC_CLOSE:
		{
			//I can add a close hook here to trap close button
			quit = 1;
			PostQuitMessage(0);
			break;
		}
		*/
		// User trying to access application menu using ALT?
		case SC_KEYMENU:
			return 0;
		}
		DefWindowProc(hWnd, message, wParam, lParam);
	}

	case WM_KEYDOWN:
		switch (wParam)
		{
		case VK_ESCAPE:
			PostQuitMessage(0);
			return 0;
		}
		return 0;

	default:
		return DefWindowProc(hWnd, message, wParam, lParam);
	}
}