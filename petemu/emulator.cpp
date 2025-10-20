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

#include <iterator> // required for istreambuf_iterator
#include <filesystem>
#include <cstdint>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>

#pragma warning( disable : 4996 4244)

// PET machine (owns 6502, I/O (PIA+VIA), video, bus)
static PetMachine* g_pet = nullptr;
static PetGL* g_gl = nullptr;

// Persistent storage so pointers remain valid after load (in case setVideoCharsets doesn't copy)
static std::vector<uint8_t> s_charrom_lo; // first 1KB
static std::vector<uint8_t> s_charrom_hi; // second 1KB

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

	// --- Character ROMs: accept either 2KB single or 2×1KB split ---
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

	LOG_INFO("ROMs installed: BASIC@C000/D000, EDIT(N)@E000, KERNAL@F000, CHAR(2×1KB)");
	return true;
}

// Original “set-2” loader retained; now calls PetMachine::loadRom(...)
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

void reset_all()
{
	if (g_pet) g_pet->reset();
}

///////////////////////  MAIN LOOP /////////////////////////////////////
void emu_update()
{
	UINT32 dwElapsedTicks = 0;

//	LOG_INFO("FRAME START");
	
	update_keyboard(g_pet);

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

	GLSwapBuffers();
	//LOG_INFO("FRAME END");
}

void emu_init(int argc, char** argv)
{
	wglSwapIntervalEXT(1);
	 
	// Init mixer. (removed)

	// --- Create PET machine (CPU + memory bus + I/O + video) ---
	g_pet = new PetMachine();

	//g_pet->bus().setRamSize(8 * 1024);  // classic PET RAM size
	//g_pet->bus().setRamSize(16 * 1024);  // classic PET RAM size
	g_pet->bus().setRamSize(32 * 1024);  // classic PET RAM size


	// --- Load ROM set from disk (adjust path / editor choice as needed) ---
	const std::string romdir = "./roms/";
	// const bool editorN = false;
	// if (!load_pet2_romset(*g_pet, romdir, editorN)) {  LOG_ERROR("[PET] Failed to load 2001N ROM set from %s", romdir.c_str());
	//    std::exit(1); }

	//if (!load_pet2001n_romset(*g_pet, romdir)) {
	//	LOG_ERROR("[PET] Failed to load 2001N ROM set from %s", romdir.c_str());
	//	std::exit(1);
	//}
	// --- Choose ROM set via CLI flags ---
	bool ok = false;
	if (arg_has(argc, argv, "-basic4")) {
		LOG_INFO("[PET] Using BASIC 4 ROM set");
		ok = load_pet4_romset(*g_pet, romdir);
	}
	else if (arg_has(argc, argv, "-basic2")) {
		LOG_INFO("[PET] Using BASIC 2 ROM set");
		const bool editorN = true; // prefer 40-col N editor here
		ok = load_pet2_romset(*g_pet, romdir, editorN);
	}
	else {
		// Fallback: your existing MAME-name BASIC 2 set
		LOG_INFO("[PET] No -basic2/-basic4 flag; defaulting to 2001N BASIC 2 set");
		ok = load_pet2001n_romset(*g_pet, romdir);
	}

	if (!ok) {
		LOG_ERROR("[PET] Failed to load requested ROM set from %s", romdir.c_str());
		std::exit(1);
	}


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
			 LoadPrgIntoIEEE(*g_pet, hostPath);   // requires pet_ieee_helpers.h
			LOG_INFO("[IEEE] Folder backend active. Use LOAD\"%s\",8  (or LOAD\"$\",8 + LIST)",
				diskArg.c_str());
		}
	}

	// --- Create the GL presenter (no swap/poll inside; your loop does that) ---
	g_gl = PetGL::create(SCREEN_W, SCREEN_H, "Commodore PET 2001", /*onKey*/nullptr);
	if (!g_gl) {
		LOG_ERROR("PetGL::create failed");
		std::exit(1);
	}

	set_pet_graphics_mode(false);
	
	// Tell the IEEE device about host root
	g_pet->bus().io().setIeeeHostRoot(filesRoot);
}

void emu_end()
{
	LOG_INFO("Calling Exit");

	delete g_pet;
	g_pet = nullptr;
}