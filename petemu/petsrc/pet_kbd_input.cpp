// -----------------------------------------------------------------------------
// PET 2001 / 2001N Keyboard Matrix Input (Windows, RawInput-friendly)
// Single-file implementation.
//
// Host mapping (this build):
//   - RUN/STOP  = CapsLock (VK_CAPITAL)
//   - BREAK     = RUN/STOP + Shift   (CapsLock + L/R Shift)
//   - CLR/HOME  = VK_HOME            (Shift+Home also asserts Shift key in matrix)
//   - DEL       = VK_DELETE or VK_BACK
//   - Cursor UP  = VK_DOWN
//   - Cursor DN  = VK_UP (synthetic: press CursorDN + Shift so PET sees "Cursor Up")
//
// Features:
//   - Uses global 'key[256]' (nonzero => VK down).
//   - Builds 10x8 active-low matrix: rows[10] (bit N cleared => column N pressed).
//   - Locale-aware typing via ToUnicodeEx (VK -> ASCII) then ASCII -> PET (row,col).
//   - Mirrors L/R modifiers into aggregate VKs (SHIFT/CONTROL/MENU) for ToUnicodeEx.
//   - Explicit handling for OEM punctuation (', ", =, +, ,, <, ., >, -, _, /, ?).
//   - Reflects host L/R Shift as PET Shift matrix keys while held (helps games).
//   - Graphics mode toggle: F12 switches between "graphics" (default) and "business".
//
// Public API in this file (declared in pet_kbd_input.h):
//   void set_pet_graphics_mode(bool) noexcept;
//   bool get_pet_graphics_mode() noexcept;
//   void toggle_pet_graphics_mode() noexcept;
//   void build_pet_rows_from_vk(uint8_t out[10]);        // fills + pushes, handles F12

// -----------------------------------------------------------------------------

// Original code
// Copyright (c) 2012,2014 Thomas Skibo.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.


#include "pet_kbd_input.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <array>

#include "rawinput.h"
#include "pet_machine.h"
#include "pet2001io.h"      // for io.setKeyrows(...)
#include "pet_mem.h"

enum class KbdMode : unsigned char { Symbolic = 0, Positional = 1 };
static KbdMode g_kbd_mode = KbdMode::Symbolic;  // default: your current behavior

static unsigned char make_vk_to_pet_pos(unsigned char(&m)[256]) {
	for (auto& x : m) x = 0xFF;

	// Letters
	auto S = [&](int vk, int row, int col) { m[vk] = static_cast<unsigned char>((row << 4) | col); };
	S('A', 4, 0); S('B', 6, 2); S('C', 6, 1); S('D', 4, 1); S('E', 2, 1); S('F', 5, 1);
	S('G', 4, 2); S('H', 5, 2); S('I', 3, 3); S('J', 4, 3); S('K', 5, 3); S('L', 4, 4);
	S('M', 6, 3); S('N', 7, 2); S('O', 2, 4); S('P', 3, 4); S('Q', 2, 0); S('R', 3, 1);
	S('S', 5, 0); S('T', 2, 2); S('U', 2, 3); S('V', 7, 1); S('W', 3, 0); S('X', 7, 0);
	S('Y', 3, 2); S('Z', 6, 0);

	// Digits
	S('0', 8, 6); S('1', 6, 6); S('2', 7, 6); S('3', 6, 7); S('4', 4, 6);
	S('5', 5, 6); S('6', 4, 7); S('7', 2, 6); S('8', 3, 6); S('9', 2, 7);

	// Also accept VK_* letter/digit constants (for non-ASCII VKs)
	S(KEY_A, 4, 0); S(VK_KEY_B, 6, 2); S(VK_KEY_C, 6, 1); S(VK_KEY_D, 4, 1);
	S(VK_KEY_E, 2, 1); S(VK_KEY_F, 5, 1); S(VK_KEY_G, 4, 2); S(VK_KEY_H, 5, 2);
	S(VK_KEY_I, 3, 3); S(VK_KEY_J, 4, 3); S(VK_KEY_K, 5, 3); S(VK_KEY_L, 4, 4);
	S(VK_KEY_M, 6, 3); S(VK_KEY_N, 7, 2); S(VK_KEY_O, 2, 4); S(VK_KEY_P, 3, 4);
	S(VK_KEY_Q, 2, 0); S(VK_KEY_R, 3, 1); S(VK_KEY_S, 5, 0); S(VK_KEY_T, 2, 2);
	S(VK_KEY_U, 2, 3); S(VK_KEY_V, 7, 1); S(VK_KEY_W, 3, 0); S(VK_KEY_X, 7, 0);
	S(VK_KEY_Y, 3, 2); S(VK_KEY_Z, 6, 0);

	S(VK_KEY_0, 8, 6); S(VK_KEY_1, 6, 6); S(VK_KEY_2, 7, 6); S(VK_KEY_3, 6, 7); S(VK_KEY_4, 4, 6);
	S(VK_KEY_5, 5, 6); S(VK_KEY_6, 4, 7); S(VK_KEY_7, 2, 6); S(VK_KEY_8, 3, 6); S(VK_KEY_9, 2, 7);

	return 1;
}

// Static-init the table once
static unsigned char g_vk_to_pet[256];
static unsigned char g_vk_to_pet_init = make_vk_to_pet_pos(g_vk_to_pet);

// Helper to tell if VK is a letter (used for graphics mode)
static inline bool vk_is_letter(int vk) {
	return (vk >= 'A' && vk <= 'Z') || (vk >= 'a' && vk <= 'z') ||
		(vk >= VK_KEY_A && vk <= VK_KEY_Z);
}

// -----------------------------------------------------------------------------
// Internal ASCII -> (row,col) map (PET matrix positions).
// Rows: 0..9, Cols: 0..7
// Active-low: out[row] bit 'col' cleared means key pressed.
// -----------------------------------------------------------------------------
struct KeyPos { int row; int col; };

static std::array<KeyPos, 128> make_ascii_to_pet_pos() {
	std::array<KeyPos, 128> map;
	for (auto& k : map) k = { -1, -1 };

	// Letters (uppercase)
	map['A'] = { 4,0 }; map['B'] = { 6,2 }; map['C'] = { 6,1 }; map['D'] = { 4,1 };
	map['E'] = { 2,1 }; map['F'] = { 5,1 }; map['G'] = { 4,2 }; map['H'] = { 5,2 };
	map['I'] = { 3,3 }; map['J'] = { 4,3 }; map['K'] = { 5,3 }; map['L'] = { 4,4 };
	map['M'] = { 6,3 }; map['N'] = { 7,2 }; map['O'] = { 2,4 }; map['P'] = { 3,4 };
	map['Q'] = { 2,0 }; map['R'] = { 3,1 }; map['S'] = { 5,0 }; map['T'] = { 2,2 };
	map['U'] = { 2,3 }; map['V'] = { 7,1 }; map['W'] = { 3,0 }; map['X'] = { 7,0 };
	map['Y'] = { 3,2 }; map['Z'] = { 6,0 };

	// Digits
	map['0'] = { 8,6 }; map['1'] = { 6,6 }; map['2'] = { 7,6 }; map['3'] = { 6,7 };
	map['4'] = { 4,6 }; map['5'] = { 5,6 }; map['6'] = { 4,7 }; map['7'] = { 2,6 };
	map['8'] = { 3,6 }; map['9'] = { 2,7 };

	// Whitespace / return
	map[' '] = { 9,2 };
	map['\r'] = { 6,5 };
	map['\n'] = { 6,5 };

	// Punctuation / symbols
	map['!'] = { 0,0 };
	map['"'] = { 1,0 };
	map['#'] = { 0,1 };
	map['$'] = { 1,1 };
	map['%'] = { 0,2 };
	map['&'] = { 0,3 };
	map['('] = { 0,4 };
	map[')'] = { 1,4 };
	map['*'] = { 5,7 };
	map['+'] = { 7,7 };
	map[','] = { 7,3 };
	map['-'] = { 8,7 };
	map['.'] = { 9,6 };
	map['/'] = { 3,7 };
	map[':'] = { 5,4 };
	map[';'] = { 6,4 };
	map['<'] = { 9,3 };
	map['='] = { 9,7 };
	map['>'] = { 8,4 };
	map['?'] = { 7,4 };
	map['@'] = { 8,1 };
	map['['] = { 9,1 };
	map['\\'] = { 1,3 };
	map[']'] = { 8,2 };
	map['^'] = { 2,5 };
	//map['_'] = { 0,5 };
	map['\''] = { 1,2 }; // apostrophe

	return map;
}

static const std::array<KeyPos, 128> ascii_to_pet_pos = make_ascii_to_pet_pos();

static inline bool pet_pos_for_ascii(unsigned char ch, int& row, int& col) {
	const KeyPos kp = ascii_to_pet_pos[ch];
	row = kp.row; col = kp.col;
	return (row >= 0 && col >= 0);
}

// Clear bit (active-low press)
static inline void pet_press(std::uint8_t out[10], int row, int col) {
	out[row] &= static_cast<std::uint8_t>(~(1u << col));
}

// -----------------------------------------------------------------------------
// VK specials mapped directly to PET matrix positions.
// Returns true if this VK was a handled special and pressed into 'out'.
// Includes OEM punctuation fallbacks and Cursor Up.
// -----------------------------------------------------------------------------
// VK specials mapped directly to PET matrix positions.
// Returns true if this VK was a handled special and pressed into 'out'.
// Includes OEM punctuation fallbacks and Cursor Up.
static inline bool press_pet_special_from_vk(std::uint8_t out[10], int vk, const BYTE kbState[256]) {
	int row = -1, col = -1;

	// Compute host Shift once, BEFORE choosing rows/cols.
	const bool sh = ((kbState[VK_SHIFT] | kbState[VK_LSHIFT] | kbState[VK_RSHIFT]) & 0x80) != 0;

	switch (vk) {
	case VK_SPACE:     row = 9; col = 2; break; // Space
	case VK_RETURN:    row = 6; col = 5; break; // Return
	case VK_SEPARATOR: row = 6; col = 5; break; // Numpad Enter (RawInput remaps E0 Return here)
	case VK_CAPITAL: row = 9; col = 4; break; // RUN/STOP (CapsLock)
	//case VK_HOME:    row = 9; col = 0; break; // CLR/HOME
	case VK_HOME: row = 0; col = 6; break; // CLR/HOME (graphics keyboard)
	case VK_DELETE:
	case VK_BACK:    row = 1; col = 7; break; // DELETE (^T)
	case VK_RIGHT:  row = 0; col = 7; break; // Cursor Right
	case VK_LEFT:   row = 0; col = 5; break; // Cursor Left
	case VK_DOWN:    row = 1; col = 6; break; // Cursor Down (^Q)

		// Cursor Up => press Cursor key; we'll add PET Shift after pressing
	case VK_UP:      row = 1; col = 6; break;

		// Quotes/apostrophe - choose PET key by host Shift, DO NOT press PET Shift here
	case VK_OEM_7:
		if (sh) { row = 1; col = 0; }  // '"'
		else { row = 1; col = 2; }  // '\''
		break;

		// '=' / '+'
	case VK_OEM_PLUS:
		if (sh) { row = 7; col = 7; }  // '+'
		else { row = 9; col = 7; }  // '='
		break;

		// ',' / '<'
	case VK_OEM_COMMA:
		if (sh) { row = 9; col = 3; }  // '<'
		else { row = 7; col = 3; }  // ','
		break;

		// '.' / '>'
	case VK_OEM_PERIOD:
		if (sh) { row = 8; col = 4; }  // '>'
		else { row = 9; col = 6; }  // '.'
		break;

		// '-' / '_'
	case VK_OEM_MINUS:
		//if (sh) { row = 0; col = 5; }  // '_' REMOVED : PET '_'
		//else 
        { row = 8; col = 7; }  // '-'
		break;

		// '/' / '?'
	case VK_OEM_2:
		if (sh) { row = 7; col = 4; }  // '?'
		else { row = 3; col = 7; }  // '/'
		break;

	default:
		break;
	}

	if (row >= 0) {
		pet_press(out, row, col);

		// For Cursor Up synth, assert a PET Shift key in addition to the cursor key.
		if (vk == VK_UP) {
			pet_press(out, 8, 0); // Left Shift (8,0) - or use (8,5) for Right Shift.
		}
		return true;
	}
	return false;
}

// VK -> ASCII via ToUnicodeEx (layout-aware)
static bool vk_to_ascii(unsigned vk, const BYTE kbState[256], HKL layout, unsigned char& outCh)
{
	UINT sc = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout);
	if (!sc) return false;

	wchar_t wbuf[8] = { 0 };
	int rc = ToUnicodeEx(vk, sc, kbState, wbuf, 8, 0, layout);
	if (rc < 0) {
		(void)ToUnicodeEx(vk, sc, kbState, wbuf, 8, 0, layout); // clear dead key state
		return false;
	}
	if (rc == 0) return false;

	wchar_t wc = wbuf[0];
	if (wc == L'\n') wc = L'\r';

	if (wc >= 0 && wc < 128) { outCh = static_cast<unsigned char>(wc); return true; }
	return false;
}

// -----------------------------------------------------------------------------
// Graphics mode toggle: true => Shift+letter asserts PET Shift (graphics chars)
// Business mode       : false => letters do not assert PET Shift
// -----------------------------------------------------------------------------
static bool g_pet_graphics_shift_mode = true;

void set_pet_graphics_mode(bool enable) noexcept { g_pet_graphics_shift_mode = enable; }
bool get_pet_graphics_mode() noexcept { return g_pet_graphics_shift_mode; }
void toggle_pet_graphics_mode() noexcept { g_pet_graphics_shift_mode = !g_pet_graphics_shift_mode; }

// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Business (8032) keyboard matrix - independent compact path.
// Positions from the VICE buuk (business UK) keymap. shift: 0 = plain,
// 1 = press PET LSHIFT (6,0) with the key, 2 = key is unshifted on the PET
// even though the host typed it shifted (e.g. ':').
// -----------------------------------------------------------------------------
static bool g_pet_business_kbd = false;
void set_pet_business_kbd(bool on) noexcept { g_pet_business_kbd = on; }
bool get_pet_business_kbd() noexcept { return g_pet_business_kbd; }

struct BizKey { signed char row, col, shift; };
static std::array<BizKey, 128> make_biz_map() {
	std::array<BizKey, 128> m;
	for (auto& k : m) k = { -1, -1, 0 };
	auto S = [&](unsigned char ch, int r, int c, int sh = 0) { m[ch] = { (signed char)r, (signed char)c, (signed char)sh }; };
	static const signed char P[26][2] = { {3,0},{6,2},{6,1},{3,1},{5,1},{2,2},{3,2},{2,3},{4,5},{3,3},{2,5},{3,5},{8,3},
		{7,2},{5,5},{4,6},{5,0},{4,2},{2,1},{5,2},{5,3},{7,1},{4,1},{8,1},{4,3},{7,0} };
	for (int i = 0; i < 26; ++i) { S((unsigned char)('a' + i), P[i][0], P[i][1], 0); S((unsigned char)('A' + i), P[i][0], P[i][1], 1); }
	S('0',1,3); S('1',1,0); S('2',0,0); S('3',9,1); S('4',1,1); S('5',0,1); S('6',9,2); S('7',1,2); S('8',0,2); S('9',9,3);
	S(' ',8,2); S('\r',3,4); S('\n',3,4);
	S('!',1,0,1); S('@',3,6,0); S('#',9,1,1); S('$',1,1,1); S('%',0,1,1); S('^',1,5,0);
	S('&',9,2,1); S('*',9,5,1); S('(',0,2,1); S(')',9,3,1); S('-',0,3,0); S('_',3,6,1);
	S('=',0,3,1); S('+',2,6,1); S('[',5,6,0); S(']',2,4,0); S('\\',4,4,0);
	S(':',9,5,2); S(';',2,6,0); S('"',0,0,1); S('\'',1,2,1);
	S(',',7,3,0); S('<',7,3,1); S('.',6,3,0); S('>',6,3,1); S('/',8,6,0); S('?',8,6,1);
	return m;
}
static const std::array<BizKey, 128> g_biz_map = make_biz_map();

static void build_business_rows(std::uint8_t out[10],
	const unsigned char key_state[256], const BYTE kbState[256], HKL layout)
{
	bool runstop = false;
	for (int vk = 0; vk < 256; ++vk) {
		if (!key_state[vk]) continue;
		switch (vk) {   // VK specials first (business positions)
		case VK_RETURN: case VK_SEPARATOR: pet_press(out, 3, 4); continue;
		case VK_CAPITAL: pet_press(out, 9, 4); runstop = true; continue; // RUN/STOP
		case VK_HOME:   pet_press(out, 8, 4); continue;
		case VK_DELETE: case VK_BACK: pet_press(out, 4, 7); continue;
		case VK_RIGHT:  pet_press(out, 0, 5); continue;
		case VK_LEFT:   pet_press(out, 0, 5); pet_press(out, 6, 0); continue;
		case VK_DOWN:   pet_press(out, 5, 4); continue;
		case VK_UP:     pet_press(out, 5, 4); pet_press(out, 6, 0); continue;
		case VK_TAB:    pet_press(out, 4, 0); continue;
		case VK_ESCAPE: pet_press(out, 2, 0); continue;
		case VK_SPACE:  pet_press(out, 8, 2); continue;
		default: break;
		}
		unsigned char ch = 0;
		if (!vk_to_ascii((unsigned)vk, kbState, layout, ch)) continue;
		if (ch >= 128) continue;
		const BizKey bk = g_biz_map[ch];
		if (bk.row < 0) continue;
		pet_press(out, bk.row, bk.col);
		if (bk.shift == 1) pet_press(out, 6, 0);   // PET LSHIFT (business (6,0))
		// shift==2: PET key is unshifted; host shift was consumed by ToUnicodeEx
	}
	// BREAK = RUN/STOP + Shift (mirror the graphics-path behavior)
	if (runstop && ((kbState[VK_SHIFT] | kbState[VK_LSHIFT] | kbState[VK_RSHIFT]) & 0x80))
		pet_press(out, 6, 0);
}
// Core worker used by both public paths (global-keys vs supplied-keys)
// handleModeToggle: if true, toggles graphics mode on F12 edge (global path).
// pushAfterBuild  : if true, pushes to g_pet->bus().io().setKeyrows(out).
// -----------------------------------------------------------------------------
static void build_pet_rows_core(std::uint8_t out[10],
	const unsigned char key_state[256],
	bool handleModeToggle,
	bool pushAfterBuild)
{
	// Optional: toggle graphics/business mode on F12 when using global path.
	// (Was F11, but F11 is the host's fullscreen accelerator - the double
	// binding silently flipped typing mode on every fullscreen toggle.)
	static bool prevF12 = false;
	if (handleModeToggle) {
		const bool currF12 = (key_state[VK_F12] != 0);
		if (currF12 && !prevF12) {
			g_pet_graphics_shift_mode = !g_pet_graphics_shift_mode;
			OutputDebugStringA(g_pet_graphics_shift_mode
				? "[PET KBD] Graphics mode: ON (Shift+letter -> PET graphics)\n"
				: "[PET KBD] Graphics mode: OFF (business typing)\n");
		}
		prevF12 = currF12;
	}

	// Initialize to idle (all 1s): active-low matrix
	std::memset(out, 0xFF, 10);

	// Build kbState from the provided key_state flags (non-zero => VK down)
	BYTE kbState[256] = { 0 };
	for (int vk = 0; vk < 256; ++vk) if (key_state[vk]) kbState[vk] = 0x80;

	// Toggle bits (helps ToUnicodeEx)
	if (GetKeyState(VK_CAPITAL) & 1) kbState[VK_CAPITAL] |= 1;
	if (GetKeyState(VK_NUMLOCK) & 1) kbState[VK_NUMLOCK] |= 1;
	if (GetKeyState(VK_SCROLL) & 1) kbState[VK_SCROLL] |= 1;

	// Mirror L/R modifiers into aggregate keys for ToUnicodeEx
	auto mirrorAgg = [&](int agg, int left, int right) {
		if ((kbState[left] | kbState[right]) & 0x80) kbState[agg] |= 0x80;
		};
	mirrorAgg(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
	mirrorAgg(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
	mirrorAgg(VK_MENU, VK_LMENU, VK_RMENU);

	// Plain Ctrl (without Alt) has no PET meaning, but ToUnicodeEx turns
	// Ctrl+letter into control codes - Ctrl+M gives 0x0D and Ctrl+J 0x0A,
	// both of which map to PET RETURN (spurious presses). Strip Ctrl from
	// the translation state unless Alt is also down (AltGr on intl layouts).
	const bool altDown = (kbState[VK_MENU] & 0x80) != 0;
	if (!altDown && (kbState[VK_CONTROL] & 0x80)) {
		kbState[VK_CONTROL] = 0;
		kbState[VK_LCONTROL] = 0;
		kbState[VK_RCONTROL] = 0;
	}

	HKL layout = GetKeyboardLayout(0);

	// ---- Business (8032) keyboard: fully separate compact path ----
	if (g_pet_business_kbd) {
		build_business_rows(out, key_state, kbState, layout);
		return;   // update_keyboard() pushes the rows (pushAfterBuild is vestigial)
	}

	// Pass 1: handle VK-based specials (includes VK_UP and all OEM fallbacks)
	bool runstop_down = false;
	for (int vk = 0; vk < 256; ++vk) {
		if (!key_state[vk]) continue;
		if (vk == VK_CAPITAL) runstop_down = true; // RUN/STOP via CapsLock
		(void)press_pet_special_from_vk(out, vk, kbState);
	}

	// Pass 2: character path (ToUnicodeEx -> ASCII -> PET row/col)
	// Pass 2: either SYMBOLIC (ToUnicodeEx) or POSITIONAL (VK->PET)
	if (g_kbd_mode == KbdMode::Symbolic) {
		for (int vk = 0; vk < 256; ++vk) {
			if (!key_state[vk]) continue;

			// Skip specials already handled
			switch (vk) {
			case VK_SPACE: case VK_RETURN: case VK_CAPITAL: case VK_HOME:
			case VK_DELETE: case VK_BACK: case VK_DOWN: case VK_UP:
			case VK_LEFT:  case VK_RIGHT:
			case VK_OEM_7: case VK_OEM_PLUS: case VK_OEM_COMMA:
			case VK_OEM_PERIOD: case VK_OEM_MINUS: case VK_OEM_2:
				continue;
			default: break;
			}

			unsigned char ch = 0;
			if (!vk_to_ascii(static_cast<unsigned>(vk), kbState, layout, ch))
				continue;

			bool is_letter = false;
			if (ch >= 'a' && ch <= 'z') { ch = static_cast<unsigned char>(ch - 'a' + 'A'); is_letter = true; }
			else if (ch >= 'A' && ch <= 'Z') { is_letter = true; }
			
			// AFTER (do not force-case; just mark letters)
			//if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')) {
			//	is_letter = true;
			//}

			int row = -1, col = -1;
			if (!pet_pos_for_ascii(ch, row, col)) continue;

			pet_press(out, row, col);

			// Graphics-mode letters (unchanged)
			if (g_pet_graphics_shift_mode && is_letter) {
				if (kbState[VK_LSHIFT] & 0x80) pet_press(out, 8, 0);
				if (kbState[VK_RSHIFT] & 0x80) pet_press(out, 8, 5);
				if (!(kbState[VK_LSHIFT] & 0x80) && !(kbState[VK_RSHIFT] & 0x80) && (kbState[VK_SHIFT] & 0x80)) {
					pet_press(out, 8, 0);
				}
			}
		}
	}
	else { // KbdMode::Positional
		for (int vk = 0; vk < 256; ++vk) {
			if (!key_state[vk]) continue;

			// Skip keys handled in Pass-1 specials
			switch (vk) {
			case VK_SPACE: case VK_RETURN: case VK_CAPITAL: case VK_HOME:
			case VK_DELETE: case VK_BACK: case VK_DOWN: case VK_UP:
			case VK_OEM_7: case VK_OEM_PLUS: case VK_OEM_COMMA:
			case VK_OEM_PERIOD: case VK_OEM_MINUS: case VK_OEM_2:
				continue;
			default: break;
			}

			unsigned char rc = g_vk_to_pet[vk];
			if (rc == 0xFF) {
				// Try uppercase ASCII fallback
				int vk2 = vk;
				if (vk2 >= 'a' && vk2 <= 'z') vk2 = vk2 - 'a' + 'A';
				rc = g_vk_to_pet[vk2];
				if (rc == 0xFF) continue;
			}

			int row = (rc >> 4) & 0x0F;
			int col = rc & 0x0F;
			pet_press(out, row, col);

			// Graphics-mode letters in positional mode too
			if (g_pet_graphics_shift_mode && vk_is_letter(vk)) {
				if (kbState[VK_LSHIFT] & 0x80) pet_press(out, 8, 0);
				if (kbState[VK_RSHIFT] & 0x80) pet_press(out, 8, 5);
				if (!(kbState[VK_LSHIFT] & 0x80) && !(kbState[VK_RSHIFT] & 0x80) && (kbState[VK_SHIFT] & 0x80)) {
					pet_press(out, 8, 0);
				}
			}
		}
	}

	// Pass 3:
	//  a) RUN/STOP + SHIFT (BREAK) - synthesize PET shift keys too
	if (runstop_down) {
		if (kbState[VK_LSHIFT] & 0x80) pet_press(out, 8, 0); // Left Shift
		if (kbState[VK_RSHIFT] & 0x80) pet_press(out, 8, 5); // Right Shift
		if (!(kbState[VK_LSHIFT] & 0x80) && !(kbState[VK_RSHIFT] & 0x80) && (kbState[VK_SHIFT] & 0x80)) {
			pet_press(out, 8, 0); // at least one shift if only VK_SHIFT set
		}
	}

	//  b) CLR/HOME + Shift => assert a Shift key as well (classic clear screen combo)
	if (key_state[VK_HOME] && (kbState[VK_LSHIFT] & 0x80 || kbState[VK_RSHIFT] & 0x80 || kbState[VK_SHIFT] & 0x80)) {
		pet_press(out, 8, 0); // choose Left Shift (8,0). Use (8,5) for Right Shift if you prefer
	}

	const bool left_pressed = (out[0] & (1u << 5)) == 0;  // row 0, bit 5 cleared?
	const bool right_pressed = (out[0] & (1u << 7)) == 0;  // row 0, bit 7 cleared?
	const bool down_pressed = (out[1] & (1u << 6)) == 0;  // row 1, bit 6
	const bool up_pressed = (out[2] & (1u << 5)) == 0;  // row 2, bit 5


}

// -----------------------------------------------------------------------------
// Public API: build from globals (key[256]) and push (original single-call behavior).
// Also handles the F12 graphics-mode toggle.
// -----------------------------------------------------------------------------
void build_pet_rows_from_vk(std::uint8_t out[PET_KBD_ROWS_BYTES])
{
	build_pet_rows_core(out, ::key /*global*/, /*handleModeToggle=*/true, /*pushAfterBuild=*/true);
}

void update_keyboard(PetMachine* pet)
{
	std::uint8_t rows10[10];

	// Build the 10x8 active-low matrix (fills rows10, does not push)
	build_pet_rows_from_vk(rows10);

	// Count pressed bits (active-low: 0 means pressed)
	/*
	int pressed_bits = 0;
	for (int r = 0; r < 10; ++r) {
		pressed_bits += 8 - __popcnt(rows10[r]);
	}

	if (pressed_bits > 0) {
		LOG_DEBUG("[KEY] active bits this frame: %d", pressed_bits);
	}
	*/
	// After build_pet_rows_core has filled `out`
	const bool left_pressed = (rows10[0] & (1u << 5)) == 0;  // row 0, bit 5 cleared?
	const bool right_pressed = (rows10[0] & (1u << 7)) == 0;  // row 0, bit 7 cleared?
	const bool down_pressed = (rows10[1] & (1u << 6)) == 0;  // row 1, bit 6
	const bool up_pressed = (rows10[2] & (1u << 5)) == 0;  // row 2, bit 5

	if (left_pressed)  LOG_DEBUG("PET matrix: LEFT bit active (r0 c5)");
	if (right_pressed) LOG_DEBUG("PET matrix: RIGHT bit active (r0 c7)");
	if (down_pressed)  LOG_DEBUG("PET matrix: DOWN bit active (r1 c6)");
	if (up_pressed)    LOG_DEBUG("PET matrix: UP bit active (r2 c5)");

	// Push to emulator if a PetMachine* was provided
	if (pet) {
		pet->bus().io().setKeyrows(rows10);
		
	}
}