// -----------------------------------------------------------------------------
// pet_roms.cpp
// ROM-set loaders for the PET emulator (moved out of emulator.cpp to keep
// that file focused on init/frame/input plumbing).
// -----------------------------------------------------------------------------
#include "pet_roms.h"
#include "pet_machine.h"
#include "sys_log.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <vector>

#pragma warning( disable : 4996 4244)

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

// Character ROM buffers: kept alive here (setVideoCharsets takes raw pointers
// the video code reads every frame, not a copy).
static std::vector<uint8_t> g_char_rom1, g_char_rom2; // graphics bank / text bank

// Quiet readFile: no error log when the file simply isn't there (used for
// fallback chains).
static bool tryReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::path(path), ec) || ec)
		return false;
	return readFile(path, out);
}

// -----------------------------------------------------------------------------
// loadCharRom
// A PET character ROM is a single 2KB device holding BOTH display banks:
// first 1KB = graphics set (VIA CA2 low), second 1KB = text set (CA2 high).
// The zimmers files characters-1.901447-08.bin and characters-2.901447-10.bin
// are two complete ROM *versions* (901447-08 = original 2001, whose text bank
// has upper/lower case swapped; 901447-10 = 2001N and later), NOT the two
// banks. An earlier loader took the first KB of each, so "text mode" rendered
// a second graphics set - lowercase never existed on screen.
// -----------------------------------------------------------------------------
static bool loadCharRom(PetMachine& pet, const std::string& dir)
{
	std::vector<uint8_t> rom;
	const char* which = "characters-2.901447-10.bin";
	if (!tryReadFile(dir + which, rom)) {
		which = "characters-1.901447-08.bin";           // old 2001: reversed case
		if (!tryReadFile(dir + which, rom)) {
			LOG_ERROR("No character ROM found in %s", dir.c_str());
			return false;
		}
	}
	if (rom.size() < 0x800) {
		LOG_ERROR("Character ROM %s too small: %zu (need 2048)", which, rom.size());
		return false;
	}
	g_char_rom1.assign(rom.begin(), rom.begin() + 0x400);           // graphics bank
	g_char_rom2.assign(rom.begin() + 0x400, rom.begin() + 0x800);   // text bank
	pet.setVideoCharsets(g_char_rom1.data(), g_char_rom2.data());
	LOG_INFO("CHAR ROM: %s (graphics + text banks)", which);
	return true;
}

// BASIC 2 loader (zimmers.net-native file names)
bool load_pet2_romset(PetMachine& pet, const std::string& dir, bool editorN)
{
	std::vector<uint8_t> basicC, basicD, editE, kernalF;
	const std::string path_basicC = dir + "basic-2-c000.901465-01.bin";
	const std::string path_basicD = dir + "basic-2-d000.901465-02.bin";
	const std::string path_edit = dir + (editorN ? "edit-2-n.901447-24.bin"
		: "edit-2-b.901474-01.bin");
	const std::string path_kernal = dir + "kernal-2.901465-03.bin";

	LOG_INFO("Loading PET ROMs from %s (editor=%s)",
		dir.c_str(), editorN ? "N (4KB)" : "B (2KB)");

	if (!readFile(path_basicC, basicC)) return false;
	if (!readFile(path_basicD, basicD)) return false;
	if (!readFile(path_edit, editE)) return false;
	if (!readFile(path_kernal, kernalF)) return false;

	if (basicC.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", path_basicC.c_str(), basicC.size()); return false; }
	if (basicD.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", path_basicD.c_str(), basicD.size()); return false; }
	if (!(editE.size() == 0x0800 || editE.size() == 0x1000))
		LOG_ERROR("%s size=%zu (expected 2048 or 4096)", path_edit.c_str(), editE.size());
	if (kernalF.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", path_kernal.c_str(), kernalF.size()); return false; }

	if (!pet.loadRom(basicC.data(), std::min<size_t>(basicC.size(), 0x1000), 0xC000)) { LOG_ERROR("load BASIC C000 failed"); return false; }
	if (!pet.loadRom(basicD.data(), std::min<size_t>(basicD.size(), 0x1000), 0xD000)) { LOG_ERROR("load BASIC D000 failed"); return false; }
	if (!pet.loadRom(editE.data(), std::min<size_t>(editE.size(), 0x1000), 0xE000)) { LOG_ERROR("load EDIT E000 failed");  return false; }
	if (!pet.loadRom(kernalF.data(), std::min<size_t>(kernalF.size(), 0x1000), 0xF000)) { LOG_ERROR("load KERNAL F000 failed"); return false; }

	if (!loadCharRom(pet, dir)) return false;

	LOG_INFO("ROMs installed: BASIC@C000/D000, EDIT@E000, KERNAL@F000, CHARS(1KBx2)");
	return true;
}

// Load BASIC 4 (PET 40-col, "N" editor) split set by the filenames you provided.
// Maps: BASIC @ B000/C000/D000 (3x4KB), EDIT-4-N @ E000 (2KB), KERNAL-4 @ F000 (4KB).
bool load_pet4_romset(PetMachine& pet, const std::string& dir)
{
	std::vector<uint8_t> basB, basC, basD, editN, kernalF;

	const std::string p_basB = dir + "basic-4-b000.901465-23.bin"; // 4096 bytes -> $B000
	const std::string p_basC = dir + "basic-4-c000.901465-20.bin"; // 4096 bytes -> $C000
	const std::string p_basD = dir + "basic-4-d000.901465-21.bin"; // 4096 bytes -> $D000
	const std::string p_edit = dir + "edit-4-n.901447-29.bin";     // 2048 bytes -> $E000
	const std::string p_kern = dir + "kernal-4.901465-22.bin";     // 4096 bytes -> $F000

	if (!readFile(p_basB, basB)) return false;
	if (!readFile(p_basC, basC)) return false;
	if (!readFile(p_basD, basD)) return false;
	if (!readFile(p_edit, editN)) return false;
	if (!readFile(p_kern, kernalF)) return false;

	if (basB.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", p_basB.c_str(), basB.size()); return false; }
	if (basC.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", p_basC.c_str(), basC.size()); return false; }
	if (basD.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", p_basD.c_str(), basD.size()); return false; }
	if (editN.size() != 0x0800) { LOG_ERROR("%s size=%zu (expected 2048) - not installed", p_edit.c_str(), editN.size()); return false; }
	if (kernalF.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", p_kern.c_str(), kernalF.size()); return false; }

	// Install ROMs into the bus overlay + CPU MEM mirror
	if (!pet.loadRom(basB.data(), std::min<size_t>(basB.size(), 0x1000), 0xB000)) { LOG_ERROR("load BASIC B000 failed"); return false; }
	if (!pet.loadRom(basC.data(), std::min<size_t>(basC.size(), 0x1000), 0xC000)) { LOG_ERROR("load BASIC C000 failed"); return false; }
	if (!pet.loadRom(basD.data(), std::min<size_t>(basD.size(), 0x1000), 0xD000)) { LOG_ERROR("load BASIC D000 failed"); return false; }
	if (!pet.loadRom(editN.data(), std::min<size_t>(editN.size(), 0x0800), 0xE000)) { LOG_ERROR("load EDIT E000 failed");  return false; }
	if (!pet.loadRom(kernalF.data(), std::min<size_t>(kernalF.size(), 0x1000), 0xF000)) { LOG_ERROR("load KERNAL F000 failed"); return false; }

	if (!loadCharRom(pet, dir)) return false;

	LOG_INFO("ROMs installed: BASIC@B000/C000/D000, EDIT-4-N@E000, KERNAL-4@F000, CHARS(1KBx2)");
	return true;
}

// 8032: BASIC 4 B/C/D + 80-column business editor (60 Hz) + KERNAL 4.
// After install, flip the machine into 80-column mode (2 KB screen window,
// 80-col renderer default until the editor ROM programs the CRTC).
bool load_pet8032_romset(PetMachine& pet, const std::string& dir)
{
	std::vector<uint8_t> basB, basC, basD, edit80, kernalF;
	if (!readFile(dir + "basic-4-b000.901465-19.bin", basB)) return false;
	if (!readFile(dir + "basic-4-c000.901465-20.bin", basC)) return false;
	if (!readFile(dir + "basic-4-d000.901465-21.bin", basD)) return false;
	if (!readFile(dir + "edit-4-80-b-60Hz.901474-03.bin", edit80)) return false;
	if (!readFile(dir + "kernal-4.901465-22.bin", kernalF)) return false;
	if (basB.size() != 0x1000 || basC.size() != 0x1000 || basD.size() != 0x1000 ||
		edit80.size() != 0x0800 || kernalF.size() != 0x1000) {
		LOG_ERROR("[8032] ROM size mismatch - set not installed"); return false;
	}
	if (!pet.loadRom(basB.data(), 0x1000, 0xB000)) return false;
	if (!pet.loadRom(basC.data(), 0x1000, 0xC000)) return false;
	if (!pet.loadRom(basD.data(), 0x1000, 0xD000)) return false;
	if (!pet.loadRom(edit80.data(), 0x0800, 0xE000)) return false;
	if (!pet.loadRom(kernalF.data(), 0x1000, 0xF000)) return false;
	if (!loadCharRom(pet, dir)) return false;
	LOG_INFO("ROMs installed: 8032 (BASIC4 B/C/D, EDIT-4-80-B 60Hz, KERNAL-4)");
	return true;
}
