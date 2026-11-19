#include "pet2001ieee.h"
#include "ieee_helpers.h"

using namespace ieee_helpers;

// -----------------------------------------------------------------------------
// Folder backend: what the drive "contains"
//
// One list serves "$", LOAD and OPEN so they always agree. Program files
// (.prg, or no extension) are PRG; data files written through the drive are
// NAME.seq (SEQ) and NAME.usr (USR). Anything else in the folder (.d64, .tap,
// notes...) is not part of the drive. Names show upper-case, cut to 16
// characters like a real disk; a 16-character name from the listing loads the
// file even when the host name is longer.
// -----------------------------------------------------------------------------
std::vector<PetIEEE::HostEntry> PetIEEE::folder_list() const
{
	std::vector<HostEntry> list;
	if (hostRoot.empty()) return list;

	std::error_code ec;
	for (auto it = std::filesystem::directory_iterator(std::filesystem::u8path(hostRoot), ec);
		!ec && it != std::filesystem::end(it); it.increment(ec))
	{
		std::error_code fe;
		if (!it->is_regular_file(fe) || fe) continue;

		const std::string fn = it->path().filename().u8string();
		if (fn.empty() || fn[0] == '.') continue;
		const std::string ext = to_upper_ascii(it->path().extension().u8string());

		HostEntry e;
		e.host = fn;
		if (ext == ".PRG" || ext.empty()) e.type = 2;
		else if (ext == ".SEQ") e.type = 1;
		else if (ext == ".USR") e.type = 3;
		else continue;

		e.stemUpper = to_upper_ascii(it->path().stem().u8string());
		if (e.stemUpper.empty()) continue;
		e.display = e.stemUpper;
		if (e.display.size() > 16) e.display.resize(16);

		const uintmax_t sz = std::filesystem::file_size(it->path(), fe);
		e.size = fe ? 0 : sz;
		list.push_back(e);
	}

	std::sort(list.begin(), list.end(), [](const HostEntry& a, const HostEntry& b) {
		if (a.display != b.display) return a.display < b.display;
		if (a.type != b.type) return a.type < b.type;
		return a.host < b.host;
	});
	return list;
}

bool PetIEEE::folder_find(const std::string& patU, int typeFilter, bool wildcard, HostEntry& out) const
{
	for (const HostEntry& e : folder_list()) {
		if (typeFilter && e.type != typeFilter) continue;
		const bool hit = wildcard
			// CBM wildcard on the listed name, or the full host name ("NAME.PRG").
			? (wild_match(patU, e.display) || e.stemUpper == patU || to_upper_ascii(e.host) == patU)
			: (e.display == patU || e.stemUpper == patU);
		if (hit) { out = e; return true; }
	}
	return false;
}

// -----------------------------------------------------------------------------
// Folder backend: load PRG by name (wildcards allowed, first listed match wins)
// -----------------------------------------------------------------------------
// Layer: Layer 4 PRG loader (host folder)
bool PetIEEE::loadHostPRG_Folder(const std::string& petName,
	std::vector<uint8_t>& payload,
	uint16_t& loadAddr)
{
	if (hostRoot.empty()) return false;

	HostEntry e;
	if (!folder_find(to_upper_ascii(petName), 2, true, e)) return false;

	std::vector<uint8_t> file;
	std::filesystem::path path;
	if (!host_file_path(hostRoot, e.host, path) || !read_all_file(path, file)) return false;
	if (file.size() < 2) return false;
	loadAddr = (uint16_t)file[0] | ((uint16_t)file[1] << 8);
	payload.assign(file.begin() + 2, file.end());
	return true;
}

// -----------------------------------------------------------------------------
// Folder backend: named data file reader for OPEN/INPUT#/GET#
// -----------------------------------------------------------------------------
// Layer: Layer 4 (named-file reader) opened via Layer 3 named channel
bool PetIEEE::openFolder_for_read(const std::string& rawName, uint8_t sa, uint8_t typeFilter)
{
	if (hostRoot.empty() || sa > 0x0F) return false;

	const std::string wantU = to_upper_ascii(strip_drive_prefix(rawName));
	HostEntry e;
	std::filesystem::path path;
	if (!folder_find(wantU, typeFilter, true, e) || !host_file_path(hostRoot, e.host, path)) {
		streams[sa].reset();
		return false;
	}

	// Not read_all_file(): an empty data file is valid and reads as end of file.
	std::ifstream f(path, std::ios::binary);
	if (!f) { streams[sa].reset(); return false; }
	std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

	streams[sa].data.swap(bytes);
	streams[sa].index = 0;
	streams[sa].name = wantU;
	return true;
}

// -----------------------------------------------------------------------------
// Folder backend: write a closed data file (SEQ/PRG/USR) to the host folder
// -----------------------------------------------------------------------------
// Layer: Layer 4 (file create/replace on the host folder)
bool PetIEEE::folder_write_file(const std::string& nameU, const std::vector<uint8_t>& bytes,
	uint8_t type, bool replace)
{
	if (hostRoot.empty()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }
	if (nameU.empty() || nameU.find_first_of("*?") != std::string::npos) {
		set_status(33, "SYNTAX ERROR", 0, 0); return false;
	}

	const uint8_t t = (uint8_t)(type & 0x0F);
	HostEntry old;
	const bool has = folder_find(nameU, 0, false, old);
	if (has && !replace) { set_status(63, "FILE EXISTS", 0, 0); return false; }

	// Same type: overwrite the existing host file (keeps its spelling). New
	// file or a type change: NAME.seq / NAME.prg / NAME.usr.
	const char* ext = (t == 3) ? ".usr" : (t == 2) ? ".prg" : ".seq";
	const std::string hostName = (has && old.type == t) ? old.host : nameU + ext;
	std::filesystem::path outPath;
	if (!host_file_path(hostRoot, hostName, outPath)) { set_status(33, "SYNTAX ERROR", 0, 0); return false; }

	std::error_code ec;
	std::filesystem::create_directories(std::filesystem::u8path(hostRoot), ec);
	if (ec || !write_all_file(outPath, bytes)) {
		LOG_ERROR("IEEE folder write failed: %s", outPath.u8string().c_str());
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}

	// Replaced under a different type: drop the old file so the name stays unique.
	if (has && old.type != t) {
		std::filesystem::path oldPath;
		if (host_file_path(hostRoot, old.host, oldPath)) std::filesystem::remove(oldPath, ec);
	}

	LOG_INFO("IEEE folder write -> %zu bytes to %s", bytes.size(), outPath.u8string().c_str());
	set_status(0, "OK", 0, 0);
	return true;
}

// -----------------------------------------------------------------------------
// Folder backend: directory PRG
// -----------------------------------------------------------------------------
// Layer: Layer 4 (directory as tokenized BASIC)
bool PetIEEE::buildDirectoryPRG_Folder(std::vector<uint8_t>& out, uint16_t startAddr, const std::string& match) {
	if (hostRoot.empty()) return false;

	// blocks = ceil(size/254), name as listed, type PRG/SEQ/USR from the extension.
	std::vector<DirEntry> entries;
	for (const HostEntry& he : folder_list()) {
		// Optional CBM wildcard filter from "$:pattern" (empty = list all).
		if (!match.empty() && !wild_match(to_upper_ascii(match), he.display)) continue;

		int blocks = 1;
		if (he.size > 0) blocks = (int)((he.size + 253) / 254);
		if (blocks < 1) blocks = 1;

		entries.push_back({ blocks, he.display, he.type == 1 ? "SEQ" : he.type == 3 ? "USR" : "PRG" });
	}

	// "Blocks free": host free space at the folder in 254-byte blocks, capped.
	int freeBlocks = 9999;
	std::error_code se;
	const auto sp = std::filesystem::space(hostRoot, se);
	if (!se) {
		const uintmax_t blk = sp.available / 254;
		freeBlocks = (blk > 65535) ? 65535 : (int)blk;
	}

	return buildDirectoryPRG_common(out, startAddr, "VDRIVE", "00", "2A", entries, freeBlocks);
}

// -----------------------------------------------------------------------------
// D64 PRG loader: Folder backend: load PRG by name (host folder)
// -----------------------------------------------------------------------------
// Layer: Layer 4 PRG loader (D64)
bool PetIEEE::loadHostPRG_D64(const std::string& petName,
	std::vector<uint8_t>& payload,
	uint16_t& loadAddr)
{
	if (!isD64Mounted()) return false;

	const std::string wantU = to_upper_ascii(petName);

	uint8_t sec[256];
	int t = 18, s = 1;
	int startT = 0, startS = 0;
	bool found = false;

	SectorVisits directoryVisited;
	while (true) {
		if (!directoryVisited.visit(t, s) || !d64_read_sector(t, s, sec)) return false;
		int nextT = sec[0];
		int nextS = sec[1];

		for (int i = 0; i < 8; ++i) {
			const uint8_t* e = sec + 2 + i * 32;
			uint8_t ftype = e[0];
			if ((ftype & 0x80) == 0) continue;        // unused
			if ((ftype & 0x0F) != 2) continue;        // require PRG (low nibble 2)
			std::string name = petscii_dirent_to_ascii(e + 3);
			const std::string upName = to_upper_ascii(name);
			// Exact, NAME.PRG, or CBM wildcard ("AB*" -> first match in dir order).
			if (upName == wantU || to_upper_ascii(name + ".PRG") == wantU || wild_match(wantU, upName)) {
				startT = e[1];
				startS = e[2];
				found = true;
				break;
			}
		}
		if (found) break;
		if (nextT == 0) break;
		t = nextT; s = nextS;
	}

	if (!found || startT == 0) return false;

	std::vector<uint8_t> file;
	int ct = startT, cs = startS;

	SectorVisits fileVisited;
	while (ct != 0) {
		if (!fileVisited.visit(ct, cs) || !d64_read_sector(ct, cs, sec)) return false;
		int nt = sec[0];
		int ns = sec[1];

		if (nt == 0) {
			// CBM-DOS: byte[1] is the offset of the last used byte, so the
			// data-byte count is ns-1 (a full final sector reads 0xFF -> 254).
			int used = (int)ns - 1;
			if (used < 0) used = 0;
			if (used > 254) used = 254;
			file.insert(file.end(), sec + 2, sec + 2 + used);
			break;
		}
		else {
			file.insert(file.end(), sec + 2, sec + 256);
			ct = nt; cs = ns;
		}
	}

	if (file.size() < 2) return false;

	loadAddr = (uint16_t)file[0] | ((uint16_t)file[1] << 8);
	payload.assign(file.begin() + 2, file.end());
	return true;
}

// -----------------------------------------------------------------------------
// scratch_host_patterns
// Delete files in the host "virtual drive" folder that match the given
// PET-style patterns (CBM wildcards allowed), whatever their data type.
//
// Parameters:
//   tokens - list of normalized name patterns from CMD15 "S:".
//
// Returns:
//   Count of files successfully deleted, or -1 for an invalid host path.
// -----------------------------------------------------------------------------
int PetIEEE::scratch_host_patterns(const std::vector<std::string>& tokens)
{
	int deleted = 0;

	if (hostRoot.empty()) {
		LOG_WARN("SCRATCH(host): no host root set; nothing to scratch");
		return 0;
	}

	for (const std::string& t : tokens) {
		// t is already normalized (uppercased, trimmed) by normalize_cmd_name().
		std::string pat = t;
		// Strip any ",P" or ",S" etc. if your normalize_cmd_name did not already.
		const size_t comma = pat.find(',');
		if (comma != std::string::npos)
			pat.erase(comma);

		// The virtual drive is flat: reject path syntax outright.
		if (pat.find_first_of("/\\:") != std::string::npos) {
			set_status(33, "SYNTAX ERROR", 0, 0);
			return -1;
		}

		for (const HostEntry& e : folder_list()) {
			if (!wild_match(pat, e.display) && e.stemUpper != pat) continue;

			std::filesystem::path checked;
			if (!host_file_path(hostRoot, e.host, checked)) continue;
			const std::string path = checked.u8string();
			LOG_DEBUG("SCRATCH(host): candidate \"%s\"", path.c_str());

			std::error_code ec;
			if (std::filesystem::remove(checked, ec) && !ec) {
				LOG_INFO("SCRATCH(host): deleted \"%s\"", path.c_str());
				++deleted;
			}
			else {
				LOG_WARN("SCRATCH(host): failed to delete \"%s\" ec=%d",
					path.c_str(), (int)ec.value());
			}
		}
	}

	return deleted;
}

// -----------------------------------------------------------------------------
// SAVE side-effects: write to host folder
// -----------------------------------------------------------------------------
// Layer: Layer 4 SAVE convenience (not DOS-accurate)
void PetIEEE::saveFile(const std::string& fname, const std::string& contents, bool replace)
{
	const std::string base = fname.empty() ? "UNTITLED" : fname;

	if (isD64Mounted()) {
		// Write raw bytes as PRG into the image
		std::vector<uint8_t> bytes(contents.begin(), contents.end());
		if (d64_save_prg(base, bytes, replace)) {
			LOG_INFO("IEEE Save -> wrote %zu bytes to D64 image \"%s\" as \"%s\"",
				bytes.size(), d64Path.c_str(), base.c_str());
		}
		else {
			LOG_ERROR("IEEE Save -> failed writing \"%s\" to D64 image.", base.c_str());
		}
		return;
	}

	// Host-folder fallback: NAME.prg (an existing program of that name is replaced in place)
	if (hostRoot.empty()) {
		LOG_WARN("IEEE Save -> %s.prg (%zu bytes). Host root not set; not writing.",
			base.c_str(), contents.size());
		set_status(74, "DRIVE NOT READY", 0, 0);
		return;
	}

	std::vector<uint8_t> bytes(contents.begin(), contents.end());
	folder_write_file(to_upper_ascii(base), bytes, 0x82, replace);
}

// -----------------------------------------------------------------------------
// Host-folder RENAME (CMD15 "R:new=old"). The file keeps its type extension.
// -----------------------------------------------------------------------------
bool PetIEEE::host_rename(const std::string& oldName, const std::string& newName)
{
	if (hostRoot.empty()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }
	HostEntry src;
	if (!folder_find(to_upper_ascii(oldName), 0, false, src)) { set_status(62, "FILE NOT FOUND", 0, 0); return false; }
	const char* ext = (src.type == 3) ? ".usr" : (src.type == 2) ? ".prg" : ".seq";
	std::filesystem::path op, np;
	if (!host_file_path(hostRoot, src.host, op) ||
		!host_file_path(hostRoot, newName + ext, np)) {
		set_status(33, "SYNTAX ERROR", 0, 0); return false;
	}
	HostEntry dummy;
	std::error_code ec;
	if (folder_find(to_upper_ascii(newName), 0, false, dummy) || std::filesystem::exists(np, ec)) {
		set_status(63, "FILE EXISTS", 0, 0); return false;
	}
	std::filesystem::rename(op, np, ec);
	if (ec) { set_status(25, "WRITE ERROR", 0, 0); return false; }
	LOG_INFO("[IEEE] RENAME host \"%s\" -> \"%s\"", op.u8string().c_str(), np.u8string().c_str());
	set_status(0, "OK", 0, 0);
	return true;
}

// -----------------------------------------------------------------------------
// Host-folder COPY (CMD15 "C:new=old"). The copy keeps the source's type.
// -----------------------------------------------------------------------------
bool PetIEEE::host_copy(const std::string& srcName, const std::string& dstName)
{
	if (hostRoot.empty()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }
	HostEntry src;
	if (!folder_find(to_upper_ascii(srcName), 0, false, src)) { set_status(62, "FILE NOT FOUND", 0, 0); return false; }
	const char* ext = (src.type == 3) ? ".usr" : (src.type == 2) ? ".prg" : ".seq";
	std::filesystem::path sp, dp;
	if (!host_file_path(hostRoot, src.host, sp) ||
		!host_file_path(hostRoot, dstName + ext, dp)) {
		set_status(33, "SYNTAX ERROR", 0, 0); return false;
	}
	HostEntry dummy;
	std::error_code ec;
	if (folder_find(to_upper_ascii(dstName), 0, false, dummy) || std::filesystem::exists(dp, ec)) {
		set_status(63, "FILE EXISTS", 0, 0); return false;
	}
	std::filesystem::copy_file(sp, dp, ec);
	if (ec) { set_status(25, "WRITE ERROR", 0, 0); return false; }
	LOG_INFO("[IEEE] COPY host \"%s\" -> \"%s\"", sp.u8string().c_str(), dp.u8string().c_str());
	set_status(0, "OK", 0, 0);
	return true;
}

//UNUSED, HERE IN CASE WE NEED IT

bool PetIEEE::scratchHostFile(const std::string& name)
{
	return scratch_host_patterns({name}) > 0;
}
