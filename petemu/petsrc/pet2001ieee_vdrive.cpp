#include "pet2001ieee.h"
#include "ieee_helpers.h"

using namespace ieee_helpers;

// -----------------------------------------------------------------------------
// Folder backend: load PRG by name (case-insensitive, NAME or NAME.PRG)
// -----------------------------------------------------------------------------
// Layer: Layer 4 PRG loader (host folder)
bool PetIEEE::loadHostPRG_Folder(const std::string& petName,
	std::vector<uint8_t>& payload,
	uint16_t& loadAddr)
{
	if (hostRoot.empty()) return false;

	const std::string wantU = to_upper_ascii(petName);
	std::error_code ec;

	auto try_one = [&](const std::string& fn)->bool {
		std::vector<uint8_t> file;
		if (!read_all_file(std::filesystem::path(hostRoot) / fn, file)) return false;
		if (file.size() < 2) return false;
		loadAddr = (uint16_t)file[0] | ((uint16_t)file[1] << 8);
		payload.assign(file.begin() + 2, file.end());
		return true;
		};

	if (try_one(petName)) return true;
	if (try_one(petName + ".prg")) return true;

	for (auto it = std::filesystem::directory_iterator(hostRoot, ec);
		!ec && it != std::filesystem::end(it); ++it)
	{
		if (!it->is_regular_file()) continue;
		const std::string fn = it->path().filename().string();
		const std::string fu = to_upper_ascii(fn);
		// Exact, NAME.PRG, or CBM wildcard ("AB*" matches "ABC.PRG").
		if (fu == wantU || fu == (wantU + ".PRG") || wild_match(wantU, fu)) {
			return try_one(fn);
		}
	}
	return false;
}

// -----------------------------------------------------------------------------
// Folder backend: directory PRG
// -----------------------------------------------------------------------------
// Layer: Layer 4 (directory as tokenized BASIC)
bool PetIEEE::buildDirectoryPRG_Folder(std::vector<uint8_t>& out, uint16_t startAddr, const std::string& match) {
	if (hostRoot.empty()) return false;

	std::error_code ec;

	// Gather entries: blocks = ceil(size/254), name = UPPERCASE filename with a
	// trailing ".PRG" dropped (capped to 16, like a real disk), type = PRG.
	std::vector<DirEntry> entries;
	for (auto it = std::filesystem::directory_iterator(hostRoot, ec);
		!ec && it != std::filesystem::end(it); ++it)
	{
		if (!it->is_regular_file()) continue;

		std::string name = to_upper_ascii(it->path().filename().string());
		if (name.size() > 4 && name.compare(name.size() - 4, 4, ".PRG") == 0)
			name.resize(name.size() - 4);          // drop ".PRG" for display
		if (name.size() > 16) name.resize(16);

		// Optional CBM wildcard filter from "$:pattern" (empty = list all).
		if (!match.empty() && !wild_match(to_upper_ascii(match), name)) continue;

		std::error_code fe;
		const uintmax_t sz = std::filesystem::file_size(it->path(), fe);
		int blocks = 1;
		if (!fe && sz > 0) blocks = (int)((sz + 253) / 254);
		if (blocks < 1) blocks = 1;

		entries.push_back({ blocks, name, "PRG" });
	}

	std::sort(entries.begin(), entries.end(),
		[](const DirEntry& a, const DirEntry& b) { return a.name < b.name; });

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

	while (true) {
		if (!d64_read_sector(t, s, sec)) break;
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

	while (ct != 0) {
		if (!d64_read_sector(ct, cs, sec)) return false;
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
// PET-style patterns. For now this uses simple exact matches on the
// normalized token names and assumes .PRG extension, matching saveFile().
//
// Parameters:
//   tokens - list of normalized name patterns from CMD15 "S:".
//
// Returns:
//   Count of files successfully deleted.
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
		std::string fname = t;
		// Strip any ",P" or ",S" etc. if your normalize_cmd_name did not already.
		const size_t comma = fname.find(',');
		if (comma != std::string::npos)
			fname.erase(comma);

		// Match saveFile()/loadHostPRG_Folder(): files live under hostRoot as NAME.prg
		const std::string path = (std::filesystem::path(hostRoot) / (fname + ".prg")).string();

		LOG_DEBUG("SCRATCH(host): candidate \"%s\"", path.c_str());

		std::error_code ec;
		const bool existed = std::filesystem::exists(path, ec) && !ec;

		if (!existed) {
			LOG_WARN("SCRATCH(host): not found \"%s\"", path.c_str());
			continue;
		}

		if (std::filesystem::remove(path, ec) && !ec) {
			LOG_INFO("SCRATCH(host): deleted \"%s\"", path.c_str());
			++deleted;
		}
		else {
			LOG_WARN("SCRATCH(host): failed to delete \"%s\" ec=%d",
				path.c_str(), (int)ec.value());
		}
	}

	return deleted;
}

// -----------------------------------------------------------------------------
// SAVE side-effects: write to host folder
// -----------------------------------------------------------------------------
// Layer: Layer 4 SAVE convenience (not DOS-accurate)
void PetIEEE::saveFile(const std::string& fname, const std::string& contents)
{
	const std::string base = fname.empty() ? "UNTITLED" : fname;

	if (isD64Mounted()) {
		// Write raw bytes as PRG into the image
		std::vector<uint8_t> bytes(contents.begin(), contents.end());
		if (d64_save_prg(base, bytes)) {
			LOG_INFO("IEEE Save -> wrote %zu bytes to D64 image \"%s\" as \"%s\"",
				bytes.size(), d64Path.c_str(), base.c_str());
		}
		else {
			LOG_ERROR("IEEE Save -> failed writing \"%s\" to D64 image.", base.c_str());
		}
		return;
	}

	// Host-folder fallback (unchanged)
	const std::string outName = base + ".prg";
	if (hostRoot.empty()) {
		LOG_WARN("IEEE Save -> %s (%zu bytes). Host root not set; not writing.",
			outName.c_str(), contents.size());
		set_status(74, "DRIVE NOT READY", 0, 0);
		return;
	}

	std::filesystem::create_directories(hostRoot);
	std::vector<uint8_t> bytes(contents.begin(), contents.end());
	const auto outPath = std::filesystem::path(hostRoot) / outName;

	if (!write_all_file(outPath, bytes)) {
		LOG_ERROR("IEEE Save -> failed to write %s", outPath.string().c_str());
		set_status(25, "WRITE ERROR", 0, 0);
		return;
	}

	LOG_INFO("IEEE Save -> wrote %zu bytes to %s",
		bytes.size(), outPath.string().c_str());
	set_status(0, "OK", 0, 0);
}

// -----------------------------------------------------------------------------
// Host-folder RENAME (CMD15 "R:new=old") - PRG files are stored as NAME.prg.
// -----------------------------------------------------------------------------
bool PetIEEE::host_rename(const std::string& oldName, const std::string& newName)
{
	if (hostRoot.empty()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }
	const auto op = std::filesystem::path(hostRoot) / (oldName + ".prg");
	const auto np = std::filesystem::path(hostRoot) / (newName + ".prg");
	std::error_code ec;
	if (!std::filesystem::exists(op, ec)) { set_status(62, "FILE NOT FOUND", 0, 0); return false; }
	if (std::filesystem::exists(np, ec)) { set_status(63, "FILE EXISTS", 0, 0); return false; }
	std::filesystem::rename(op, np, ec);
	if (ec) { set_status(25, "WRITE ERROR", 0, 0); return false; }
	LOG_INFO("[IEEE] RENAME host \"%s\" -> \"%s\"", op.string().c_str(), np.string().c_str());
	set_status(0, "OK", 0, 0);
	return true;
}

// -----------------------------------------------------------------------------
// Host-folder COPY (CMD15 "C:new=old").
// -----------------------------------------------------------------------------
bool PetIEEE::host_copy(const std::string& srcName, const std::string& dstName)
{
	if (hostRoot.empty()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }
	const auto sp = std::filesystem::path(hostRoot) / (srcName + ".prg");
	const auto dp = std::filesystem::path(hostRoot) / (dstName + ".prg");
	std::error_code ec;
	if (!std::filesystem::exists(sp, ec)) { set_status(62, "FILE NOT FOUND", 0, 0); return false; }
	if (std::filesystem::exists(dp, ec)) { set_status(63, "FILE EXISTS", 0, 0); return false; }
	std::filesystem::copy_file(sp, dp, ec);
	if (ec) { set_status(25, "WRITE ERROR", 0, 0); return false; }
	LOG_INFO("[IEEE] COPY host \"%s\" -> \"%s\"", sp.string().c_str(), dp.string().c_str());
	set_status(0, "OK", 0, 0);
	return true;
}

//UNUSED, HERE IN CASE WE NEED IT

bool PetIEEE::scratchHostFile(const std::string& name)
{
	std::string path = "./files/" + name + ".prg";
	if (std::filesystem::exists(path)) {
		std::filesystem::remove(path);
		LOG_INFO("[IEEE] SCRATCH host file: \"%s\"", path.c_str());
		return true;
	}
	LOG_WARN("[IEEE] SCRATCH host file NOT FOUND: \"%s\"", path.c_str());
	return false;
}