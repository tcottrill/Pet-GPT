#include "pet2001ieee.h"
#include "ieee_helpers.h"

using namespace ieee_helpers;

// ----------------------------------------   SEQ FILE SUPPORT  ---------------------------------------------------- //

// -----------------------------------------------------------------------------
// Name helpers and SEQ-write plumbing
// -----------------------------------------------------------------------------

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

// --- SEQ/PRG/USR write (or append): open on SA ------------------------------
bool PetIEEE::open_seq_write_channel(uint8_t sa, const std::string& rawName,
                                     uint8_t type, bool append)
{
	if (!isD64Mounted()) {
		LOG_WARN("write open ignored: no D64 mounted");
		return false;
	}

	sa &= 0x0F;

	// Normalize (quotes, 0:, mode suffixes) and upper-case to match lookups.
	const std::string name = normalize_open_name_for_seq(rawName);
	if (name.empty()) {
		LOG_WARN("write open: empty normalized name from '%s'", rawName.c_str());
		return false;
	}

	seq_write_buf_[sa].clear();

	// Append (",A"): pre-load the existing file so the commit extends it.
	// openD64SEQ_for_read fills streams[sa]; move those bytes into the write
	// buffer, then clear the read stream. A missing file appends to empty
	// (CBM-DOS creates it), matching real drive behavior.
	if (append) {
		const uint8_t tf = (uint8_t)(type & 0x0F); // 1 SEQ / 2 PRG / 3 USR
		if (openD64SEQ_for_read(name, sa, tf)) {
			seq_write_buf_[sa] = streams[sa].data;
			LOG_DEBUG("write open (append): SA=%u preloaded %zu bytes",
				(unsigned)sa, seq_write_buf_[sa].size());
		}
	}

	seq_write_name_[sa] = name;
	seq_write_type_[sa] = type ? type : 0x81;
	seq_write_active_[sa] = true;

	// This SA is not a talk stream; keep read-stream state isolated.
	streams[sa].reset();

	LOG_DEBUG("write open: SA=%u name='%s' type=%02X append=%d",
		(unsigned)sa, name.c_str(), (unsigned)seq_write_type_[sa], append ? 1 : 0);
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
	const uint8_t type = seq_write_type_[sa] ? seq_write_type_[sa] : 0x81;

	LOG_INFO("write commit: SA=%u name='%s' type=%02X bytes=%zu",
		(unsigned)sa, fname.c_str(), (unsigned)type, bytes.size());

	// d64_save_file sets the CLOSED bit and updates BAM/dir; if the file
	// already exists it is replaced (append preloaded the old contents above).
	const bool ok = d64_save_file(fname,
		bytes.empty() ? nullptr : bytes.data(),
		bytes.size(),
		type);

	if (!ok) {
		LOG_ERROR("SEQ,S,W commit failed for '%s' (%zu bytes)", fname.c_str(), bytes.size());
	}

	// Tear down channel state regardless of success (matches 1541 semantics).
	seq_write_active_[sa] = false;
	seq_write_name_[sa].clear();
	seq_write_buf_[sa].clear();

	return ok;
}

// -----------------------------------------------------------------------------
// Open a named data file (SEQ/PRG/USR) from D64 for reading on a secondary
// address. typeFilter narrows to one type when the OPEN text carried a
// ",S"/",P"/",U" suffix; 0 accepts any data type. Wildcards (* ?) match like
// the LOAD path. For PRG the 2-byte load address is served first, exactly
// like real DOS when a PRG is read through a data channel.
// -----------------------------------------------------------------------------
// Layer: Layer 4 (named-file reader) opened via Layer 3 named channel
bool PetIEEE::openD64SEQ_for_read(const std::string& rawName, uint8_t sa, uint8_t typeFilter)
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
			if ((ftype & 0x80) == 0) continue; // unused/scratched
			const int ft = ftype & 0x0F;       // 1=SEQ 2=PRG 3=USR
			if (typeFilter ? (ft != typeFilter) : (ft < 1 || ft > 3)) continue;
			std::string name = petscii_dirent_to_ascii(e + 3);
			if (wild_match(wantU, to_upper_ascii(name))) {
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
			// CBM-DOS: byte[1] is the offset of the last used byte, so the
			// data-byte count is ns-1 (a full final sector reads 0xFF -> 254).
			int used = (int)ns - 1;
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