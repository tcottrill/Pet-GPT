#include "framework.h"
#include "glew.h"
#include "wglew.h"
#include "sys_log.h"
#include "rawinput.h"
#include "fileio.h"
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <process.h>
#include <memory.h>
#include "cpu_6502.h"
#include "fast_poly.h"

#include "sys_gl.h"

#include "pet_machine.h"
#include "pet2001io.h"      // for io.setKeyrows(...)
#include "pet_mem.h"
#include "pet_gl.h"
#include "pet_kbd_input.h"
#include "ieee_helpers.h"
#include "pet2001ieee.h"
#include "basic_prg.h"       // basic_relink (rebuild BASIC line links after PRG inject)
#include "mixer.h"

#include <iterator> // required for istreambuf_iterator
#include <filesystem>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include "joystick.h"        // XInput/WinMM gamepad layer
#include "iniFile.h"         // pet.ini config
#include "snes_adapter.h"    // SNES button bit positions

#pragma warning( disable : 4996 4244)

// -----------------------------------------------------------------
// SNES adapter (user-port joystick) host bridge
// -----------------------------------------------------------------
static bool g_snes_enabled = true;

// Map the host gamepad (Joystick.cpp, player 1) to the 12 SNES button bits.
// Xbox layout: joy_b1=A joy_b2=B joy_b3=X joy_b4=Y joy_b5=LB joy_b6=RB
//              joy_b7=Back joy_b8=Start.
static uint16_t map_joy_to_snes()
{
	if (!joystick_any_connected()) return 0;
	uint16_t m = 0;
	if (joy_up)    m |= (1u << SnesAdapter::BTN_UP);
	if (joy_down)  m |= (1u << SnesAdapter::BTN_DOWN);
	if (joy_left)  m |= (1u << SnesAdapter::BTN_LEFT);
	if (joy_right) m |= (1u << SnesAdapter::BTN_RIGHT);
	if (joy_b1)    m |= (1u << SnesAdapter::BTN_B);      // A   -> B (fire)
	if (joy_b2)    m |= (1u << SnesAdapter::BTN_A);      // B   -> A
	if (joy_b3)    m |= (1u << SnesAdapter::BTN_Y);      // X   -> Y
	if (joy_b4)    m |= (1u << SnesAdapter::BTN_X);      // Y   -> X
	if (joy_b5)    m |= (1u << SnesAdapter::BTN_L);      // LB  -> L
	if (joy_b6)    m |= (1u << SnesAdapter::BTN_R);      // RB  -> R
	if (joy_b7)    m |= (1u << SnesAdapter::BTN_SELECT); // Back-> Select
	if (joy_b8)    m |= (1u << SnesAdapter::BTN_START);  // Start->Start
	return m;
}

int16_t* stream_data = nullptr;
const UINT32 sampleRate = 44100;
const UINT32 frameCount = 735;

// Audio State
static bool s_last_cb2_level = false;
static double s_dc_offset = 0.0;
static double s_lpf_output = 0.0; // Low Pass Filter state

// CB2 -> PCM reconstruction (extracted, testable module: sys_audio/cb2_render.h).
#include "cb2_render.h"
static Cb2Render g_cb2render;

// NOTE: the CB2 duty/reconstruction logic now lives in sys_audio/cb2_render.h
// (Cb2Render). It was moved out of this file so it can be unit-tested and so
// there is a single authoritative copy (no more editing a dead duplicate).

// PET machine (owns 6502, I/O (PIA+VIA), video, bus)
static PetMachine* g_pet = nullptr;
static PetGL* g_gl = nullptr;

// Persistent storage so pointers remain valid after load (in case setVideoCharsets doesn't copy)
static std::vector<uint8_t> s_charrom_lo; // first 1KB
static std::vector<uint8_t> s_charrom_hi; // second 1KB

//Audio mixer

// Somewhere global / per-PET
static int16_t g_cb2_frame_buffer[2048]; // choose a size matching your audio frame
static int     g_cb2_frame_samples = 0;  // number of samples per frame
static int     g_cb2_volume = 128; // mid volume 0..255

void pet_audio_init(int device_rate, int fps)
{
	// Pick how many samples you want per emu frame.
	// For example, device_rate / fps.
	g_cb2_frame_samples = device_rate / fps;
}
// FIX: Add this function to reset audio state
void reset_audio()
{
	s_last_cb2_level = false;
	s_dc_offset = 0.0;
	g_cb2render.reset();   // clear reconstruction filter state
	// Clear buffer
	if (stream_data) {
		memset(stream_data, 0, frameCount * sizeof(int16_t));
	}
}

int screenw;
int screenh;
int windowed;

UINT32 frames = 0;
UINT32 dwElapsedTicks = 0;

UINT32 dwResult = 0;
UINT32 dwDisplayInterval = 0;
UINT32 dwIntTotal = 0;

// Simple boolean flag scan (e.g., -basic4)
static bool arg_has(int argc, char** argv, const char* key) {
	for (int i = 1; i < argc; ++i) {
		if (_stricmp(argv[i], key) == 0) return true;
	}
	return false;
}

// Simple arg scan
static std::string arg_get(int argc, char** argv, const char* key) {
	for (int i = 1; i + 1 < argc; ++i) {
		if (_stricmp(argv[i], key) == 0) return std::string(argv[i + 1]);
	}
	return {};
}

static bool ends_with_icase(const std::string& s, const char* suffix) {
	const size_t n = std::strlen(suffix);
	if (s.size() < n) return false;
	for (size_t i = 0; i < n; ++i) {
		char a = s[s.size() - n + i], b = suffix[i];
		if (a >= 'a' && a <= 'z') a -= 32;
		if (b >= 'a' && b <= 'z') b -= 32;
		if (a != b) return false;
	}
	return true;
}

static bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();

	std::error_code ec;
	const auto sz = std::filesystem::file_size(std::filesystem::path(path), ec);
	if (ec || sz == static_cast<uintmax_t>(-1) || sz == 0) {
		LOG_ERROR("file_size failed or empty: %s", path.c_str());
		return false;
	}

	out.resize(static_cast<size_t>(sz));

	std::ifstream f(path, std::ios::binary);
	if (!f) {
		LOG_ERROR("Can't open: %s", path.c_str());
		out.clear();
		return false;
	}

	f.read(reinterpret_cast<char*>(out.data()),
		static_cast<std::streamsize>(out.size()));

	if (!f || f.gcount() != static_cast<std::streamsize>(out.size())) {
		LOG_ERROR("Short read: %s got=%lld want=%zu",
			path.c_str(), static_cast<long long>(f.gcount()), out.size());
		out.clear();
		return false;
	}

	LOG_DEBUG("Loaded %s (%zu bytes)", path.c_str(), out.size());
	return true;
}

static std::vector<uint8_t> g_char_rom1, g_char_rom2; // keep alive for video

// Load 2001N set by MAME-style names
static bool load_pet2001n_romset(PetMachine& m, const std::string& dir)
{
	auto rd = [](const std::string& p, std::vector<uint8_t>& out)->bool {
		std::ifstream f(p, std::ios::binary);
		if (!f) { LOG_ERROR("Can't open %s", p.c_str()); return false; }
		out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		if (out.empty()) { LOG_ERROR("Empty file %s", p.c_str()); return false; }
		LOG_DEBUG("Loaded %s (%zu bytes)", p.c_str(), out.size());
		return true;
		};

	// --- CPU ROMs ---
	std::vector<uint8_t> basicC, basicD, editN, kernal;
	if (!rd(dir + "901465-01.ud6", basicC)) return false;   // BASIC 2 @ C000
	if (!rd(dir + "901465-02.ud7", basicD)) return false;   // BASIC 2 @ D000
	if (!rd(dir + "901447-24.ud8", editN))  return false;   // EDIT (normal) @ E000 (2KB)
	if (!rd(dir + "901465-03.ud9", kernal)) return false;   // KERNAL @ F000

	// Use PetMachine::loadRom so CPU MEM mirror is kept in sync
	if (!m.loadRom(basicC.data(), basicC.size(), 0xC000)) return false;
	if (!m.loadRom(basicD.data(), basicD.size(), 0xD000)) return false;
	if (!m.loadRom(editN.data(), editN.size(), 0xE000)) return false;
	if (!m.loadRom(kernal.data(), kernal.size(), 0xF000)) return false;

	// --- Character ROMs: accept either 2KB single or 2x1KB split ---
	s_charrom_lo.clear(); s_charrom_hi.clear();

	// Preferred split files (as per your earlier set)
	std::vector<uint8_t> char1, char2;
	bool have_split =
		rd(dir + "characters-1.901447-08.bin", char1) &&
		rd(dir + "characters-2.901447-10.bin", char2);

	if (have_split) {
		if (char1.size() < 0x400 || char2.size() < 0x400) {
			LOG_ERROR("Character split ROM(s) too small: got %zu / %zu (need >= 1024 each)",
				char1.size(), char2.size());
			return false;
		}
		s_charrom_lo.assign(char1.begin(), char1.begin() + 0x400);
		s_charrom_hi.assign(char2.begin(), char2.begin() + 0x400);
		LOG_INFO("CHAR ROMs: using split files (1KB each): %s , %s",
			"characters-1.901447-08.bin", "characters-2.901447-10.bin");
	}
	else {
		// Fallback: single 2KB PET char ROM (MAME name)
		std::vector<uint8_t> chargen2k;
		if (!rd(dir + "901447-10.uf10", chargen2k)) {
			LOG_ERROR("No character ROM found (tried split and 2KB single).");
			return false;
		}
		if (chargen2k.size() < 0x800) {
			LOG_ERROR("Character ROM too small: %zu (need 2048)", chargen2k.size());
			return false;
		}
		// Split 2KB -> two 1KB banks
		s_charrom_lo.assign(chargen2k.begin(), chargen2k.begin() + 0x400);
		s_charrom_hi.assign(chargen2k.begin() + 0x400, chargen2k.begin() + 0x800);
		LOG_INFO("CHAR ROM: using 2KB file 901447-10.uf10 (split into two 1KB banks).");
	}

	// Hand both 1KB banks to the video
	m.setVideoCharsets(s_charrom_lo.data(), s_charrom_hi.data());

	LOG_INFO("ROMs installed: BASIC@C000/D000, EDIT(N)@E000, KERNAL@F000, CHAR(2x1KB)");
	return true;
}

// Original "set-2" loader retained; now calls PetMachine::loadRom(...)
static bool load_pet2_romset(PetMachine& pet, const std::string& dir, bool editorN)
{
	std::vector<uint8_t> basicC, basicD, editE, kernalF, char1, char2;
	const std::string path_basicC = dir + "basic-2-c000.901465-01.bin";
	const std::string path_basicD = dir + "basic-2-d000.901465-02.bin";
	const std::string path_edit = dir + (editorN ? "edit-2-n.901447-24.bin"
		: "edit-2-b.901474-01.bin");
	const std::string path_kernal = dir + "kernal-2.901465-03.bin";
	const std::string path_ch1 = dir + "characters-1.901447-08.bin";
	const std::string path_ch2 = dir + "characters-2.901447-10.bin";

	LOG_INFO("Loading PET ROMs from %s (editor=%s)",
		dir.c_str(), editorN ? "N (4KB)" : "B (2KB)");

	if (!readFile(path_basicC, basicC)) return false;
	if (!readFile(path_basicD, basicD)) return false;
	if (!readFile(path_edit, editE)) return false;
	if (!readFile(path_kernal, kernalF)) return false;
	if (!readFile(path_ch1, char1)) return false;
	if (!readFile(path_ch2, char2)) return false;

	if (basicC.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", path_basicC.c_str(), basicC.size());
	if (basicD.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", path_basicD.c_str(), basicD.size());
	if (!(editE.size() == 0x0800 || editE.size() == 0x1000))
		LOG_ERROR("%s size=%zu (expected 2048 or 4096)", path_edit.c_str(), editE.size());
	if (kernalF.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", path_kernal.c_str(), kernalF.size());
	if (char1.size() < 0x0400)    LOG_ERROR("%s size=%zu (expected >=1024)", path_ch1.c_str(), char1.size());
	if (char2.size() < 0x0400)    LOG_ERROR("%s size=%zu (expected >=1024)", path_ch2.c_str(), char2.size());

	if (!pet.loadRom(basicC.data(), std::min<size_t>(basicC.size(), 0x1000), 0xC000)) { LOG_ERROR("load BASIC C000 failed"); return false; }
	if (!pet.loadRom(basicD.data(), std::min<size_t>(basicD.size(), 0x1000), 0xD000)) { LOG_ERROR("load BASIC D000 failed"); return false; }
	if (!pet.loadRom(editE.data(), std::min<size_t>(editE.size(), 0x1000), 0xE000)) { LOG_ERROR("load EDIT E000 failed");  return false; }
	if (!pet.loadRom(kernalF.data(), std::min<size_t>(kernalF.size(), 0x1000), 0xF000)) { LOG_ERROR("load KERNAL F000 failed"); return false; }

	// Char ROMs: take first 1KB of each
	g_char_rom1.assign(char1.begin(), char1.begin() + std::min<size_t>(char1.size(), 0x400));
	g_char_rom2.assign(char2.begin(), char2.begin() + std::min<size_t>(char2.size(), 0x400));
	pet.setVideoCharsets(g_char_rom1.data(), g_char_rom2.data());

	LOG_INFO("ROMs installed: BASIC@C000/D000, EDIT@E000, KERNAL@F000, CHARS(1KBx2)");
	return true;
}

// Load BASIC 4 (PET 40-col, "N" editor) split set by the filenames you provided.
// Maps: BASIC @ B000/C000/D000 (3x4KB), EDIT-4-N @ E000 (2KB), KERNAL-4 @ F000 (4KB).
static bool load_pet4_romset(PetMachine& pet, const std::string& dir)
{
	std::vector<uint8_t> basB, basC, basD, editN, kernalF, char1, char2;

	const std::string p_basB = dir + "basic-4-b000.901465-23.bin"; // 4096 bytes -> $B000
	const std::string p_basC = dir + "basic-4-c000.901465-20.bin"; // 4096 bytes -> $C000
	const std::string p_basD = dir + "basic-4-d000.901465-21.bin"; // 4096 bytes -> $D000
	const std::string p_edit = dir + "edit-4-n.901447-29.bin";     // 2048 bytes -> $E000
	const std::string p_kern = dir + "kernal-4.901465-22.bin";     // 4096 bytes -> $F000
	const std::string p_ch1 = dir + "characters-1.901447-08.bin"; // >=1024 (use first 1KB)
	const std::string p_ch2 = dir + "characters-2.901447-10.bin"; // >=1024 (use first 1KB)

	if (!readFile(p_basB, basB)) return false;
	if (!readFile(p_basC, basC)) return false;
	if (!readFile(p_basD, basD)) return false;
	if (!readFile(p_edit, editN)) return false;
	if (!readFile(p_kern, kernalF)) return false;
	if (!readFile(p_ch1, char1)) return false;
	if (!readFile(p_ch2, char2)) return false;

	if (basB.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", p_basB.c_str(), basB.size());
	if (basC.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", p_basC.c_str(), basC.size());
	if (basD.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", p_basD.c_str(), basD.size());
	if (editN.size() != 0x0800) LOG_ERROR("%s size=%zu (expected 2048)", p_edit.c_str(), editN.size());
	if (kernalF.size() != 0x1000) LOG_ERROR("%s size=%zu (expected 4096)", p_kern.c_str(), kernalF.size());
	if (char1.size() < 0x0400)    LOG_ERROR("%s size=%zu (expected >=1024)", p_ch1.c_str(), char1.size());
	if (char2.size() < 0x0400)    LOG_ERROR("%s size=%zu (expected >=1024)", p_ch2.c_str(), char2.size());

	// Install ROMs into the bus overlay + CPU MEM mirror
	if (!pet.loadRom(basB.data(), std::min<size_t>(basB.size(), 0x1000), 0xB000)) { LOG_ERROR("load BASIC B000 failed"); return false; }
	if (!pet.loadRom(basC.data(), std::min<size_t>(basC.size(), 0x1000), 0xC000)) { LOG_ERROR("load BASIC C000 failed"); return false; }
	if (!pet.loadRom(basD.data(), std::min<size_t>(basD.size(), 0x1000), 0xD000)) { LOG_ERROR("load BASIC D000 failed"); return false; }
	if (!pet.loadRom(editN.data(), std::min<size_t>(editN.size(), 0x0800), 0xE000)) { LOG_ERROR("load EDIT E000 failed");  return false; }
	if (!pet.loadRom(kernalF.data(), std::min<size_t>(kernalF.size(), 0x1000), 0xF000)) { LOG_ERROR("load KERNAL F000 failed"); return false; }

	// Characters: take first 1KB from each file (like BASIC 2 path)
	g_char_rom1.assign(char1.begin(), char1.begin() + 0x400);
	g_char_rom2.assign(char2.begin(), char2.begin() + 0x400);
	pet.setVideoCharsets(g_char_rom1.data(), g_char_rom2.data());

	LOG_INFO("ROMs installed: BASIC@B000/C000/D000, EDIT-4-N@E000, KERNAL-4@F000, CHARS(1KBx2)");
	return true;
}

static int g_basic_set = 2;  // 2 or 4
static int g_ram_kb    = 32; // configured RAM size in KB (4/8/16/32)

static bool load_basic_set(int which) {
	const std::string romdir = "./roms/";
	bool ok = (which == 4) ? load_pet4_romset(*g_pet, romdir)
	                       : load_pet2001n_romset(*g_pet, romdir); // BASIC 2 default boot set
	if (!ok) { LOG_ERROR("[PET] Failed to load BASIC %d ROM set from %s", which, romdir.c_str()); return false; }
	g_basic_set = which;
	return true;
}

void reset_all()
{
	if (g_pet) g_pet->reset();
	reset_audio();
}

///////////////////////  MAIN LOOP /////////////////////////////////////
bool emu_run_frame()
{
	g_pet->io().cb2ResetEdgeLog();

	update_keyboard(g_pet);

	// Poll the host gamepad and feed the emulated SNES adapter.
	if (g_snes_enabled) {
		poll_joystick();
		g_pet->io().setSnesButtons(map_joy_to_snes());
	}

	// 2) Run ~1/60 sec of CPU afterward
	const int cycles_per_frame = 1000000 / 60;
	if (g_pet) g_pet->runCycles(cycles_per_frame);

	// 3) Present video
	if (g_gl && g_pet) {
		const uint32_t* fb = g_pet->video().framebuffer();
		const int fbW = g_pet->video().fbWidth();
		const int fbH = g_pet->video().fbHeight();
		g_gl->present(fb, fbW, fbH);
	}
	// -----------------------------------------------------------------
	// AUDIO GENERATION
	// -----------------------------------------------------------------
	const uint32_t edgeCount = g_pet->io().cb2GetEdgeCount();
	const CB2Edge* edges = g_pet->io().cb2GetEdges();
	const bool frameStartLevel = g_pet->io().cb2GetFrameStartLevel();

	// Map the frame's transitions over the ACTUAL number of cycles that ran this
	// frame (not a fixed nominal span), so no tail transition is dropped and the
	// audio time-base matches game time exactly.
	const double frameCycles = (double)g_pet->io().cb2GetTickCounter();

	g_cb2render.render(edges, edgeCount, frameStartLevel, stream_data, (int)frameCount, frameCycles);

	// Send to sound driver
	stream_update(1, stream_data); // 1 channel

	mixer_update();

	extern unsigned char key[256];
	if (key[VK_ESCAPE]) return false;   // host decides: fullscreen->windowed, else quit
	return true;
}

void emu_init(int argc, char** argv)
{
	// Init mixer.
	mixer_init(44100, 60);
	
	// --- Create PET machine (CPU + memory bus + I/O + video) ---
	g_pet = new PetMachine();

	// RAM size from pet.ini [machine] ram (KB); default 32K. (4/8/16/32 only.)
	g_ram_kb = get_config_int("machine", "ram", 32);
	if (g_ram_kb != 4 && g_ram_kb != 8 && g_ram_kb != 16 && g_ram_kb != 32) g_ram_kb = 32;
	g_pet->bus().setRamSize((size_t)g_ram_kb * 1024);

	// --- Load ROM set from disk ---
	int want = arg_has(argc, argv, "-basic4") ? 4
	         : arg_has(argc, argv, "-basic2") ? 2
	         : get_config_int("machine", "basic", 2);
	if (!load_basic_set(want)) std::exit(1);

	// --- Sanity: verify ROM mapping and vectors ---
	auto& mem = g_pet->bus();

	// Check a few bytes in KERNAL at $F000
	uint8_t f000 = mem.readByte(0xF000);
	uint8_t f001 = mem.readByte(0xF001);
	LOG_INFO("KERNAL $F000 bytes: %02X %02X ...", f000, f001);

	// Confirm CPU reset vector bytes at $FFFC/$FFFD
	uint8_t rv_lo = mem.readByte(0xFFFC);
	uint8_t rv_hi = mem.readByte(0xFFFD);
	uint16_t rv = uint16_t(rv_hi) << 8 | rv_lo;
	LOG_INFO("Reset vector @ $FFFC = %02X %02X -> $%04X", rv_lo, rv_hi, rv);

	// (Optional) check IRQ/NMI vectors too
	uint8_t ir_lo = mem.readByte(0xFFFE), ir_hi = mem.readByte(0xFFFF);
	LOG_INFO("IRQ/BRK vector @ $FFFE = %02X %02X -> $%04X", ir_lo, ir_hi, (ir_hi << 8) | ir_lo);
	uint8_t nm_lo = mem.readByte(0xFFFA), nm_hi = mem.readByte(0xFFFB);
	LOG_INFO("NMI vector  @ $FFFA = %02X %02X -> $%04X", nm_lo, nm_hi, (nm_hi << 8) | nm_lo);

	// --- Reset PET ---
	g_pet->reset();

	LOG_INFO("BASIC $C000 bytes: %02X %02X %02X", mem.readByte(0xC000), mem.readByte(0xC001), mem.readByte(0xC002));
	LOG_INFO("BASIC $D000 bytes: %02X %02X %02X", mem.readByte(0xD000), mem.readByte(0xD001), mem.readByte(0xD002));

	// --- Minimal IEEE virtual drive setup ---
	const std::string filesRoot = "./files";
	g_pet->bus().io().setIeeeHostRoot(filesRoot);   // always mount the folder

	// Parse: -disk "filename.xxx" (resolved relative to ./files)
	const std::string diskArg = arg_get(argc, argv, "-disk");
	if (!diskArg.empty()) {
		const std::string hostPath = filesRoot + "/" + diskArg;

		if (ends_with_icase(diskArg, ".d64")) {
			// Mount a read-only D64 image
			if (!g_pet->bus().io().setIeeeD64Image(hostPath)) {
				LOG_ERROR("[IEEE] Failed to mount D64: %s", hostPath.c_str());
			}
			else {
				LOG_INFO("[IEEE] D64 mounted: %s", hostPath.c_str());
			}
		}
		else {
			// Keep D64 disabled; folder backend will serve files by name.
			// Optional: auto-prime a PRG so LOAD\"\",8 works immediately.
			// If you prefer manual LOAD\"NAME\",8, simply comment this out.
			ieee_helpers::LoadPrgIntoIEEE(*g_pet, hostPath);   // requires pet_ieee_helpers.h
			LOG_INFO("[IEEE] Folder backend active. Use LOAD\"%s\",8  (or LOAD\"$\",8 + LIST)",
				diskArg.c_str());
		}
	}

	// --- Create the GL presenter (no swap/poll inside) ---
	g_gl = PetGL::create(SCREEN_W, SCREEN_H, "Commodore PET 2001", /*onKey*/nullptr);
	if (!g_gl) {
		LOG_ERROR("PetGL::create failed");
		std::exit(1);
	}

	set_pet_graphics_mode(false);

	// Tell the IEEE device about host root
	g_pet->bus().io().setIeeeHostRoot(filesRoot);

	//Setup the audio stream
	int frames_per_update = 44100 / 60;
	stream_data = (int16_t*)std::malloc(frames_per_update * sizeof(int16_t));

	for (int i = 0; i < frames_per_update; ++i)
	{
		if (stream_data) stream_data[i] = 0;
	}
	stream_start(1, 1, 16, 60, 0);

	// Configure the CB2 reconstruction module (1 MHz PET CPU -> 44100 output).
	g_cb2render.configure(/*fs*/ (double)sampleRate, /*cpuHz*/ 1000000.0, /*vol*/ 8000.0);
	g_cb2render.reset();

	// --- Input: SNES user-port adapter (optional, via pet.ini) ---
	SetIniFile("pet.ini");
	g_snes_enabled = get_config_bool("input", "snes_adapter", true);
	const bool snesInvert = get_config_bool("input", "snes_invert", false);
	install_joystick();
	g_pet->io().setSnesInvert(snesInvert);
	g_pet->io().setSnesEnabled(g_snes_enabled);
	LOG_INFO("SNES adapter: %s (invert=%d), joystick driver=%s",
		g_snes_enabled ? "enabled" : "disabled", snesInvert ? 1 : 0, joystick_driver_name());
}

// Eject a mounted .d64: clearing the image leaves hostRoot ("./files") in place,
// so device 8 falls straight back to the folder vdrive. No machine reset.
void pet_eject_disk() {
	if (!g_pet) return;
	if (!g_pet->bus().io().ieeeIsD64Mounted()) {
		LOG_INFO("[PET] eject: no disk mounted");
		return;
	}
	g_pet->bus().io().setIeeeD64Image("");   // unmount -> ./files folder vdrive
	LOG_INFO("[PET] disk ejected; device 8 back to ./files virtual drive");
}

int pet_get_disk_mounted() {
	return (g_pet && g_pet->bus().io().ieeeIsD64Mounted()) ? 1 : 0;
}

void pet_reset() { if (g_pet) g_pet->reset(); }

void pet_set_basic(int which) {
	if (which != 2 && which != 4) return;
	if (!load_basic_set(which)) return;   // PetMachine::loadRom keeps the CPU MEM mirror in sync
	g_pet->reset();
	LOG_INFO("[PET] switched to BASIC %d", which);
}

void pet_set_ram(int kb) {
	if (kb != 4 && kb != 8 && kb != 16 && kb != 32) return;
	if (!g_pet) return;
	g_pet->bus().setRamSize((size_t)kb * 1024);
	g_ram_kb = kb;
	g_pet->reset();
	LOG_INFO("[PET] RAM size set to %dK", kb);
}

void pet_set_crt(int on) { if (g_gl) g_gl->setCrtEnabled(on != 0); }
int  pet_get_crt()       { return (g_gl && g_gl->getCrtEnabled()) ? 1 : 0; }

// Reset to a clean BASIC and run the boot forward until the screen shows the
// "READY." prompt, the way VICE's autostart detects readiness (scan the screen,
// don't guess a delay). Needed because right after reset() the CPU has only had
// its PC set to the reset vector; BASIC's cold-start (RAM test, NEW, screen
// clear) runs over the next frames and would wipe a program injected too early.
// Returns true if "READY." was seen; false if the frame cap was hit first (we
// still inject - cold-start has almost certainly finished by then).
static bool reset_and_wait_for_ready()
{
	if (!g_pet) return false;
	g_pet->reset();

	static const uint8_t READY[6] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2E }; // "READY." screen codes
	const int cyclesPerFrame = 1000000 / 60;
	const int maxFrames = 300;   // ~5 s emulated backstop; the loop exits as soon as READY appears

	auto screen_has_ready = []() -> bool {
		for (uint16_t base = VIDRAM_ADDR; base + 6 <= VIDRAM_END + 1; ++base) {
			bool hit = true;
			for (int k = 0; k < 6; ++k)
				if (g_pet->bus().readByte((uint16_t)(base + k)) != READY[k]) { hit = false; break; }
			if (hit) return true;
		}
		return false;
	};

	for (int f = 0; f < maxFrames; ++f) {
		g_pet->runCycles(cyclesPerFrame);
		if (screen_has_ready()) {
			LOG_INFO("[PET] reset: READY after %d frame(s)", f + 1);
			return true;
		}
	}
	LOG_WARN("[PET] reset: 'READY.' not seen within %d frames; injecting anyway", maxFrames);
	return false;
}

void pet_load_software(const char* utf8_path) {
	if (!g_pet || !utf8_path) return;
	std::string p = utf8_path;
	std::string ext; { size_t d = p.find_last_of('.'); if (d != std::string::npos) ext = p.substr(d + 1); }
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return (char)tolower(c); });

	if (ext == "d64") {
		if (g_pet->bus().io().setIeeeD64Image(p))
			LOG_INFO("[PET] mounted D64 '%s'", p.c_str());
		else
			LOG_ERROR("[PET] failed to mount D64 '%s'", p.c_str());
		return;
	}
	// .prg: deposit the program directly into PET RAM at its own load address
	// (equivalent to LOAD"name",8,1). The CPU executes from the same RAM buffer
	// PetMem::writeByte() stores into, so the bytes are visible immediately.
	std::vector<uint8_t> file;
	if (!ieee_helpers::read_all_file(p, file) || file.size() < 2) {
		LOG_ERROR("[PET] failed to read PRG '%s'", p.c_str());
		return;
	}

	// Reboot to a clean BASIC before injecting, so loading a new program can't
	// corrupt/crash whatever is currently running. The ./files vdrive persists
	// across reset, and we wait for the READY prompt so cold-start doesn't wipe
	// the program we're about to inject.
	reset_and_wait_for_ready();

	auto& mem = g_pet->bus();
	const uint16_t loadAddr = (uint16_t)file[0] | ((uint16_t)file[1] << 8);
	const size_t   nbytes   = file.size() - 2;
	const uint32_t endAddr  = (uint32_t)loadAddr + (uint32_t)nbytes;   // exclusive

	if (endAddr > mem.ramConfiguredBytes())
		LOG_WARN("[PET] PRG '%s' ends at $%04X, past configured RAM ($%04zX); "
		         "trailing bytes not loaded", p.c_str(), (unsigned)endAddr, mem.ramConfiguredBytes());

	for (size_t i = 0; i < nbytes; ++i) {
		const uint32_t a = (uint32_t)loadAddr + (uint32_t)i;
		if (a > 0xFFFF) break;                          // never wrap past $FFFF
		mem.writeByte((uint16_t)a, file[i + 2]);
	}

	if (loadAddr == 0x0401) {
		// BASIC program: make it LIST/RUN-able by setting the BASIC zero-page
		// pointers. PET layout (constant across BASIC 2/4): TXTTAB=$28/$29,
		// VARTAB=$2A/$2B, ARYTAB=$2C/$2D, STREND=$2E/$2F. VARTAB is the byte
		// after the program (start of variables = end of program).
		const uint16_t before = (uint16_t)(mem.readByte(0x28) | (mem.readByte(0x29) << 8));
		LOG_INFO("[PET] (sanity) TXTTAB $28/$29 read $%04X before load (expect $0401 once booted)", before);

		const uint16_t txttab = 0x0401;

		// Rebuild the forward line links for this load address, exactly as a
		// relocating LOAD does. Programs saved for a different start (e.g. a
		// $0801 C64-style image that nonetheless loads at $0401) carry links
		// that are wrong here; without relinking, RUN follows a broken chain.
		// (Run for the side effect; VARTAB stays at end-of-file so any trailing
		// ML after a BASIC SYS stub isn't clobbered by variables.)
		basic_relink(mem.ramData(), txttab, endAddr);
		const uint16_t vartab = (uint16_t)endAddr;
		mem.writeByte(0x28, (uint8_t)(txttab & 0xFF)); mem.writeByte(0x29, (uint8_t)(txttab >> 8));
		mem.writeByte(0x2A, (uint8_t)(vartab & 0xFF)); mem.writeByte(0x2B, (uint8_t)(vartab >> 8));
		mem.writeByte(0x2C, (uint8_t)(vartab & 0xFF)); mem.writeByte(0x2D, (uint8_t)(vartab >> 8)); // ARYTAB (defensive)
		mem.writeByte(0x2E, (uint8_t)(vartab & 0xFF)); mem.writeByte(0x2F, (uint8_t)(vartab >> 8)); // STREND (defensive)

		LOG_INFO("[PET] loaded BASIC PRG '%s' @ $%04X (%zu bytes); VARTAB=$%04X - type RUN",
		         p.c_str(), loadAddr, nbytes, vartab);
	}
	else {
		LOG_INFO("[PET] loaded ML PRG '%s' @ $%04X (%zu bytes) - run with SYS %u",
		         p.c_str(), loadAddr, nbytes, (unsigned)loadAddr);
	}
}

void emu_end()
{
	LOG_INFO("Calling Exit");
	remove_joystick();
	stream_stop(1, 1);
	mixer_end();
	delete g_pet;
	g_pet = nullptr;
}