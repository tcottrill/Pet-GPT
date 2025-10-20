#include "pet2001ieee.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

// This was originally based on Thomas Skibo's code but has moved to a point where I consider it to be 
// it's own source, if you can call it that with AI generated.


// Local debug helper (file-scope, no header change)
static inline void ieee_trace_bus_snapshot(const char* tag,
	const PetIEEE* self,
	const char* extra = nullptr)
{
#if IEEE_TRACE
	// Show logical (pre-inversion) position if we know it, plus line levels.
	// Note: DIO holds "physical" bus level as stored (already inverted when device talks).
	LOG_DEBUG("%s BUS: DIO=%02X NDAC(in=%d,out=%d,level=%d) NRFD(in=%d,out=%d,level=%d) "
		"DAV(in=%d,out=%d,level=%d) ATN=%d EOI(in=%d,out=%d,level=%d)%s%s",
		tag,
		self->DIOin(),
		self->NDACin() ? 1 : 0,  // 'level' below will show combined in/out again
		/* out */ 0,             // We don't expose the raw 'o' directly; keep 'level' truthy
		self->NDACin() ? 1 : 0,
		self->NRFDin() ? 1 : 0,
		/* out */ 0,
		self->NRFDin() ? 1 : 0,
		self->DAVin() ? 1 : 0,
		/* out */ 0,
		self->DAVin() ? 1 : 0,
		/* ATN */ 0,
		self->EOIin() ? 1 : 0,
		/* out */ 0,
		self->EOIin() ? 1 : 0,
		(extra && *extra) ? " " : "",
		(extra && *extra) ? extra : "");
#else
	(void)tag; (void)self; (void)extra;
#endif
}

// -----------------------------------------------------------------------------
// Utility helpers
// -----------------------------------------------------------------------------
// Layer: misc utility
//More PET Directory Helpers

// -----------------------------------------------------------------------------
// count_bits_24
// Fast popcount for the 24 BAM bits stored in 3 bytes.
// -----------------------------------------------------------------------------
static int count_bits_24(const uint8_t bits[3])
{
	auto pop8 = [](uint8_t v) -> int {
		v = v - ((v >> 1) & 0x55);
		v = (v & 0x33) + ((v >> 2) & 0x33);
		return (int)((v + (v >> 4)) & 0x0F);
		};
	return pop8(bits[0]) + pop8(bits[1]) + pop8(bits[2]);
}

// -----------------------------------------------------------------------------
// d64_log_bam_sanity
// -----------------------------------------------------------------------------
void PetIEEE::d64_log_bam_sanity(const D64Catalog& cat) const
{
	for (int t = 1; t <= 35; ++t) {
		const D64BamTrack& bt = cat.bam[t];
		const int pop = count_bits_24(bt.bits);
		if ((int)bt.freeCount != pop) {
			LOG_ERROR("[D64] BAM MISMATCH t=%d freeCount=%u bits_pop=%d (bits=%02X %02X %02X)",
				t, (unsigned)bt.freeCount, pop,
				(unsigned)bt.bits[0], (unsigned)bt.bits[1], (unsigned)bt.bits[2]);
		}
		else {
			LOG_DEBUG("[D64] BAM OK t=%d free=%u bits=%02X %02X %02X",
				t, (unsigned)bt.freeCount,
				(unsigned)bt.bits[0], (unsigned)bt.bits[1], (unsigned)bt.bits[2]);
		}
	}
}

// ----------Debug helpers(hex / ASCII dumps) ----------
static inline char dbg_printable(uint8_t c) {
	return (c >= 32 && c <= 126) ? (char)c : '.';
}

// Dump raw bytes as hex into a std::string
static std::string dump_hex(const uint8_t* p, size_t n) {
	static const char* hexd = "0123456789ABCDEF";
	std::string out; out.reserve(n * 3);
	for (size_t i = 0; i < n; ++i) {
		uint8_t b = p[i];
		out.push_back(hexd[b >> 4]);
		out.push_back(hexd[b & 0xF]);
		if (i + 1 < n) out.push_back(' ');
	}
	return out;
}

// Dump raw bytes as ASCII with dots for non-printables
static std::string dump_ascii(const uint8_t* p, size_t n) {
	std::string out; out.reserve(n);
	for (size_t i = 0; i < n; ++i) out.push_back(dbg_printable(p[i]));
	return out;
}

// Dump std::string in hex
static std::string dump_hex_str(const std::string& s) {
	return dump_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Dump the 16-byte PETSCII dir name (raw hex + naive ASCII view)
static void log_dirent_name_hex_ascii(const uint8_t name16[16]) {
	std::string hex = dump_hex(name16, 16);
	std::string asc; asc.reserve(16);
	for (int i = 0; i < 16; ++i) asc.push_back(dbg_printable(name16[i]));
	LOG_DEBUG("DIR NAME raw16 hex=[%s] ascii='%s'", hex.c_str(), asc.c_str());
}

static std::string dir_name_to_ascii(const uint8_t* p16) {
	int len = 16;
	while (len > 0 && p16[len - 1] == 0xA0) len--;
	std::string s;
	s.reserve(len);
	for (int i = 0; i < len; ++i) s.push_back(p16[i]); // you already convert PETSCII elsewhere if needed
	return s;
}

static const char* base_type(uint8_t tcode) {
	switch (tcode & 0x0F) {
	case 1: return "SEQ";
	case 2: return "PRG";
	case 3: return "USR";
	case 4: return "REL";
	default: return "DEL";
	}
}

static std::string render_type_token(uint8_t ftype, uint8_t recLenIfREL) {
	bool closed = (ftype & 0x80) != 0;
	bool locked = (ftype & 0x40) != 0;
	const char* core = base_type(ftype);
	std::string out;

	// leading '*' if NOT closed
	if (!closed) out.push_back('*');
	out += core;
	// locked '<'
	if (locked) out.push_back('<');
	// REL record length
	if ((ftype & 0x0F) == 5) {
		char buf[8];
		std::snprintf(buf, sizeof(buf), ",%d", (int)recLenIfREL);
		out += buf;
	}
	return out;
}

static std::string fmt_blocks_3(int n) {
	char buf[8];
	std::snprintf(buf, sizeof(buf), "%3d", n);
	return std::string(buf);
}

std::string PetIEEE::to_upper_ascii(std::string s) {
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c; });
	return s;
}

// Layer: Layer 4 helper (turn PETSCII-style dir entry into readable ASCII)
std::string PetIEEE::petscii_dirent_to_ascii(const uint8_t* name16) {
	std::string out;
	out.reserve(16);
	for (int i = 0; i < 16; ++i) {
		uint8_t c = name16[i];
		if (c == 0xA0) c = ' ';               // pad -> space
		if (c >= 'a' && c <= 'z') c -= 32;    // to upper
		out.push_back((char)c);
	}
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
}

static bool read_all_file(const std::filesystem::path& p, std::vector<uint8_t>& out) {
	std::ifstream f(p, std::ios::binary);
	if (!f) return false;
	out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
	return !out.empty();
}

static bool write_all_file(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
	std::ofstream f(p, std::ios::binary);
	if (!f) return false;
	f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	return (bool)f;
}

// Remove quotes, drive prefix, and mode tokens (",S,R", ",S,W", etc.).
// Layer: Layer 4 name normalization for SEQ OPEN
static std::string normalize_open_name_for_seq(const std::string& raw)
{
	std::string s = raw;

	// Trim surrounding quotes
	if (!s.empty() && s.front() == '\"') s.erase(0, 1);
	if (!s.empty() && s.back() == '\"') s.pop_back();

	// Trim leading spaces (defensive)
	while (!s.empty() && s.front() == ' ') s.erase(0, 1);

	// Strip optional drive prefix:
	//   - canonical: "0:" or "1:"
	//   - lenient:   leading '0' or '1' with NO colon if next char is a letter (missing colon case)
	if (s.size() >= 2 && (s[0] == '0' || s[0] == '1')) {
		if (s[1] == ':') {
			s.erase(0, 2);
		}
		else {
			// If next char is alphabetic, treat it as missing-colon prefix "0NAME" -> "NAME"
			unsigned char n1 = (unsigned char)s[1];
			if ((n1 >= 'A' && n1 <= 'Z') || (n1 >= 'a' && n1 <= 'z')) {
				s.erase(0, 1);
			}
		}
	}

	// Truncate at first comma: discard mode tokens (",S,R", ",S,W", ",L", etc.)
	size_t cpos = s.find(',');
	if (cpos != std::string::npos) s.erase(cpos);

	// Uppercase ASCII
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char ch) {
			return (ch >= 'a' && ch <= 'z') ? char(ch - 32) : char(ch);
		});

	// Trim trailing spaces
	while (!s.empty() && s.back() == ' ') s.pop_back();

	return s;
}

// New helper: normalize PRG/LOAD names (quotes, drive prefix, commas).
// Preserves "$" and "$..." after optional "0:"/"1:" so directory detection works.
//
// Layer: Layer 4 name normalization for PRG/LOAD and "$" directory
std::string PetIEEE::normalize_name_for_prg(const std::string& raw)
{
	std::string s = raw;

	// Remove surrounding quotes
	if (!s.empty() && s.front() == '\"') s.erase(0, 1);
	if (!s.empty() && s.back() == '\"') s.pop_back();

	// Trim leading spaces
	while (!s.empty() && s.front() == ' ') s.erase(0, 1);

	// Optional drive prefix 0: or 1: or missing-colon variant when next is letter or '$'
	if (!s.empty() && (s[0] == '0' || s[0] == '1')) {
		if (s.size() >= 2 && s[1] == ':') {
			s.erase(0, 2);
		}
		else if (s.size() >= 2) {
			unsigned char n1 = (unsigned char)s[1];
			if ((n1 == '$') ||
				(n1 >= 'A' && n1 <= 'Z') || (n1 >= 'a' && n1 <= 'z')) {
				s.erase(0, 1);
			}
		}
	}

	// Truncate at first comma (mode suffixes discarded)
	size_t cpos = s.find(',');
	if (cpos != std::string::npos) s.erase(cpos);

	// Upper-case for lookup convenience
	std::transform(s.begin(), s.end(), s.begin(),
		[](unsigned char ch) {
			return (ch >= 'a' && ch <= 'z') ? char(ch - 32) : char(ch);
		});

	// Trim trailing spaces
	while (!s.empty() && s.back() == ' ') s.pop_back();

	return s;
}

// -----------------------------------------------------------------------------
// Constructor / Reset
// -----------------------------------------------------------------------------
// Layer: init
PetIEEE::PetIEEE() {
	reset();
}

// Layer: init/reset
void PetIEEE::reset() {
	state = STATE_IDLE;
	filename.clear();
	save_data.clear();
	oldRom = false;
	data_index = 0;

	// Layer 1/2 signal defaults (idle = true for open-collector "released")
	dio = 0x00;
	ndac_i = true; ndac_o = true;
	nrfd_i = true; nrfd_o = true;
	atn = true;
	dav_i = true; dav_o = true;
	srq = true;
	eoi_i = true; eoi_o = true;

	hostRoot.clear();
	d64Path.clear();
	d64.clear();

	// Channels
	last_listen_sa = 0xFF;
	for (auto& st : streams) st.reset();
	current_talk_sa = 0xFF;

	// Tiny built-in default PRG at $0401 (PRINT "HELLO")
	load_data = {
		0x01, 0x04,
		0x0a, 0x04, 0x64, 0x00, 0x8f, 0x20, 'H','E','L','L','O', 0x00,
		0x00, 0x00
	};
}

// -----------------------------------------------------------------------------
// Mount points
// -----------------------------------------------------------------------------
// Layer: host backend mgmt
void PetIEEE::setHostRoot(const std::string& dir) {
	hostRoot = dir;
}

// Layer: D64 backend mgmt
bool PetIEEE::setD64Image(const std::string& path) {
	d64Path = path;
	d64.clear();
	if (path.empty()) return true;
	if (!read_all_file(path, d64)) {
		LOG_ERROR("IEEE: failed to read D64 image: %s", path.c_str());
		return false;
	}
	// Standard 35-track D64 is 174848 bytes; accept other sizes but warn.
	if (d64.size() != 174848 && d64.size() != 175531) {
		LOG_WARN("IEEE: D64 size %zu (non-standard) loaded; proceeding in danger zone", d64.size());
	}

	const size_t n = d64.size();

	// Standard sizes:
	//  - 174,848  = 35-track, no error map
	//  - 175,531  = 35-track, 683 error bytes
	//  - 196,608  = 40-track, no error map (common "extended" images)
	//  - 197,376  = 40-track, 768 error bytes
	const bool is35 = (n == 174848 || n == 175531);
	const bool is40 = (n == 196608 || n == 197376);

	if (!is35 && !is40) {
		LOG_WARN("[IEEE] non-standard D64 size (%zu bytes). Reads/writes will still be attempted; "
			"BAM/dir math assumes 35 tracks, so use with care.", n);
	}

	return true;
}

// -----------------------------------------------------------------------------
// Preload data for LOAD
// -----------------------------------------------------------------------------
// Layer: Layer 4 (device prepares a PRG or stream for TALK)
void PetIEEE::ieeeLoadData(uint16_t addr, const std::vector<uint8_t>& bytes) {
	load_data.clear();
	load_data.push_back((uint8_t)(addr & 0xFF));
	load_data.push_back((uint8_t)((addr >> 8) & 0xFF));
	load_data.insert(load_data.end(), bytes.begin(), bytes.end());
	data_index = 0;
}

// -----------------------------------------------------------------------------
// Folder backend: directory PRG
// -----------------------------------------------------------------------------
// Layer: Layer 4 (directory as tokenized BASIC)
bool PetIEEE::buildDirectoryPRG_Folder(std::vector<uint8_t>& out, uint16_t startAddr) {
	if (hostRoot.empty()) return false;

	std::error_code ec;
	std::vector<std::string> names;
	for (auto it = std::filesystem::directory_iterator(hostRoot, ec);
		!ec && it != std::filesystem::end(it); ++it)
	{
		if (!it->is_regular_file()) continue;
		names.push_back(it->path().filename().string());
	}
	std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
		auto ua = a; auto ub = b;
		std::transform(ua.begin(), ua.end(), ua.begin(),
			[](unsigned char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c; });
		std::transform(ub.begin(), ub.end(), ub.begin(),
			[](unsigned char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c; });
		return ua < ub;
		});

	std::vector<uint8_t> body;
	uint16_t cur = startAddr;

	auto append_line = [&](uint16_t lineNo, const std::string& text) {
		std::vector<uint8_t> t(text.begin(), text.end());
		uint16_t next = (uint16_t)(cur + 2 + 2 + t.size() + 1);
		body.push_back((uint8_t)(next & 0xFF));
		body.push_back((uint8_t)(next >> 8));
		body.push_back((uint8_t)(lineNo & 0xFF));
		body.push_back((uint8_t)(lineNo >> 8));
		body.insert(body.end(), t.begin(), t.end());
		body.push_back(0x00);
		cur = next;
		};

	append_line(0, "0 \"VDRIVE\" 00");
	uint16_t ln = 10;
	for (const auto& n : names) {
		std::string u = to_upper_ascii(n);
		append_line(ln, "\"" + u + "\" PRG");
		ln = (uint16_t)(ln + 10);
	}

	body.push_back(0x00); body.push_back(0x00);

	out.clear();
	out.push_back((uint8_t)(startAddr & 0xFF));
	out.push_back((uint8_t)(startAddr >> 8));
	out.insert(out.end(), body.begin(), body.end());
	return true;
}

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
		if (fu == wantU || fu == (wantU + ".PRG")) {
			return try_one(fn);
		}
	}
	return false;
}

// -----------------------------------------------------------------------------
// D64 low-level helpers
// -----------------------------------------------------------------------------
// Layer: D64 fs helpers
int PetIEEE::d64_sectors_per_track(int track) {
	if (track < 1) return 0;
	if (track <= 17) return 21;
	if (track <= 24) return 19;
	if (track <= 30) return 18;
	if (track <= 35) return 17;
	return 0;
}

size_t PetIEEE::d64_sector_offset(int track, int sector) const {
	int total = 0;
	for (int t = 1; t < track; ++t) total += d64_sectors_per_track(t);
	return (size_t)((total + sector) * 256);
}

bool PetIEEE::d64_read_sector(int track, int sector, uint8_t* dst) const {
	const int spt = d64_sectors_per_track(track);
	if (spt == 0 || sector < 0 || sector >= spt) return false;
	size_t off = d64_sector_offset(track, sector);
	if (off + 256 > d64.size()) return false;
	std::copy(d64.begin() + off, d64.begin() + off + 256, dst);
	return true;
}

bool PetIEEE::d64_write_sector(int track, int sector, const uint8_t* src) {
	const int spt = d64_sectors_per_track(track);
	if (spt == 0 || sector < 0 || sector >= spt) return false;
	const size_t off = d64_sector_offset(track, sector);
	if (off + 256 > d64.size()) return false;
	std::copy(src, src + 256, d64.begin() + off);
	return true;
}

bool PetIEEE::d64_flush_image_to_disk() {
	if (d64Path.empty() || d64.empty()) return false;
	return write_all_file(std::filesystem::path(d64Path), d64);
}

bool PetIEEE::buildDirectoryPRG_D64(std::vector<uint8_t>& out, uint16_t startAddr) {
	if (!isD64Mounted()) return false;

	// ----- Read BAM (Track 18 / Sector 0) -----
	uint8_t bam[256];
	if (!d64_read_sector(18, 0, bam)) return false;

	// Disk name (strip trailing $A0; NO padding inside quotes)
	const std::string diskName = dir_name_to_ascii(&bam[0x90]); // 16 bytes, $A0 padded in image

	// Disk ID: 2 chars at $A2/$A3 -> uppercase alnum or space
	auto read_id_2 = [](const uint8_t* p) -> std::string {
		std::string s; s.reserve(2);
		for (int i = 0; i < 2; ++i) {
			uint8_t c = p[i];
			if (c == 0xA0) c = ' ';
			if (c >= 'a' && c <= 'z') c -= 32;
			if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) s.push_back((char)c);
			else s.push_back(' ');
		}
		return s;
		};

	auto read_dos_2 = [](const uint8_t* p) -> std::string {
		std::string s; s.reserve(2);
		for (int i = 0; i < 2; ++i) {
			uint8_t c = p[i];
			if (c >= 'a' && c <= 'z') c -= 32;           // uppercase
			if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) s.push_back((char)c);
			else s.push_back(' ');
		}
		if (s.size() < 2) s = "2A";
		if (s[0] == ' ' && s[1] == ' ') s = "2A";       // sensible default
		return s;
		};

	auto pad_trunc_16 = [](std::string s) {
		if (s.size() > 16) s.resize(16);
		else if (s.size() < 16) s.append(16 - s.size(), ' ');
		return s;
		};

	// Free blocks: BAM free count per track ($04 + 4*(track-1))
	int freeBlocks = 0;
	for (int track = 1; track <= 35; ++track) {
		const int off = 0x04 + (track - 1) * 4;
		if (off >= 256) break;
		freeBlocks += bam[off];
	}

	// ----- Compute blocks by walking T/S chain -----
	auto count_chain_blocks = [&](int startTrack, int startSector) -> int {
		if (startTrack <= 0) return 0;
		int count = 0;
		int t = startTrack, s = startSector;
		uint8_t buf[256];
		for (int guard = 0; guard < 10000; ++guard) {
			if (!d64_read_sector(t, s, buf)) break;
			++count;
			int nextT = buf[0];
			int nextS = buf[1];
			if (nextT == 0) break; // end of chain
			const int spt = d64_sectors_per_track(nextT);
			if (spt <= 0 || nextS < 0 || nextS >= spt) break; // invalid link
			t = nextT; s = nextS;
		}
		return count;
		};

	// Type token from CBM nibble + flags; no padding added here
	auto type_token = [](uint8_t ftype, uint8_t recLen) -> std::string {
		const uint8_t code = (uint8_t)(ftype & 0x0F);   // 0=DEL,1=SEQ,2=PRG,3=USR,4=REL
		const bool    closed = (ftype & 0x80) != 0;     // 1=closed
		const bool    locked = (ftype & 0x40) != 0;     // 1=locked
		const char* core = "DEL";
		switch (code) { case 1: core = "SEQ"; break; case 2: core = "PRG"; break; case 3: core = "USR"; break; case 4: core = "REL"; break; default: core = "DEL"; break; }
							  std::string out; if (!closed) out.push_back('*'); out += core; if (locked) out.push_back('<');
							  if (code == 4) { char b[8]; std::snprintf(b, sizeof(b), ",%d", (int)recLen); out += b; }
							  return out;
		};

	// Filename (quotes tight; no padding inside quotes)
	auto tight_name = [&](const uint8_t* p16) -> std::string {
		return dir_name_to_ascii(p16); // strips trailing $A0; do NOT pad
		};

	// ----- Build header visible content (NO leading "0 "; BASIC will print a single 0) -----
	const std::string id = read_id_2(&bam[0xA2]);   // e.g. "1A"
	const std::string dos = read_dos_2(&bam[0xA5]); // e.g. "2A"

	// Build header: "\"NAME(16)\"  " + ID + "  " + DOS
	std::string hdr_visible;
	hdr_visible.reserve(1 + diskName.size() + 1 + 2 + 2 + 2 + 2);
	hdr_visible += "\"";
	hdr_visible += pad_trunc_16(diskName);  // header keeps 16 chars inside quotes
	hdr_visible += "\" ";
	hdr_visible += id;
	hdr_visible += " ";
	const int type_col = (int)hdr_visible.size();   // start of DOS in header
	const int type_col_screen = type_col + 2;       // add the leading "0 " BASIC prints
	hdr_visible += dos;

	// Reverse video header + RVSOFF erase trick (KEEP EXACTLY AS YOU HAVE IT)
	const uint8_t RVSON = 0x12, RVSOFF = 0x92;
	std::vector<uint8_t> hdr_bytes;
	hdr_bytes.reserve(hdr_visible.size() + 2);
	hdr_bytes.push_back(RVSON);
	hdr_bytes.insert(hdr_bytes.end(), hdr_visible.begin(), hdr_visible.end());
	hdr_bytes.push_back(RVSOFF);
	hdr_bytes.push_back(0x14); // DELETE 'W'
	hdr_bytes.push_back(0x14); // DELETE 'A'
	hdr_bytes.push_back(0x14); // DELETE 'I'
	hdr_bytes.push_back(0x14); // DELETE 'T'

	// ----- Walk directory chain (18/1 → …) and build entry lines -----
	struct DirLine { int blocks; std::string text; };
	std::vector<DirLine> lines;

	uint8_t sec[256];
	int t = 18, s = 1;
	while (true) {
		if (!d64_read_sector(t, s, sec)) break;
		const int nextT = sec[0], nextS = sec[1];

		for (int i = 0; i < 8; ++i) {
			const uint8_t* e = sec + 2 + i * 32;

			const uint8_t ftype = e[0];
			const int     startTrack = e[1];
			const int     startSector = e[2];

			// Skip empty/deleted entries safely
			if (startTrack == 0) continue;
			if ((ftype & 0x07) == 0x00) continue; // DEL/empty type

			// Blocks with chain fallback
			const uint8_t raw_lo = e[0x1E];
			const uint8_t raw_hi = e[0x1F];
			const int blocks_dir = (int)raw_lo | ((int)raw_hi << 8);
			const int blocks_chain = count_chain_blocks(startTrack, startSector);
			const int MAX_SANE_BLOCKS = 1000;
			const int blocks = (blocks_dir == 0 || blocks_dir > MAX_SANE_BLOCKS) ? blocks_chain : blocks_dir;

			// Blocks: right-aligned width 5, then ONE space
			char bfield[8];
			std::snprintf(bfield, sizeof(bfield), "%5d", blocks);

			// Visible name: tight (no in-quote padding), but cap to 16 (D64 limit)
			std::string name = tight_name(e + 3);
			if ((int)name.size() > 16) name.resize(16);

			std::string line;
			line.reserve(5 + 1 + 1 + (int)name.size() + 2 + 8);
			line += bfield;
			line += ' ';      // one space after blocks
			line += '"';
			line += name;     // no padding inside quotes
			line += '"';
			line += "  ";     // two spaces after closing quote

			// Align TYPE to fixed column
			int target_col = type_col_screen + 3;  // keep your current anchor
			int cur_len = (int)line.size();
			if (cur_len < target_col) line.append(target_col - cur_len, ' ');

			// Type token: REL record length is at 0x1C (not 0x1E)
			const uint8_t recLen = e[0x1C];
			line += type_token(ftype, recLen);

			lines.push_back({ blocks, line });
		}

		if (nextT == 0) break;
		t = nextT; s = nextS;
	}

	// ----- Tokenized program body with PETSCII deletes to erase BASIC's line numbers (non-header) -----
	std::vector<uint8_t> body;
	uint16_t cur = startAddr;

	auto append_basic_line = [&](uint16_t lineNo, const std::vector<uint8_t>& bytes) {
		const uint16_t next = (uint16_t)(cur + 2 + 2 + (uint16_t)bytes.size() + 1);
		body.push_back((uint8_t)(next & 0xFF));
		body.push_back((uint8_t)(next >> 8));
		body.push_back((uint8_t)(lineNo & 0xFF));
		body.push_back((uint8_t)(lineNo >> 8));
		body.insert(body.end(), bytes.begin(), bytes.end());
		body.push_back(0x00);
		cur = next;
		};

	// Header: DO NOT erase the printed "0 " — we want one visible zero.
	auto append_header = [&](const std::vector<uint8_t>& hdr) {
		append_basic_line(0, hdr);
		};

	// Entries/footer: erase the printed line number (digits + trailing space), then text
	auto append_entry = [&](uint16_t lineNo, const std::string& text, int extraEatCols) {
		int digits = (lineNo < 10) ? 1 :
			(lineNo < 100) ? 2 :
			(lineNo < 1000) ? 3 : 4;
		int dels = digits + 1 + extraEatCols;   // left shift via extra deletes

		std::vector<uint8_t> bytes;
		bytes.reserve(dels + text.size());
		for (int i = 0; i < dels; ++i) bytes.push_back(0x14); // $14 = DELETE
		bytes.insert(bytes.end(), text.begin(), text.end());

		append_basic_line(lineNo, bytes);
		};

	// Emit header (line 0)
	append_header(hdr_bytes);

	// Entries: keep your existing left-shift knob
	uint16_t ln = 10;
	for (const auto& L : lines) {
		append_entry(ln, L.text, 3);
		ln = (uint16_t)(ln + 10);
	}

	// Footer: no extra shift
	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%d BLOCKS FREE.", freeBlocks);
		append_entry(ln, buf, 0);
	}

	// Program terminator
	body.push_back(0x00); body.push_back(0x00);

	// Prepend PRG load address
	out.clear();
	out.push_back((uint8_t)(startAddr & 0xFF));
	out.push_back((uint8_t)(startAddr >> 8));
	out.insert(out.end(), body.begin(), body.end());
	return true;
}

// -----------------------------------------------------------------------------
// D64 PRG loader
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
			if (to_upper_ascii(name) == wantU || to_upper_ascii(name + ".PRG") == wantU) {
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
			int used = ns;
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
// Strip optional drive "0:" prefix
// -----------------------------------------------------------------------------
// Layer: Layer 4 helper
std::string PetIEEE::strip_drive_prefix(const std::string& n) {
	if (n.size() >= 2 && n[1] == ':' && (n[0] == '0' || n[0] == '1')) {
		return n.substr(2);
	}
	return n;
}

// -----------------------------------------------------------------------------
// Open a SEQ file from D64 for reading on a secondary address
// -----------------------------------------------------------------------------
// Layer: Layer 4 (SEQ reader) opened via Layer 3 named channel
bool PetIEEE::openD64SEQ_for_read(const std::string& rawName, uint8_t sa)
{
	if (!isD64Mounted()) return false;
	if (sa > 0x0F) return false;

	const std::string petName = to_upper_ascii(strip_drive_prefix(rawName));
	const std::string wantU = petName;

	uint8_t sec[256];
	int t = 18, s = 1;
	int startT = 0, startS = 0;
	bool found = false;

	while (true) {
		if (!d64_read_sector(t, s, sec)) break;
		const int nextT = sec[0];
		const int nextS = sec[1];

		for (int i = 0; i < 8; ++i) {
			const uint8_t* e = sec + 2 + i * 32;
			const uint8_t ftype = e[0];
			if ((ftype & 0x80) == 0) continue; // unused
			if ((ftype & 0x0F) != 1) continue; // SEQ only
			std::string name = petscii_dirent_to_ascii(e + 3);
			if (to_upper_ascii(name) == wantU) {
				startT = e[1];
				startS = e[2];
				found = true;
				break;
			}
		}
		if (found || nextT == 0) break;
		t = nextT; s = nextS;
	}

	if (!found || startT == 0) {
		// No SEQ found by that name
		streams[sa].reset();
		return false;
	}

	std::vector<uint8_t> seq;
	int ct = startT, cs = startS;

	while (ct != 0) {
		if (!d64_read_sector(ct, cs, sec)) return false;
		const int nt = sec[0];
		const int ns = sec[1];

		if (nt == 0) {
			int used = ns;
			if (used < 0) used = 0;
			if (used > 254) used = 254;
			seq.insert(seq.end(), sec + 2, sec + 2 + used);
			break;
		}
		else {
			seq.insert(seq.end(), sec + 2, sec + 256);
			ct = nt; cs = ns;
		}
	}

	streams[sa].data.swap(seq);
	streams[sa].index = 0;
	streams[sa].name = wantU;
	return true;
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
		return;
	}

	std::filesystem::create_directories(hostRoot);
	std::vector<uint8_t> bytes(contents.begin(), contents.end());
	const auto outPath = std::filesystem::path(hostRoot) / outName;

	if (!write_all_file(outPath, bytes)) {
		LOG_ERROR("IEEE Save -> failed to write %s", outPath.string().c_str());
		return;
	}

	LOG_INFO("IEEE Save -> wrote %zu bytes to %s",
		bytes.size(), outPath.string().c_str());
}

// -----------------------------------------------------------------------------
// Data bus
// -----------------------------------------------------------------------------
// Layer: 1/2 DIO line model
void PetIEEE::DIOout(uint8_t d8)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE DIOout: drive bus=%02X", d8);
#endif
	dio = d8;
}

uint8_t PetIEEE::DIOin() const
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE DIOin: read bus=%02X", dio);
#endif
	return dio;
}

// -----------------------------------------------------------------------------
// NDAC
// -----------------------------------------------------------------------------
// Layer: 1/2 NDAC line model
bool PetIEEE::NDACin() const { return ndac_i && ndac_o; }

void PetIEEE::NDACout(bool flag)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE NDACout: %d -> %d (device)", ndac_o ? 1 : 0, flag ? 1 : 0);
#endif
	bool rising = (!last_ndac_ && flag);
	last_ndac_ = flag;
	ndac_o = flag;

	// Ignore while ATN low (command phase)
	if (!atn) return;

	if (rising) {
		hs_debug("NDAC rise");
		if (state == STATE_LOAD && hs_ == HS_WAIT_NDAC_H) {
			// Finish current byte
			dav_i = true;
			eoi_i = true;
			if (data_index < load_data.size()) {
				data_index++;
			}
			LOG_DEBUG("HS NDAC rise: byte accepted, idx now %zu of %zu", (size_t)data_index, load_data.size());

			if (data_index < load_data.size()) {
				if (nrfd_o) {
					LOG_DEBUG("HS NDAC fast-path: NRFD already high -> present next");
					hs_present_byte_();
					hs_ = HS_WAIT_NDAC_H;
					hs_debug("NDAC fast-path -> WAIT_NDAC_H");
				}
				else {
					hs_ = HS_WAIT_NRFD_H;
					hs_debug("NDAC rise -> WAIT_NRFD_H");
				}
			}
			else {
				hs_ = HS_IDLE;
				hs_debug("NDAC rise -> IDLE (out of data)");
			}
		}
	}
}

// -----------------------------------------------------------------------------
// NRFD
// -----------------------------------------------------------------------------
// Layer: 1/2 NRFD line model
bool PetIEEE::NRFDin() const { return nrfd_i && nrfd_o; }

void PetIEEE::NRFDout(bool flag)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE NRFDout: %d -> %d (device)", nrfd_o ? 1 : 0, flag ? 1 : 0);
#endif
	bool rising = (!last_nrfd_ && flag);
	last_nrfd_ = flag;
	nrfd_o = flag;

	// Ignore data handshaking while ATN is low (command phase)
	if (!atn) return;

	if (rising) {
		hs_debug("NRFD rise");
		if (state == STATE_LOAD && hs_ == HS_WAIT_NRFD_H && data_index < load_data.size()) {
			hs_present_byte_();
			hs_ = HS_WAIT_NDAC_H;
			hs_debug("NRFD rise -> WAIT_NDAC_H");
		}
	}

	// Level-help: if waiting for ready and NRFD is already high, present now
	if (state == STATE_LOAD && hs_ == HS_WAIT_NRFD_H && nrfd_o && data_index < load_data.size()) {
		LOG_DEBUG("HS NRFD level-help: NRFD high while waiting -> present byte");
		hs_present_byte_();
		hs_ = HS_WAIT_NDAC_H;
		hs_debug("NRFD level-help -> WAIT_NDAC_H");
	}
}

// -----------------------------------------------------------------------------
// EOI
// -----------------------------------------------------------------------------
// Layer: 1/2 EOI line model
bool PetIEEE::EOIin() const { return eoi_i && eoi_o; }
void PetIEEE::EOIout(bool flag)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE EOIout: %d -> %d (device)", eoi_o ? 1 : 0, flag ? 1 : 0);
#endif
	eoi_o = flag;
}

// -----------------------------------------------------------------------------
// ATN
// -----------------------------------------------------------------------------
// Layer: 1/2 ATN control and addressing phase bookends
void PetIEEE::ATNout(bool flag)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE ATNout: %d -> %d (controller)", atn ? 1 : 0, flag ? 1 : 0);
#endif
	if (atn && !flag) {
		// ATN falling: controller entering command/addressing phase
		if (state == STATE_LOAD) {
			// Abort any in-flight data handshake
			dav_i = true;
			eoi_i = true;
			hs_ = HS_IDLE;
		}
		ndac_i = false;
		hs_debug("ATN fall");
	}
	else if (!atn && flag) {
		// ATN rising: end of addressing
		if (state == STATE_LOAD) {
			// Prepare for data phase
			dav_i = true;
			eoi_i = true;
			hs_ = HS_WAIT_NRFD_H;
			// sync edge latches so we don't wait for edges that already occurred
			last_nrfd_ = nrfd_o;
			last_ndac_ = ndac_o;
			// If listener is already ready (NRFD high), prime first byte
			if (nrfd_o && data_index < load_data.size()) {
				hs_present_byte_();
				hs_ = HS_WAIT_NDAC_H;
			}
			hs_debug("ATN rise (arm)");
		}
		else {
			hs_ = HS_IDLE;
			hs_debug("ATN rise (idle)");
		}
	}
	atn = flag;
}

// -----------------------------------------------------------------------------
// DAV
// -----------------------------------------------------------------------------
// Layer: 1/2 controller's DAV handling; device peeks command bytes while ATN low
void PetIEEE::DAVout(bool flag)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE DAVout: %d -> %d (controller)", dav_o ? 1 : 0, flag ? 1 : 0);
#endif
	if (dav_o && !flag) {
		// falling edge: controller put a byte on DIO (during ATN low)
		ndac_i = true;
		nrfd_i = false;
		const uint8_t d = (uint8_t)(dio ^ 0xFF); // capture logical command
#if IEEE_TRACE
		LOG_DEBUG("IEEE DAV falling: capture cmd=%02X (from bus=%02X)", d, dio);
#endif
		dataIn(d);
		ieee_trace_bus_snapshot("SNAP DAV-DN", this);
	}
	else if (!dav_o && flag) {
		// rising edge
		ndac_i = false;
		nrfd_i = true;
#if IEEE_TRACE
		LOG_DEBUG("IEEE DAV rising: cmd phase complete");
		ieee_trace_bus_snapshot("SNAP DAV-UP", this);
#endif
	}
	dav_o = flag;
}

bool PetIEEE::DAVin() const { return dav_i && dav_o; }

// -----------------------------------------------------------------------------
// SRQ
// -----------------------------------------------------------------------------
// Layer: 1/2 SRQ is not used by PET KERNAL; just expose level
bool PetIEEE::SRQin() const { return srq; }

// -----------------------------------------------------------------------------
// Main state machine for incoming bytes (addressing/data phases)
// -----------------------------------------------------------------------------
// Layer: 3 (TALK/LISTEN/SECOND/OPEN/CLOSE) + Layer 4 (DOS behaviors)
void PetIEEE::dataIn(uint8_t d8)
{
#if IEEE_TRACE
	LOG_DEBUG("IEEE dataIn: byte=%02X ATN=%d state=%d", d8, atn ? 1 : 0, (int)state);
#endif

	if (!atn) {
		// -----------------------
		// Address/command phase (controller drives ATN low)
		// -----------------------
		switch (state) {
		case STATE_IDLE:
			if (d8 == (uint8_t)(0x20 + MY_ADDRESS)) {
				// LISTEN primary to our device
				state = STATE_LISTEN;
				last_listen_sa = 0xFF;
			}
			else if (d8 == (uint8_t)(0x40 + MY_ADDRESS)) {
				// TALK primary from our device (LOAD/TALK path)
				current_talk_sa = 0xFF;
			}
			else if (d8 == 0x5F) { // UNTALK
				state = STATE_IDLE;
				current_talk_sa = 0xFF;
			}
			else if (d8 == 0x3F) { // UNLISTEN
				// Do NOT commit SEQ here; BASIC UNLISTENs between bursts.
				// If we were receiving command text on SA 15, execute now.
				if (!cmd15_buf_.empty()) {
					process_command_channel_string(cmd15_buf_);
					cmd15_buf_.clear();
				}
				state = STATE_IDLE;
			}
			else if (d8 >= 0x60 && d8 <= 0x6F) {
				// TALK-secondary: choose the SA to serve when in TALK (handled below)
				current_talk_sa = (uint8_t)(d8 & 0x0F);
				tracef("TALK SA=%u", current_talk_sa);

				if (current_talk_sa <= 0x0F && streams[current_talk_sa].active()) {
					// Serve remaining bytes from this SA
					load_data.assign(
						streams[current_talk_sa].data.begin() + streams[current_talk_sa].index,
						streams[current_talk_sa].data.end());
					data_index = 0;
					if (!load_data.empty()) {
						trace_hex_preview("SEQ serve preview", load_data.data(), load_data.size());
					}
					state = STATE_LOAD;
					// Arm handshake FSM
					dav_i = true; eoi_i = true; hs_ = HS_WAIT_NRFD_H;
					last_nrfd_ = nrfd_o; last_ndac_ = ndac_o;
					hs_debug("ARM");
				}
				else {
					// Fall back to single-shot LOAD by filename if available
					if (!filename.empty()) {
						std::vector<uint8_t> pay;
						uint16_t addr = 0x0801;
						const std::string nameClean = normalize_name_for_prg(filename);

						if (!nameClean.empty() && nameClean[0] == '$') {
							std::vector<uint8_t> dirprg;
							bool ok = false;
							if (isD64Mounted()) ok = buildDirectoryPRG_D64(dirprg, 0x0401);
							else                ok = buildDirectoryPRG_Folder(dirprg, 0x0401);

							if (ok && dirprg.size() >= 2) {
								addr = (uint16_t)dirprg[0] | ((uint16_t)dirprg[1] << 8);
								pay.assign(dirprg.begin() + 2, dirprg.end());
								ieeeLoadData(addr, pay);
								state = STATE_LOAD;
								dav_i = true; eoi_i = true; hs_ = HS_WAIT_NRFD_H;
								last_nrfd_ = nrfd_o; last_ndac_ = ndac_o;
								hs_debug("ARM");
							}
						}
						else if (!nameClean.empty()) {
							bool ok = false;
							if (isD64Mounted()) ok = loadHostPRG_D64(nameClean, pay, addr);
							else                ok = loadHostPRG_Folder(nameClean, pay, addr);

							if (ok) {
								ieeeLoadData(addr, pay);
								state = STATE_LOAD;
								dav_i = true; eoi_i = true; hs_ = HS_WAIT_NRFD_H;
								last_nrfd_ = nrfd_o; last_ndac_ = ndac_o;
								hs_debug("ARM");
							}
						}
					}
				}
			}
			else if (d8 == 0x7F && !filename.empty() && load_data.size() > 2) {
				// Old PET ROM LOAD
				data_index = (load_data[0] == 0) ? 2 : 1;
				dio = (uint8_t)(load_data[data_index] ^ 0xFF);
				dav_i = false;
				oldRom = true;
				state = STATE_LOAD;
				dav_i = true; eoi_i = true; hs_ = HS_WAIT_NRFD_H;
				last_nrfd_ = nrfd_o; last_ndac_ = ndac_o;
				hs_debug("ARM");
			}
			else if (d8 == 0x3F && !filename.empty()) {
				// Old PET ROM SAVE
				oldRom = true;
				save_data.clear();
				state = STATE_SAVE1;
			}
			break;

		case STATE_LISTEN:
			if (d8 == 0x3F) { // UNLISTEN
				// Execute command channel text if we were receiving it
				if (!cmd15_buf_.empty()) {
					process_command_channel_string(cmd15_buf_);
					cmd15_buf_.clear();
				}
				// Keep SEQ open; commit happens on CLOSE SA.
				state = STATE_IDLE;
			}
			else if (d8 >= 0x60 && d8 <= 0x6F) {
				// LISTEN-secondary (data channel select)
				last_listen_sa = (uint8_t)(d8 & 0x0F);
				tracef("LISTEN SA=%u (data channel)", last_listen_sa);
				// stay in STATE_LISTEN; data bytes come with ATN high
			}
			else if (d8 >= 0xE0 && d8 <= 0xEF) {
				// CLOSE CHANNEL via secondary (CLOSE #sa)
				const uint8_t sa = (uint8_t)(d8 & 0x0F);
				tracef("CLOSE SA=%u (via 0xE0..0xEF)", sa);
				// If any pending command text for 15, process now:
				if (sa == 15 && !cmd15_buf_.empty()) {
					process_command_channel_string(cmd15_buf_);
					cmd15_buf_.clear();
				}
				(void)close_seq_write_channel(sa);   // commit SEQ if armed
				streams[sa].reset();                  // close any read stream bound to this SA
				// remain in STATE_LISTEN until UNLISTEN arrives
			}
			else if (d8 >= 0xF0 && d8 <= 0xFF) {
				// OPEN/CLOSE text transfer (CLOSE if empty name)
				last_listen_sa = (uint8_t)(d8 & 0x0F);
				filename.clear();
				data_index = 0;
				state = STATE_FNAME;
				tracef("LISTEN SA=%u (await OPEN/CLOSE text)", last_listen_sa);
			}
			else if (d8 == 0x61) {
				// Legacy SAVE (host-folder)
				save_data.clear();
				state = STATE_SAVE;
			}
			break;

		case STATE_FNAME:
			if (d8 == 0x3F) { // UNLISTEN ends OPEN/CLOSE text
				const uint8_t sa = last_listen_sa;

				// Command channel (15): execute command immediately.
				if (sa == 15) {
					LOG_DEBUG("CMD15 FNAME finalize: hex=[%s] text='%s'",
						dump_hex_str(filename).c_str(), filename.c_str());
					if (!filename.empty()) process_command_channel_string(filename);
					filename.clear(); state = STATE_IDLE; break;
				}

				// SA 0/1 = LOAD/SAVE command channels. Keep name for LOAD fallback.
				if (sa == 0 || sa == 1) {
					tracef("FNAME finalize on SA=%u (LOAD/SAVE path) name=\"%s\"", sa, filename.c_str());
					state = STATE_IDLE;
					break;
				}

				if (filename.empty()) {
					// CLOSE channel -> commit SEQ writer if armed on this SA
					if (sa <= 0x0F) {
						(void)close_seq_write_channel(sa);
						streams[sa].reset();
						tracef("CLOSE channel SA=%u (empty-name)", sa);
					}
					state = STATE_IDLE;
					break;
				}

				// OPEN on SA: detect ",S,W" and arm SEQ writer
				tracef("OPEN SA=%u \"%s\"", sa, filename.c_str());
				std::string up = to_upper_ascii(filename);
				if (up.find(",S,W") != std::string::npos) {
					open_seq_write_channel(sa, filename);
					filename.clear();
					state = STATE_IDLE;
					break;
				}

				// Else try SEQ READ on this SA (if mounted)
				if (isD64Mounted() && sa <= 0x0F) {
					const std::string clean = normalize_open_name_for_seq(filename);
					tracef("OPEN normalized: \"%s\"", clean.c_str());
					if (openD64SEQ_for_read(clean, sa)) {
						const auto& st = streams[sa];
						tracef("SEQ armed SA=%u, bytes=%zu", sa, st.data.size());
						if (!st.data.empty()) {
							const size_t off = 0;
							const size_t n = st.data.size() - off;
							trace_hex_preview("SEQ arm preview", st.data.data() + off, n);
						}
					}
					else {
						tracef("SEQ not found SA=%u \"%s\"", sa, filename.c_str());
					}
				}

				filename.clear();
				state = STATE_IDLE;
			}
			break;

		case STATE_LOAD:
			if (d8 == 0x5F) { // UNTALK
				if (current_talk_sa <= 0x0F && streams[current_talk_sa].active()) {
					streams[current_talk_sa].index += data_index;
					if (streams[current_talk_sa].index > streams[current_talk_sa].data.size())
						streams[current_talk_sa].index = streams[current_talk_sa].data.size();
					tracef("UNTALK SA=%u consumed=%zu total=%zu at=%zu",
						current_talk_sa, (size_t)data_index,
						streams[current_talk_sa].data.size(),
						streams[current_talk_sa].index);
				}
				state = STATE_IDLE;
				current_talk_sa = 0xFF;
			}
			break;

		case STATE_SAVE:
			if (d8 == 0x3F) { // UNLISTEN -> legacy SAVE complete
				saveFile(filename, save_data);
				state = STATE_IDLE;
			}
			break;

		case STATE_SAVE1:
			if (eoi_o) {
				save_data.push_back((char)d8);
				data_index++;
			}
			else {
				// insert $0004 load address header
				save_data.insert(0, 1, 0x00);
				save_data.insert(1, 1, 0x04);
				saveFile(filename, save_data);
				oldRom = false;
				state = STATE_IDLE;
			}
			break;
		}
	}
	else {
		// -----------------------
		// Data phase (ATN high) — controller streams bytes
		// -----------------------
		switch (state) {
		case STATE_FNAME:
			// OPEN/CLOSE text bytes (terminated by UNLISTEN in address phase)
			filename.push_back((char)d8);
			break;

		case STATE_LISTEN:
			// PRINT# data bytes for the currently selected LISTEN SA
			if (last_listen_sa <= 15) {
				const uint8_t sa = last_listen_sa;
				if (sa == 15) {
					// Command channel: accumulate text until UNLISTEN
					cmd15_buf_.push_back((char)d8);
					LOG_DEBUG("CMD15 byte: %02X '%c'  buf_size=%zu",
						d8, dbg_printable(d8), cmd15_buf_.size());
				}
				else {
					// SEQ,S,W: latch-style append into that SA buffer
					accept_byte_for_possible_seq_write(sa, d8);
				}
			}
			break;

		case STATE_SAVE:
			// Legacy SAVE data (host-folder). Also mirror into SEQ buffer if armed on this SA.
			save_data.push_back((char)d8);
			data_index++;
			if (last_listen_sa <= 15) {
				const uint8_t sa = last_listen_sa;
				if (sa == 15) {
					cmd15_buf_.push_back((char)d8);
				}
				else {
					accept_byte_for_possible_seq_write(sa, d8);
				}
			}
			break;

		default:
			break;
		}
	}
}

bool PetIEEE::d64_parse_bam(D64Catalog& out)
{
	uint8_t bam[256];
	if (!d64_read_sector(18, 0, bam)) return false;

	out.bam.clear();
	out.bam.resize(36); // index by track; we support 1..35

	for (int t = 1; t <= 35; ++t) {
		const int off = 4 + (t - 1) * 4;
		D64BamTrack bt{};
		bt.freeCount = bam[off + 0];
		bt.bits[0] = bam[off + 1];
		bt.bits[1] = bam[off + 2];
		bt.bits[2] = bam[off + 3];
		out.bam[t] = bt;
	}

	out.dirFirstT = bam[0x00] ? bam[0x00] : 18;
	out.dirFirstS = bam[0x01] ? bam[0x01] : 1;
	out.is40 = false;
	return true;
}

bool PetIEEE::d64_load_dir_sector(uint8_t t, uint8_t s, std::array<uint8_t, 256>& sec)
{
	return d64_read_sector(t, s, sec.data());
}

bool PetIEEE::d64_write_dir_sector(uint8_t t, uint8_t s, const std::array<uint8_t, 256>& sec)
{
	return d64_write_sector(t, s, sec.data());
}

// --- FIXED: parse directory sectors with correct offsets (base = 2 + i*32) ---
// -----------------------------------------------------------------------------
// Parse directory into D64Catalog (correct offsets: base = 0x20 * i)
// -----------------------------------------------------------------------------
bool PetIEEE::d64_parse_directory(D64Catalog& out)
{
	out.entries.clear();

	uint8_t t = out.dirFirstT;
	uint8_t s = out.dirFirstS;
	while (t != 0) {
		std::array<uint8_t, 256> sec{};
		if (!d64_load_dir_sector(t, s, sec)) return false;

		uint8_t nextT = sec[0x00];
		uint8_t nextS = sec[0x01];

		for (int i = 0; i < 8; ++i) {
			const int base = 0x20 * i;
			uint8_t ftype = sec[base + 0x02];
			if (ftype == 0x00) continue; // empty

			D64DirEntry e{};
			e.fileType = ftype;
			e.startT = sec[base + 0x03];
			e.startS = sec[base + 0x04];

			// 16-char name (A0 padded) -> ASCII spaces, stripped right
			for (int j = 0; j < 16; ++j) {
				uint8_t c = sec[base + 0x05 + j];
				if (c == 0xA0) c = ' ';
				e.name[j] = (char)c;
			}
			e.name[16] = 0;
			while (e.name[0] && e.name[strlen(e.name) - 1] == ' ')
				e.name[strlen(e.name) - 1] = 0;

			e.sizeSectors = uint16_t(sec[base + 0x1E]) | (uint16_t(sec[base + 0x1F]) << 8);
			e.dirT = t; e.dirS = s; e.slot = (uint8_t)i;

			out.entries.push_back(e);
		}

		t = nextT;
		s = nextS;
	}
	return true;
}

static inline bool test_bit_24(const uint8_t bits[3], int bitIndex /*0..23*/)
{
	const int b = bitIndex >> 3;
	const int m = bitIndex & 7;
	return (bits[b] >> m) & 1;
}
static inline void set_bit_24(uint8_t bits[3], int bitIndex, bool val)
{
	const int b = bitIndex >> 3;
	const int m = bitIndex & 7;
	if (val) bits[b] |= (uint8_t)(1u << m);
	else     bits[b] &= (uint8_t)~(1u << m);
}

bool PetIEEE::d64_bam_is_free(const D64Catalog& cat, int track, int sector) const
{
	if (track <= 0 || track >= (int)cat.bam.size()) return false;
	const D64BamTrack& bt = cat.bam[track];
	if (sector < 0 || sector >= 24) return false;
	return test_bit_24(bt.bits, sector);
}

void PetIEEE::d64_bam_set_used(D64Catalog& cat, int track, int sector, bool used)
{
	if (track <= 0 || track >= (int)cat.bam.size()) return;
	D64BamTrack& bt = cat.bam[track];
	set_bit_24(bt.bits, sector, !used); // 1=free, 0=used

	int cnt = 0;
	const int maxS = d64_sectors_per_track(track);
	for (int s = 0; s < maxS; ++s) if (test_bit_24(bt.bits, s)) ++cnt;
	bt.freeCount = (uint8_t)cnt;
}

bool PetIEEE::d64_write_bam_sector(const D64Catalog& cat)
{
	uint8_t bam[256];
	if (!d64_read_sector(18, 0, bam)) return false;

	for (int t = 1; t <= 35; ++t) {
		const int off = 4 + (t - 1) * 4;
		bam[off + 0] = cat.bam[t].freeCount;
		bam[off + 1] = cat.bam[t].bits[0];
		bam[off + 2] = cat.bam[t].bits[1];
		bam[off + 3] = cat.bam[t].bits[2];
	}
	return d64_write_sector(18, 0, bam);
}

bool PetIEEE::d64_find_dir_entry(const D64Catalog& cat, const std::string& petNameUpper, int& idx)
{
	idx = -1;
	for (size_t i = 0; i < cat.entries.size(); ++i) {
		std::string a(cat.entries[i].name);
		while (!a.empty() && a.back() == ' ') a.pop_back();
		if (to_upper_ascii(a) == to_upper_ascii(petNameUpper)) { idx = (int)i; return true; }
	}
	return false;
}

// --- FIXED: scratch a single directory entry at its true offsets ---
// --- FIXED: scratch a single directory entry at its true offsets ---
bool PetIEEE::d64_scratch_dir_entry(D64Catalog& cat, int idx)
{
	if (idx < 0 || idx >= (int)cat.entries.size()) return false;
	const auto& e = cat.entries[idx];

	std::array<uint8_t, 256> sec{};
	if (!d64_load_dir_sector(e.dirT, e.dirS, sec)) return false;

	const int base = 0x20 * e.slot;  // correct entry start

	// mark unused and clear fields like CBM-DOS
	sec[base + 0x02] = 0x00;            // file type -> scratched
	sec[base + 0x03] = 0x00;            // start T
	sec[base + 0x04] = 0x00;            // start S
	for (int j = 0; j < 16; ++j) sec[base + 0x05 + j] = 0xA0; // name -> A0 padded
	sec[base + 0x1E] = 0x00;            // blocks lo
	sec[base + 0x1F] = 0x00;            // blocks hi

	return d64_write_dir_sector(e.dirT, e.dirS, sec);
}

bool PetIEEE::d64_find_last_dir_sector(const D64Catalog& cat, uint8_t& outT, uint8_t& outS)
{
	uint8_t t = cat.dirFirstT ? cat.dirFirstT : 18;
	uint8_t s = cat.dirFirstS ? cat.dirFirstS : 1;

	if (t == 0) { outT = 18; outS = 1; return true; }

	while (true) {
		std::array<uint8_t, 256> sec{};
		if (!d64_load_dir_sector(t, s, sec)) return false;
		uint8_t nextT = sec[0x00];
		uint8_t nextS = sec[0x01];
		if (nextT == 0) { outT = t; outS = s; return true; }
		t = nextT; s = nextS;
	}
}

bool PetIEEE::d64_alloc_next_dir_sector_on_track18(
	D64Catalog& cat,
	uint8_t prevT, uint8_t prevS,
	uint8_t& newT, uint8_t& newS)
{
	if (prevT != 18) return false;

	const int track = 18;
	const int maxS = d64_sectors_per_track(track);
	if (maxS <= 1) return false;

	int start = (prevS + D64_DIR_INTERLEAVE) % maxS;
	if (start == 0) start = 1;

	int candidate = start;
	for (int tries = 0; tries < maxS - 1; ++tries) {
		if (candidate != 0 && d64_bam_is_free(cat, track, candidate)) {
			// mark used in BAM in-memory only
			d64_bam_set_used(cat, track, candidate, true);

			// write blank dir sector with 00 FF link
			std::array<uint8_t, 256> blank{};
			blank[0] = 0x00; blank[1] = 0xFF;
			if (!d64_write_dir_sector((uint8_t)track, (uint8_t)candidate, blank)) return false;

			// patch previous sector's link
			std::array<uint8_t, 256> prev{};
			if (!d64_load_dir_sector(prevT, prevS, prev)) return false;
			prev[0x00] = (uint8_t)track;
			prev[0x01] = (uint8_t)candidate;
			if (!d64_write_dir_sector(prevT, prevS, prev)) return false;

			// do NOT write BAM here; caller will commit once at end
			newT = (uint8_t)track; newS = (uint8_t)candidate;
			return true;
		}
		candidate = (candidate + D64_DIR_INTERLEAVE) % maxS;
		if (candidate == 0) candidate = 1;
	}
	return false;
}


// --- FIXED: append (or place) a directory entry using the correct layout ---
// -----------------------------------------------------------------------------
// Append a new directory entry (correct offsets: base = 0x20 * i)
// -----------------------------------------------------------------------------
bool PetIEEE::d64_append_dir_entry(D64Catalog& cat, const D64DirEntry& src, int& outIndex)
{
	// Walk chain for free slot (start at dir head, or 18/1)
	uint8_t t = 18, s = 1;
	if (cat.dirFirstT) { t = cat.dirFirstT; s = cat.dirFirstS; }

	while (true) {
		std::array<uint8_t, 256> sec{};
		if (!d64_load_dir_sector(t, s, sec)) return false;

		// try to find free slot
		for (int i = 0; i < 8; ++i) {
			const int base = 0x20 * i;
			if (sec[base + 0x02] == 0x00) {
				// write entry (caller already chose fileType with closed bit as needed)
				sec[base + 0x02] = (uint8_t)(src.fileType | 0x80); // ensure closed=1
				sec[base + 0x03] = src.startT;
				sec[base + 0x04] = src.startS;

				auto pn = to_pet_upper_a0_padded_16(src.name);
				for (int j = 0; j < 16; ++j)
					sec[base + 0x05 + j] = (uint8_t)pn[j];

				sec[base + 0x1E] = (uint8_t)(src.sizeSectors & 0xFF);
				sec[base + 0x1F] = (uint8_t)((src.sizeSectors >> 8) & 0xFF);

				if (!d64_write_dir_sector(t, s, sec)) return false;

				D64DirEntry e = src; e.dirT = t; e.dirS = s; e.slot = (uint8_t)i;
				cat.entries.push_back(e);
				outIndex = (int)cat.entries.size() - 1;
				return true;
			}
		}

		// follow link
		const uint8_t nextT = sec[0x00];
		const uint8_t nextS = sec[0x01];
		if (nextT == 0) {
			// need to grow dir on track 18
			uint8_t tailT = t, tailS = s, newT = 0, newS = 0;
			if (!d64_alloc_next_dir_sector_on_track18(cat, tailT, tailS, newT, newS)) return false;

			// write into slot 0 of the new sector
			std::array<uint8_t, 256> newSec{};
			if (!d64_load_dir_sector(newT, newS, newSec)) return false;

			const int base = 0x20 * 0;
			newSec[base + 0x02] = (uint8_t)(src.fileType | 0x80);
			newSec[base + 0x03] = src.startT;
			newSec[base + 0x04] = src.startS;

			auto pn = to_pet_upper_a0_padded_16(src.name);
			for (int j = 0; j < 16; ++j)
				newSec[base + 0x05 + j] = (uint8_t)pn[j];

			newSec[base + 0x1E] = (uint8_t)(src.sizeSectors & 0xFF);
			newSec[base + 0x1F] = (uint8_t)((src.sizeSectors >> 8) & 0xFF);

			if (!d64_write_dir_sector(newT, newS, newSec)) return false;

			D64DirEntry e = src; e.dirT = newT; e.dirS = newS; e.slot = 0;
			cat.entries.push_back(e);
			outIndex = (int)cat.entries.size() - 1;
			return true;
		}

		t = nextT; s = nextS;
	}
}

// -----------------------------------------------------------------------------
// Allocate a free data block, preferring 'preferredTrack' and using 'interleave'.
// Skips Track 18 (DOS/BAM/Directory).
// -----------------------------------------------------------------------------
bool PetIEEE::d64_alloc_block(D64Catalog& cat, int preferredTrack, int interleave, D64Block& out)
{
	int t = preferredTrack;
	if (t < 1 || t > 35) t = 17;
	if (t == 18) t = 17; // never start on DOS track

	auto try_track = [&](int tt) -> bool {
		if (tt == 18) return false; // skip DOS track
		const int ms = d64_sectors_per_track(tt);
		for (int offset = 0; offset < ms; ++offset) {
			int s = (offset * interleave) % ms;
			if (d64_bam_is_free(cat, tt, s)) {
				d64_bam_set_used(cat, tt, s, true);
				out.track = (uint8_t)tt;
				out.sector = (uint8_t)s;
				return true;
			}
		}
		return false;
		};

	if (try_track(t)) return true;

	// Spiral search outward from preferred track
	for (int rad = 1; rad <= 35; ++rad) {
		for (int sign = -1; sign <= 1; sign += 2) {
			int tt = t + sign * rad;
			if (tt < 1 || tt > 35) continue;
			if (try_track(tt)) return true;
		}
	}
	return false; // no space
}

bool PetIEEE::d64_free_block(D64Catalog& cat, int track, int sector)
{
	if (track <= 0) return false;
	d64_bam_set_used(cat, track, sector, false);
	return true;
}

// -----------------------------------------------------------------------------
// Write a file's T/S chain to the image using BAM allocation.
// - type: 0x82 PRG, 0x81 SEQ, 0x83 USR
// - petNameUpper: already uppercased ASCII; 16-char limit is enforced later
// - Correct last-sector "used bytes" semantics: byte[1] = data-bytes (0..254)
// -----------------------------------------------------------------------------
bool PetIEEE::d64_write_file_chain(
	D64Catalog& cat,
	uint8_t type,
	const std::string& petNameUpper,
	const uint8_t* data,
	size_t len,
	D64DirEntry& outEntry)
{
	// first block near track 17
	D64Block first{};
	if (!d64_alloc_block(cat, 17, D64_FILE_INTERLEAVE, first)) return false;

	D64Block cur = first;
	uint16_t writtenSectors = 0;
	size_t pos = 0;

	while (true) {
		uint8_t buf[256] = { 0 };
		const size_t room = 254;
		const size_t remaining = (len > pos) ? (len - pos) : 0;
		const size_t chunk = (remaining > room) ? room : remaining;

		D64Block next{};
		const bool last = (pos + chunk) == len;

		if (!last) {
			// allocate next block on/near this track using interleave
			if (!d64_alloc_block(cat, cur.track, D64_FILE_INTERLEAVE, next)) return false;
			buf[0] = next.track;
			buf[1] = next.sector;
		}
		else {
			// end of chain
			buf[0] = 0x00;
			buf[1] = (uint8_t)chunk; // bytes used in last sector (0..254)
		}

		if (chunk) std::memcpy(&buf[2], data + pos, chunk);
		if (!d64_write_sector(cur.track, cur.sector, buf)) return false;

		++writtenSectors;
		pos += chunk;

		if (last) break;
		cur = next;
	}

	// fill out directory entry (CLOSED bit will be set when writing the slot)
	D64DirEntry ent{};
	ent.fileType = type;
	std::snprintf(ent.name, sizeof(ent.name), "%s", petNameUpper.c_str());
	ent.startT = first.track;
	ent.startS = first.sector;
	ent.sizeSectors = writtenSectors;

	outEntry = ent;

	// do NOT write BAM here; caller will commit once at the end
	return true;
}


std::string PetIEEE::to_pet_upper_a0_padded_16(const std::string& ascii)
{
	std::string up = to_upper_ascii(ascii);
	if (up.size() > 16) up.resize(16);
	std::string out(16, char(0xA0));
	for (size_t i = 0; i < up.size() && i < 16; ++i) {
		unsigned char c = (unsigned char)up[i];
		if (c < 0x20) c = 0x20;
		out[i] = (char)c;
	}
	return out;
}

// -----------------------------------------------------------------------------
// Save a file (PRG/SEQ/USR) into the mounted D64.
// - If a file with the same PET name exists, free its blocks first, then scratch
//   the directory entry (simple, CBM-like overwrite without splat).
// - Writes full chain first, then appends a closed directory entry.
// -----------------------------------------------------------------------------
bool PetIEEE::d64_save_file(const std::string& petName,
	const uint8_t* data, size_t len, uint8_t type)
{
	if (!isD64Mounted()) return false;

	// Build catalog snapshot
	D64Catalog cat{};
	if (!d64_parse_bam(cat)) return false;
	if (!d64_parse_directory(cat)) return false;

	const std::string petUp = to_upper_ascii(petName);

	// Detect existing entry but DO NOT delete it yet
	int existingIdx = -1;
	const bool hasExisting = d64_find_dir_entry(cat, petUp, existingIdx);

	// 1) Allocate and write the NEW chain first (in-memory BAM updates only)
	D64DirEntry newEnt{};
	if (!d64_write_file_chain(cat, type, petUp, data, len, newEnt)) {
		// Allocation or data write failed; keep original file untouched.
		return false;
	}

	// 2) If an old file exists, free its chain in-memory and scratch its dir slot
	if (hasExisting && existingIdx >= 0) {
		const auto& e = cat.entries[existingIdx];
		if (e.startT) {
			if (!d64_free_chain(cat, e.startT, e.startS)) return false; // in-memory only
		}
		if (!d64_scratch_dir_entry(cat, existingIdx)) return false;     // directory sector write only
	}

	// 3) Append the new directory entry (may grow dir; still no BAM write yet)
	int newIdx = -1;
	if (!d64_append_dir_entry(cat, newEnt, newIdx)) {
		// We could attempt to free the just-written new chain here,
		// but safest is to abort before BAM write – nothing committed yet.
		return false;
	}

	// 4) Sanity log and single commit
	d64_log_bam_sanity(cat);

	if (!d64_write_bam_sector(cat)) return false;
	if (!d64_flush_image_to_disk()) return false;

	return true;
}



bool PetIEEE::d64_save_prg(const std::string& petName, const std::vector<uint8_t>& bytes)
{
	return d64_save_file(petName, bytes.data(), bytes.size(), 0x82);
}

// -----------------------------------------------------------------------------
// Free (reclaim) an existing file's entire T/S chain in the BAM.
// Safe to call with t=0 (no-op).
// -----------------------------------------------------------------------------
bool PetIEEE::d64_free_chain(D64Catalog& cat, uint8_t t, uint8_t s)
{
	uint8_t sec[256];
	for (int guard = 0; guard < 10000 && t != 0; ++guard) {
		if (!d64_read_sector(t, s, sec)) break;
		const uint8_t nt = sec[0];
		const uint8_t ns = sec[1];

		// mark free in BAM (in-memory only)
		d64_free_block(cat, t, s);

		// optional scrub (keep commented unless you want tools to see cleared links)
		// sec[0] = 0; sec[1] = 0;
		// d64_write_sector(t, s, sec);

		t = nt;
		s = ns;
	}
	// do NOT write BAM here; caller will commit once at the end
	return true;
}

// --- SEQ,S,W: open on SA ----------------------------------------------------
bool PetIEEE::open_seq_write_channel(uint8_t sa, const std::string& rawName)
{
	if (!isD64Mounted()) {
		LOG_WARN("SEQ,S,W open ignored: no D64 mounted");
		return false;
	}

	sa &= 0x0F;

	// Normalize (quotes, 0:, mode suffixes) and upper-case to match your lookups.
	const std::string name = normalize_open_name_for_seq(rawName);
	if (name.empty()) {
		LOG_WARN("SEQ,S,W open: empty normalized name from '%s'", rawName.c_str());
		return false;
	}

	// Reset per-SA buffers and mark channel active.
	seq_write_buf_[sa].clear();
	seq_write_name_[sa] = name;
	seq_write_active_[sa] = true;

	// This SA is not a talk stream; keep read-stream state isolated.
	streams[sa].reset();

	LOG_DEBUG("SEQ,S,W open: SA=%u name='%s'", (unsigned)sa, name.c_str());
	return true;
}

// --- SEQ,S,W: feed a received byte into SA buffer ----------------------------
void PetIEEE::accept_byte_for_possible_seq_write(uint8_t sa, uint8_t b)
{
	sa &= 0x0F;
	if (!seq_write_active_[sa]) {
		// Not an active SEQ,S,W channel; ignore silently.
		return;
	}
	seq_write_buf_[sa].push_back(b);
}

// --- SEQ,S,W: close/commit SA to the mounted D64 -----------------------------
bool PetIEEE::close_seq_write_channel(uint8_t sa)
{
	sa &= 0x0F;
	if (!seq_write_active_[sa]) {
		// Nothing to do for this SA.
		return false;
	}

	const std::string& fname = seq_write_name_[sa];
	const auto& bytes = seq_write_buf_[sa];

	LOG_INFO("SEQ,S,W commit: SA=%u name='%s' bytes=%zu",
		(unsigned)sa, fname.c_str(), bytes.size());

	// 0x81 = SEQ (d64_save_file will set the CLOSED bit and update BAM/dir)
	const bool ok = d64_save_file(fname,
		bytes.empty() ? nullptr : bytes.data(),
		bytes.size(),
		0x81);

	if (!ok) {
		LOG_ERROR("SEQ,S,W commit failed for '%s' (%zu bytes)", fname.c_str(), bytes.size());
	}

	// Tear down channel state regardless of success (matches 1541 semantics).
	seq_write_active_[sa] = false;
	seq_write_name_[sa].clear();
	seq_write_buf_[sa].clear();

	return ok;
}

// --------------------------- D64 SCRATCH SUPPORT -----------------------------

// Normalize one scratch name token:
// - strips surrounding quotes
// - drops optional leading drive "0:" or "1:"
// - uppercases
// - trims spaces and trailing CR/LF
// - leaves '*' and '?' intact (wildcards)
static std::string normalize_cmd_name(std::string s)
{
	// Surrounding quotes
	if (!s.empty() && s.front() == '\"') s.erase(0, 1);
	if (!s.empty() && s.back() == '\"') s.pop_back();

	// Trim leading spaces
	while (!s.empty() && s.front() == ' ')
		s.erase(0, 1);

	// Optional drive prefix "0:" or "1:"
	if (s.size() >= 2 && (s[0] == '0' || s[0] == '1') && s[1] == ':')
		s.erase(0, 2);

	// Trim trailing spaces and CR/LF
	while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n'))
		s.pop_back();

	// Uppercase ASCII A–Z
	for (auto& ch : s) {
		if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
	}

	return s;
}

// Simple wildcard matcher supporting '*' (any) and '?' (single)
static bool wild_match(const std::string& pat, const std::string& name)
{
	size_t pi = 0, ni = 0, star = std::string::npos, mark = 0;
	while (ni < name.size()) {
		if (pi < pat.size() && (pat[pi] == '?' || pat[pi] == name[ni])) {
			++pi; ++ni;
		}
		else if (pi < pat.size() && pat[pi] == '*') {
			star = ++pi; mark = ni;
		}
		else if (star != std::string::npos) {
			pi = star; ni = ++mark;
		}
		else {
			return false;
		}
	}
	while (pi < pat.size() && pat[pi] == '*') ++pi;
	return pi == pat.size();
}

// -----------------------------------------------------------------------------
// d64_scratch_patterns
// Scratch (delete) all directory entries that match any of the patterns.
// Uses catalog-based BAM updates and writes BAM once at the end.
// Flushes the host image only once at the very end.
// -----------------------------------------------------------------------------
int PetIEEE::d64_scratch_patterns(const std::vector<std::string>& patterns)
{
	if (!isD64Mounted()) {
		LOG_WARN("[D64] SCRATCH: no D64 mounted");
		return 0;
	}
	if (patterns.empty()) {
		LOG_WARN("[D64] SCRATCH: empty pattern list");
		return 0;
	}

	// Normalize patterns to uppercase ASCII (your wildcard matcher is case-sensitive)
	std::vector<std::string> pats;
	pats.reserve(patterns.size());
	for (auto p : patterns) {
		// Trim/uppercase helper you already have nearby
		for (auto& ch : p) if (ch >= 'a' && ch <= 'z') ch = char(ch - 32);
		while (!p.empty() && (p.back() == ' ' || p.back() == '\r' || p.back() == '\n')) p.pop_back();
		pats.push_back(p);
	}

	// Build fresh catalog (in-memory)
	D64Catalog cat{};
	if (!d64_parse_bam(cat) || !d64_parse_directory(cat)) {
		LOG_ERROR("[D64] SCRATCH: failed to parse BAM/dir");
		return 0;
	}

	auto name_matches_any = [&](const std::string& up) -> bool {
		for (const auto& pat : pats) {
			if (wild_match(pat, up)) return true;
		}
		return false;
		};

	// Helper: count blocks by following the chain
	auto count_chain_blocks = [&](uint8_t t, uint8_t s) -> int {
		int blocks = 0;
		uint8_t sec[256];
		while (t != 0) {
			if (!d64_read_sector(t, s, sec)) {
				LOG_ERROR("[D64] SCRATCH: count_chain read failed at %u/%u", (unsigned)t, (unsigned)s);
				return -1;
			}
			const uint8_t nt = sec[0], ns = sec[1];
			++blocks;
			t = nt; s = ns;
		}
		return blocks;
		};

	// Free a chain **in memory** (don’t write BAM yet)
	auto free_chain_in_memory = [&](D64Catalog& c, uint8_t t, uint8_t s) -> bool {
		uint8_t sec[256];
		while (t != 0) {
			if (!d64_read_sector(t, s, sec)) {
				LOG_ERROR("[D64] SCRATCH: free_chain read failed at %u/%u", (unsigned)t, (unsigned)s);
				return false;
			}
			const uint8_t nt = sec[0], ns = sec[1];
			if (!d64_free_block(c, t, s)) {
				LOG_ERROR("[D64] SCRATCH: free_block failed at %u/%u", (unsigned)t, (unsigned)s);
				return false;
			}
			t = nt; s = ns;
		}
		return true;
		};

	int scratched = 0;
	// Walk a snapshot of the entries so we can scratch multiple
	for (size_t i = 0; i < cat.entries.size(); ++i) {
		const D64DirEntry& e = cat.entries[i];
		if ((e.fileType & 0x07) == 0) continue; // empty
		const std::string up = to_upper_ascii(std::string(e.name));
		if (!name_matches_any(up)) continue;

		// Debug: before counts
		const int dirBlocksBefore = cat.bam[17].freeCount; // track18 index is 18-1
		const int dataBlocks = count_chain_blocks(e.startT, e.startS);
		LOG_DEBUG("[D64] SCRATCH match: \"%s\" type=%02X start=%u/%u blocks=%d dir@%u/%u slot=%u (free18=%d)",
			up.c_str(), e.fileType, e.startT, e.startS, dataBlocks,
			e.dirT, e.dirS, e.slot, dirBlocksBefore);

		// 1) Free data chain in-memory
		if (!free_chain_in_memory(cat, e.startT, e.startS)) {
			LOG_ERROR("[D64] SCRATCH: free_chain_in_memory failed for \"%s\"", up.c_str());
			continue; // try next file; don’t leave partially scratched state
		}

		LOG_DEBUG("[D64] dir scratch at %u/%u slot=%u", e.dirT, e.dirS, e.slot);
		// 2) Scratch directory slot (writes only that dir sector)
		if (!d64_scratch_dir_entry(cat, (int)i)) {
			LOG_ERROR("[D64] SCRATCH: d64_scratch_dir_entry failed for \"%s\"", up.c_str());
			continue; // BAM not written yet; continue
		}

		++scratched;
	}

	if (scratched == 0) {
		LOG_DEBUG("[D64] SCRATCH: no matches");
		return 0;
	}

	// Sanity-log BAM before writing
	d64_log_bam_sanity(cat);

	// Write BAM once
	if (!d64_write_bam_sector(cat)) {
		LOG_ERROR("[D64] SCRATCH: write_bam_sector failed after %d deletes", scratched);
		return 0; // don’t flush a half-baked image
	}

	// Flush the whole image once
	if (!d64_flush_image_to_disk()) {
		LOG_ERROR("[D64] SCRATCH: flush_image_to_disk failed after %d deletes", scratched);
		return 0;
	}

	LOG_DEBUG("[D64] SCRATCH: deleted %d file(s).", scratched);
	return scratched;
}

// Handle a completed command-channel string (accumulated via PRINT#15 / OPEN 15,"CMD")
// Accepts:  S:NAME      (scratch/delete)
//           S0:NAME     (common variant; drive # ignored here)
//           wildcards   (* and ? like CBM-DOS)
// Also trims trailing CR/LF that PET BASIC sends with PRINT#.
//
// Returns true if the command was understood (even if it deleted 0 files).
bool PetIEEE::process_command_channel_string(const std::string& rawIn)
{
	LOG_DEBUG("CMD15 rawIn hex=[%s] text='%s'",
		dump_hex_str(rawIn).c_str(), rawIn.c_str());

	// Copy and strip outer quotes if present
	std::string raw = rawIn;
	if (!raw.empty() && raw.front() == '\"') raw.erase(0, 1);
	if (!raw.empty() && raw.back() == '\"') raw.pop_back();

	// Trim spaces/CR/LF both ends
	auto ltrim = [](std::string& x) { while (!x.empty() && (x.front() == ' ' || x.front() == '\r' || x.front() == '\n')) x.erase(0, 1); };
	auto rtrim = [](std::string& x) { while (!x.empty() && (x.back() == ' ' || x.back() == '\r' || x.back() == '\n')) x.pop_back(); };
	ltrim(raw); rtrim(raw);

	LOG_DEBUG("CMD15 trimmed hex=[%s] text='%s'",
		dump_hex_str(raw).c_str(), raw.c_str());

	if (raw.empty()) { LOG_WARN("CMD15 empty after trim"); return false; }

	// Uppercase copy for verb detection
	std::string up = raw;
	for (auto& c : up) if (c >= 'a' && c <= 'z') c = (char)(c - 32);

	LOG_DEBUG("CMD15 upper hex=[%s] text='%s'",
		dump_hex_str(up).c_str(), up.c_str());

	// Scratch command "S:..." or "S0:..."
	if (up.size() >= 2 && up[0] == 'S') {
		size_t pos = 1;
		if (pos < up.size() && (up[pos] == '0' || up[pos] == '1')) ++pos;
		if (pos >= up.size() || up[pos] != ':') {
			LOG_WARN("CMD15: 'S' missing ':' (got '%s')", up.c_str());
			return false;
		}
		++pos;
		std::string arg = up.substr(pos);
		rtrim(arg);

		LOG_DEBUG("CMD15 verb=S args hex=[%s] text='%s'",
			dump_hex_str(arg).c_str(), arg.c_str());

		// Split args by comma, normalize each token as your current code does
		std::vector<std::string> tokens;
		size_t start = 0;
		while (start <= arg.size()) {
			size_t comma = arg.find(',', start);
			const std::string one = (comma == std::string::npos) ? arg.substr(start)
				: arg.substr(start, comma - start);
			// Use your current normalize (no behavior change)
			std::string norm = normalize_cmd_name(one);
			if (!norm.empty()) tokens.push_back(norm);
			if (comma == std::string::npos) break;
			start = comma + 1;
		}

		// Log parsed tokens
		{
			std::string dbg;
			for (size_t i = 0; i < tokens.size(); ++i) {
				if (i) dbg += ", ";
				dbg += "'" + tokens[i] + "'";
			}
			LOG_DEBUG("CMD15 tokens: %s", dbg.c_str());
		}

		if (tokens.empty()) {
			LOG_WARN("CMD15 S: no valid tokens");
			return false;
		}

		if (!isD64Mounted()) {
			LOG_WARN("CMD15 S: no D64 mounted; ignoring");
			return true;
		}

		LOG_DEBUG("CMD15 S: invoking d64_scratch_patterns...");
		const int n = d64_scratch_patterns(tokens);
		LOG_INFO("SCRATCH: patterns=%zu -> deleted %d file(s)", tokens.size(), n);

		return true;
	}

	LOG_WARN("CMD15 unrecognized verb: '%s'", up.c_str());
	return false;
}