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

#ifdef _MSC_VER
#pragma warning( disable : 4996 4244)
#endif

static bool readFile(const std::string& path, std::vector<uint8_t>& out)
{
	out.clear();

	std::error_code ec;
	const auto sz = std::filesystem::file_size(std::filesystem::u8path(path), ec);
	if (ec || sz == static_cast<uintmax_t>(-1) || sz == 0) {
		LOG_ERROR("file_size failed or empty: %s", path.c_str());
		return false;
	}

	out.resize(static_cast<size_t>(sz));

	std::ifstream f(std::filesystem::u8path(path), std::ios::binary);
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
static std::vector<uint8_t> g_char_rom3, g_char_rom4; // same, upper half of a 4 KB character ROM

// Quiet readFile: no error log when the file simply isn't there (used for
// fallback chains).
static bool tryReadFile(const std::string& path, std::vector<uint8_t>& out)
{
	std::error_code ec;
	if (!std::filesystem::exists(std::filesystem::u8path(path), ec) || ec)
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
static bool readCharRom(const std::string& dir, std::vector<uint8_t>& rom, bool original = false)
{
	const char* which = original ? "characters-1.901447-08.bin" : "characters-2.901447-10.bin";
	if (original && !readFile(dir + which, rom)) return false;
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
    return true;
}
static void installCharRom(PetMachine& pet, const std::vector<uint8_t>& rom, bool upperBank = false) {
    g_char_rom1.assign(rom.begin(), rom.begin()+0x400);
    g_char_rom2.assign(rom.begin()+0x400, rom.begin()+0x800);
    pet.setVideoCharsets(g_char_rom1.data(),g_char_rom2.data());   // also drops any second bank from a previous set
    if (upperBank && rom.size() >= 0x1000) {                       // 4 KB chargen (8296-D): upper 2 KB, selected by CRTC R12 bit 5
        g_char_rom3.assign(rom.begin()+0x800, rom.begin()+0xC00);
        g_char_rom4.assign(rom.begin()+0xC00, rom.begin()+0x1000);
        pet.video().setCharsetsUpper(g_char_rom3.data(),g_char_rom4.data());
    }
}

// BASIC 2 loader (zimmers.net-native file names)
bool load_pet1_romset(PetMachine& pet, const std::string& dir)
{
	const char* names[] = {
		"rom-1-c000.901447-01.bin", "rom-1-c800.901447-02.bin",
		"rom-1-d000.901447-03.bin", "rom-1-d800.901447-04.bin",
		"rom-1-e000.901447-05.bin", "rom-1-f000.901447-06.bin",
		"rom-1-f800.901447-07.bin"
	};
	const uint16_t bases[] = {0xC000, 0xC800, 0xD000, 0xD800, 0xE000, 0xF000, 0xF800};
	std::vector<uint8_t> images[7], chars;
	// Validate the entire set before altering the current machine.
	for (int i = 0; i < 7; ++i)
		if (!readFile(dir + names[i], images[i]) || images[i].size() != 0x800) return false;
	if (!readCharRom(dir, chars, true)) return false;
	for (int i = 0; i < 7; ++i)
		if (!pet.loadRom(images[i].data(), images[i].size(), bases[i])) return false;
	// An earlier profile may have installed a 4 KB editor overlay.
	pet.bus().clearROM(0xE800, 0x800);
	installCharRom(pet, chars);
	LOG_INFO("ROMs installed: original PET 2001 BASIC 1 (901447), original character ROM");
	return true;
}

bool load_pet2_romset(PetMachine& pet, const std::string& dir, bool editorN)
{
	std::vector<uint8_t> basicC, basicD, editE, kernalF, chars;
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
	if (!(editE.size() == 0x0800 || editE.size() == 0x1000)) {
		LOG_ERROR("%s size=%zu (expected 2048 or 4096)", path_edit.c_str(), editE.size()); return false; }
	if (kernalF.size() != 0x1000) { LOG_ERROR("%s size=%zu (expected 4096) - not installed", path_kernal.c_str(), kernalF.size()); return false; }

	if (!readCharRom(dir, chars)) return false;
	if (!pet.loadRom(basicC.data(), std::min<size_t>(basicC.size(), 0x1000), 0xC000)) { LOG_ERROR("load BASIC C000 failed"); return false; }
	if (!pet.loadRom(basicD.data(), std::min<size_t>(basicD.size(), 0x1000), 0xD000)) { LOG_ERROR("load BASIC D000 failed"); return false; }
	if (!pet.loadRom(editE.data(), std::min<size_t>(editE.size(), 0x1000), 0xE000)) { LOG_ERROR("load EDIT E000 failed");  return false; }
	if (!pet.loadRom(kernalF.data(), std::min<size_t>(kernalF.size(), 0x1000), 0xF000)) { LOG_ERROR("load KERNAL F000 failed"); return false; }

	installCharRom(pet, chars);

	LOG_INFO("ROMs installed: BASIC@C000/D000, EDIT@E000, KERNAL@F000, CHARS(1KBx2)");
	return true;
}

// Load BASIC 4 (PET 40-col, "N" editor) split set by the filenames you provided.
// Maps: BASIC @ B000/C000/D000 (3x4KB), EDIT-4-N @ E000 (2KB), KERNAL-4 @ F000 (4KB).
bool load_pet4_romset(PetMachine& pet, const std::string& dir, bool crtc, bool hz50)
{
	std::vector<uint8_t> basB, basC, basD, editN, kernalF, chars;

	const std::string p_basB = dir + "basic-4-b000.901465-23.bin"; // 4096 bytes -> $B000
	const std::string p_basC = dir + "basic-4-c000.901465-20.bin"; // 4096 bytes -> $C000
	const std::string p_basD = dir + "basic-4-d000.901465-21.bin"; // 4096 bytes -> $D000
	const std::string p_edit = dir + (crtc ? (hz50 ? "edit-4-40-n-50Hz.901498-01.bin" : "edit-4-40-n-60Hz.901499-01.bin")
	                                       : "edit-4-n.901447-29.bin");     // 2048 bytes -> $E000
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
	if (!readCharRom(dir, chars)) return false;
	if (!pet.loadRom(basB.data(), std::min<size_t>(basB.size(), 0x1000), 0xB000)) { LOG_ERROR("load BASIC B000 failed"); return false; }
	if (!pet.loadRom(basC.data(), std::min<size_t>(basC.size(), 0x1000), 0xC000)) { LOG_ERROR("load BASIC C000 failed"); return false; }
	if (!pet.loadRom(basD.data(), std::min<size_t>(basD.size(), 0x1000), 0xD000)) { LOG_ERROR("load BASIC D000 failed"); return false; }
	if (!pet.loadRom(editN.data(), std::min<size_t>(editN.size(), 0x0800), 0xE000)) { LOG_ERROR("load EDIT E000 failed");  return false; }
	if (!pet.loadRom(kernalF.data(), std::min<size_t>(kernalF.size(), 0x1000), 0xF000)) { LOG_ERROR("load KERNAL F000 failed"); return false; }

	installCharRom(pet, chars);

	LOG_INFO("ROMs installed: BASIC4, 40-column %s editor, KERNAL4", crtc ? (hz50 ? "CRTC 50Hz" : "CRTC 60Hz") : "discrete video");
	return true;
}

// 8032: BASIC 4 B/C/D + 80-column business editor (60 Hz, or 50 Hz when hz50) + KERNAL 4.
// After install, flip the machine into 80-column mode (2 KB screen window,
// 80-col renderer default until the editor ROM programs the CRTC).
bool load_pet8032_romset(PetMachine& pet, const std::string& dir, bool hz50)
{
	std::vector<uint8_t> basB, basC, basD, edit80, kernalF, chars;
	if (!readFile(dir + "basic-4-b000.901465-19.bin", basB)) return false;
	if (!readFile(dir + "basic-4-c000.901465-20.bin", basC)) return false;
	if (!readFile(dir + "basic-4-d000.901465-21.bin", basD)) return false;
	if (!readFile(dir + (hz50 ? "edit-4-80-b-50Hz.901474-04-0283.bin" : "edit-4-80-b-60Hz.901474-03.bin"), edit80)) return false;
	if (!readFile(dir + "kernal-4.901465-22.bin", kernalF)) return false;
	if (basB.size() != 0x1000 || basC.size() != 0x1000 || basD.size() != 0x1000 ||
		edit80.size() != 0x0800 || kernalF.size() != 0x1000) {
		LOG_ERROR("[8032] ROM size mismatch - set not installed"); return false;
	}
	if (!readCharRom(dir, chars)) return false;
	if (!pet.loadRom(basB.data(), 0x1000, 0xB000)) return false;
	if (!pet.loadRom(basC.data(), 0x1000, 0xC000)) return false;
	if (!pet.loadRom(basD.data(), 0x1000, 0xD000)) return false;
	if (!pet.loadRom(edit80.data(), 0x0800, 0xE000)) return false;
	if (!pet.loadRom(kernalF.data(), 0x1000, 0xF000)) return false;
	installCharRom(pet, chars);
	LOG_INFO("ROMs installed: 8032 (BASIC4 B/C/D, EDIT-4-80-B %s, KERNAL-4)", hz50 ? "50Hz" : "60Hz");
	return true;
}

// 8296 (North American, 60 Hz): the 8032 business editor with BASIC 4 (B000 is the -23 revision),
// 2 KB character ROM. With hz50 the European 8296-D: 4 KB editor at $E000 (the I/O page keeps
// priority over $E800-$E8FF) and a 4 KB character ROM whose upper half is chosen by CRTC R12 bit 5.
// The 64K expansion and 4 KB screen are applied by the caller.
bool load_pet8296_romset(PetMachine& pet, const std::string& dir, bool hz50)
{
	std::vector<uint8_t> basB, basC, basD, edit, kernalF, chars;
	if (!readFile(dir + "basic-4-b000.901465-23.bin", basB)) return false;
	if (!readFile(dir + "basic-4-c000.901465-20.bin", basC)) return false;
	if (!readFile(dir + "basic-4-d000.901465-21.bin", basD)) return false;
	if (!readFile(dir + (hz50 ? "edit-50hz-324243-04.bin" : "edit-4-80-b-60Hz.901474-03.bin"), edit)) return false;
	if (!readFile(dir + "kernal-4.901465-22.bin", kernalF)) return false;
	if (!readFile(dir + (hz50 ? "characters-324242-01.bin" : "characters-2.901447-10.bin"), chars)) return false;
	if (basB.size() != 0x1000 || basC.size() != 0x1000 || basD.size() != 0x1000 ||
		edit.size() != (hz50 ? 0x1000u : 0x800u) || kernalF.size() != 0x1000 ||
		(hz50 ? chars.size() != 0x1000 : chars.size() < 0x800)) {
		LOG_ERROR("[8296] ROM size mismatch - set not installed"); return false;
	}
	if (!pet.loadRom(basB.data(), 0x1000, 0xB000)) return false;
	if (!pet.loadRom(basC.data(), 0x1000, 0xC000)) return false;
	if (!pet.loadRom(basD.data(), 0x1000, 0xD000)) return false;
	if (!pet.loadRom(edit.data(), edit.size(), 0xE000)) return false;
	if (!hz50) pet.bus().clearROM(0xE800, 0x800);   // a 4 KB editor from an earlier set must not leave stale bytes
	if (!pet.loadRom(kernalF.data(), 0x1000, 0xF000)) return false;
	installCharRom(pet, chars, hz50);
	LOG_INFO("ROMs installed: 8296 (BASIC4 B/C/D, %s, KERNAL-4)", hz50 ? "8296-D 50Hz editor, 4 KB character ROM" : "EDIT-4-80-B 60Hz");
	return true;
}

// The optional European editor ROM for the CRTC models (12, 8 and 96); model 96 needs
// its 4 KB editor and 4 KB character ROM both.
bool pet_editor50_present(int which, const std::string& dir)
{
	auto size = [&](const char* name) -> uintmax_t {
		std::error_code ec;
		const auto n = std::filesystem::file_size(std::filesystem::u8path(dir + name), ec);
		return ec ? 0 : static_cast<uintmax_t>(n);
	};
	if (which == 96) return size("edit-50hz-324243-04.bin") == 0x1000 && size("characters-324242-01.bin") == 0x1000;
	const char* name = which == 12 ? "edit-4-40-n-50Hz.901498-01.bin"
	                 : which == 8  ? "edit-4-80-b-50Hz.901474-04-0283.bin" : nullptr;
	if (!name) return false;
	return size(name) == 0x800;
}

// Same files and size rules as the loaders above, without reading or installing.
bool pet_romset_present(int which, const std::string& dir)
{
	auto size = [&](const char* name) -> uintmax_t {
		std::error_code ec;
		const auto n = std::filesystem::file_size(std::filesystem::u8path(dir + name), ec);
		return ec ? 0 : static_cast<uintmax_t>(n);
	};
	auto has = [&](const char* name, uintmax_t want) { return size(name) == want; };
	const bool charsOld = size("characters-1.901447-08.bin") >= 0x800;
	const bool charsNew = size("characters-2.901447-10.bin") >= 0x800;
	const bool charsAny = size("characters-2.901447-10.bin") ? charsNew : charsOld;
	switch (which) {
	case 1: {
		const char* names[] = { "rom-1-c000.901447-01.bin", "rom-1-c800.901447-02.bin", "rom-1-d000.901447-03.bin",
			"rom-1-d800.901447-04.bin", "rom-1-e000.901447-05.bin", "rom-1-f000.901447-06.bin", "rom-1-f800.901447-07.bin" };
		for (const char* n : names) if (!has(n, 0x800)) return false;
		return charsOld;
	}
	case 2: {
		const uintmax_t edit = size("edit-2-n.901447-24.bin");
		return has("basic-2-c000.901465-01.bin", 0x1000) && has("basic-2-d000.901465-02.bin", 0x1000) &&
			(edit == 0x800 || edit == 0x1000) && has("kernal-2.901465-03.bin", 0x1000) && charsAny;
	}
	case 4: case 12:
		return has("basic-4-b000.901465-23.bin", 0x1000) && has("basic-4-c000.901465-20.bin", 0x1000) &&
			has("basic-4-d000.901465-21.bin", 0x1000) && has("kernal-4.901465-22.bin", 0x1000) &&
			has(which == 12 ? "edit-4-40-n-60Hz.901499-01.bin" : "edit-4-n.901447-29.bin", 0x800) && charsAny;
	case 8:
		return has("basic-4-b000.901465-19.bin", 0x1000) && has("basic-4-c000.901465-20.bin", 0x1000) &&
			has("basic-4-d000.901465-21.bin", 0x1000) && has("edit-4-80-b-60Hz.901474-03.bin", 0x800) &&
			has("kernal-4.901465-22.bin", 0x1000) && charsAny;
	case 96:   // the 60 Hz North American set; the 8296-D files are optional
		return has("basic-4-b000.901465-23.bin", 0x1000) && has("basic-4-c000.901465-20.bin", 0x1000) &&
			has("basic-4-d000.901465-21.bin", 0x1000) && has("kernal-4.901465-22.bin", 0x1000) &&
			has("edit-4-80-b-60Hz.901474-03.bin", 0x800) && charsNew;
	}
	return false;
}
