// pet2001ieee_dir.cpp
#include "pet2001ieee.h"
#include "ieee_helpers.h"

using namespace ieee_helpers;

// -----------------------------------------------------------------------------
// D64 low-level helpers (sector math / read / write / flush)
// -----------------------------------------------------------------------------
// Layer: D64 fs helpers
int PetIEEE::d64_sectors_per_track(int track) {
	if (track < 1) return 0;
	// 1571 side 1 (tracks 36..70) mirrors side 0's speed-zone geometry.
	int z = (track >= 36 && track <= 70) ? (track - 35) : track;
	if (z <= 17) return 21;
	if (z <= 24) return 19;
	if (z <= 30) return 18;
	if (z <= 35) return 17;
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
	// Writable geometries: 35-track (.d64/1541) and 70-track (.d71/1571), with
	// or without a trailing error map. Any other size (e.g. 40-track) has BAM
	// math we don't model, so it stays WRITE-PROTECTED: reads work, writes fail
	// cleanly (callers set WRITE ERROR / DS$).
	const size_t n = d64.size();
	const bool writable = (n == 174848 || n == 175531 ||   // 35-track
	                       n == 349696 || n == 351062);     // 70-track (.d71)
	if (!writable) {
		LOG_ERROR("[D64] image geometry not writable (%zu bytes) - mounted write-protected, not flushed", n);
		return false;
	}
	return write_all_file(std::filesystem::path(d64Path), d64);
}

// -----------------------------------------------------------------------------
// D64 directory PRG builder
// -----------------------------------------------------------------------------
bool PetIEEE::buildDirectoryPRG_D64(std::vector<uint8_t>& out, uint16_t startAddr, const std::string& match) {
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

	// Free blocks: BAM free count per track. Track 18 (the directory track) is
	// excluded, matching a real 1541 (a blank disk reports 664, not 683).
	int freeBlocks = 0;
	for (int track = 1; track <= 35; ++track) {
		if (track == 18) continue;
		const int off = 0x04 + (track - 1) * 4;
		if (off >= 256) break;
		freeBlocks += bam[off];
	}
	// 1571 side 1: free counts for tracks 36..70 sit at $DD in 18/0. Track 53
	// (the side-1 BAM track) is excluded, same convention as track 18 - a blank
	// .d71 then reports 1328 blocks free.
	if (d64_double_sided()) {
		for (int track = 36; track <= 70; ++track) {
			if (track == D71_BAM2_TRACK) continue;
			freeBlocks += bam[D71_SIDE1_COUNT_OFF + (track - 36)];
		}
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

	// Header fields (the shared emitter pads/wraps them like a 1541).
	const std::string id = read_id_2(&bam[0xA2]);   // e.g. "1A"
	const std::string dos = read_dos_2(&bam[0xA5]); // e.g. "2A"

	// ----- Walk directory chain (18/1 -> ..) and gather entries -----
	std::vector<DirEntry> entries;

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

			// Visible name: tight (no in-quote padding), but cap to 16 (D64 limit)
			std::string name = tight_name(e + 3);
			if ((int)name.size() > 16) name.resize(16);

			// Optional CBM wildcard filter from "$:pattern" (empty = list all).
			if (!match.empty() && !wild_match(to_upper_ascii(match), to_upper_ascii(name)))
				continue;

			// Type token: REL record length is at 0x1C (not 0x1E)
			const uint8_t recLen = e[0x1C];
			entries.push_back({ blocks, name, type_token(ftype, recLen) });
		}

		if (nextT == 0) break;
		t = nextT; s = nextS;
	}

	return buildDirectoryPRG_common(out, startAddr, diskName, id, dos, entries, freeBlocks);
}

// -----------------------------------------------------------------------------
// Shared directory-PRG emitter: header + entry lines + "N BLOCKS FREE." footer,
// formatted like a real 1541. Both buildDirectoryPRG_D64 and _Folder feed this.
// -----------------------------------------------------------------------------
bool PetIEEE::buildDirectoryPRG_common(std::vector<uint8_t>& out, uint16_t startAddr,
	const std::string& diskName, const std::string& id, const std::string& dos,
	const std::vector<DirEntry>& entries, int freeBlocks)
{
	auto pad_trunc_16 = [](std::string s) {
		if (s.size() > 16) s.resize(16);
		else if (s.size() < 16) s.append(16 - s.size(), ' ');
		return s;
		};

	// Header visible content (NO leading "0 "; BASIC prints a single 0).
	std::string hdr_visible;
	hdr_visible += "\"";
	hdr_visible += pad_trunc_16(diskName);   // 16 chars inside quotes
	hdr_visible += "\" ";
	hdr_visible += id;
	hdr_visible += " ";
	const int type_col = (int)hdr_visible.size();
	const int type_col_screen = type_col + 2;   // add the leading "0 " BASIC prints
	hdr_visible += dos;

	// Reverse-video header + RVSOFF + DELETE chars (erase the "WAIT" artifact).
	const uint8_t RVSON = 0x12, RVSOFF = 0x92;
	std::vector<uint8_t> hdr_bytes;
	hdr_bytes.reserve(hdr_visible.size() + 6);
	hdr_bytes.push_back(RVSON);
	hdr_bytes.insert(hdr_bytes.end(), hdr_visible.begin(), hdr_visible.end());
	hdr_bytes.push_back(RVSOFF);
	hdr_bytes.push_back(0x14); hdr_bytes.push_back(0x14);
	hdr_bytes.push_back(0x14); hdr_bytes.push_back(0x14);

	// One entry's visible line: blocks(%5d) ' ' "name" "  " <pad to type col> type.
	auto format_entry_line = [&](const DirEntry& en) -> std::string {
		char bfield[8];
		std::snprintf(bfield, sizeof(bfield), "%5d", en.blocks);
		std::string line;
		line += bfield;
		line += ' ';
		line += '"';
		line += en.name;
		line += '"';
		line += "  ";
		const int target_col = type_col_screen + 3;
		if ((int)line.size() < target_col) line.append(target_col - (int)line.size(), ' ');
		line += en.type;
		return line;
		};

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

	// Entries/footer erase the printed line number (digits + trailing space).
	auto append_entry = [&](uint16_t lineNo, const std::string& text, int extraEatCols) {
		int digits = (lineNo < 10) ? 1 : (lineNo < 100) ? 2 : (lineNo < 1000) ? 3 : 4;
		int dels = digits + 1 + extraEatCols;
		std::vector<uint8_t> bytes;
		bytes.reserve(dels + text.size());
		for (int i = 0; i < dels; ++i) bytes.push_back(0x14); // $14 = DELETE
		bytes.insert(bytes.end(), text.begin(), text.end());
		append_basic_line(lineNo, bytes);
		};

	// Header (line 0): keep the printed "0 " (one visible zero).
	append_basic_line(0, hdr_bytes);

	uint16_t ln = 10;
	for (const auto& en : entries) {
		append_entry(ln, format_entry_line(en), 3);
		ln = (uint16_t)(ln + 10);
	}

	{
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%d BLOCKS FREE.", freeBlocks);
		append_entry(ln, buf, 0);
	}

	body.push_back(0x00); body.push_back(0x00);

	out.clear();
	out.push_back((uint8_t)(startAddr & 0xFF));
	out.push_back((uint8_t)(startAddr >> 8));
	out.insert(out.end(), body.begin(), body.end());
	return true;
}
// -----------------------------------------------------------------------------
// Catalog / BAM / directory / allocation helpers
// -----------------------------------------------------------------------------
bool PetIEEE::d64_parse_bam(D64Catalog& out)
{
	uint8_t bam[256];
	if (!d64_read_sector(18, 0, bam)) return false;

	const int nt = d64_num_tracks();
	out.tracks = nt;
	out.bam.clear();
	out.bam.resize(nt + 1); // index by track; 1..nt

	for (int t = 1; t <= 35; ++t) {
		const int off = 4 + (t - 1) * 4;
		D64BamTrack bt{};
		bt.freeCount = bam[off + 0];
		bt.bits[0] = bam[off + 1];
		bt.bits[1] = bam[off + 2];
		bt.bits[2] = bam[off + 3];
		out.bam[t] = bt;
	}

	// 1571 side 1: free counts in 18/0 ($DD+), 3-byte bitmaps in 53/0.
	if (nt > 35) {
		uint8_t bam2[256];
		if (!d64_read_sector(D71_BAM2_TRACK, 0, bam2)) return false;
		for (int t = 36; t <= 70; ++t) {
			D64BamTrack bt{};
			bt.freeCount = bam[D71_SIDE1_COUNT_OFF + (t - 36)];
			const int b = 3 * (t - 36);
			bt.bits[0] = bam2[b + 0];
			bt.bits[1] = bam2[b + 1];
			bt.bits[2] = bam2[b + 2];
			out.bam[t] = bt;
		}
	}

	out.dirFirstT = bam[0x00] ? bam[0x00] : 18;
	out.dirFirstS = bam[0x01] ? bam[0x01] : 1;
	out.is40 = false;
	return true;
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

	// 1571 side 1: write free counts back into 18/0 ($DD+) and bitmaps into 53/0.
	if (d64_num_tracks() > 35 && (int)cat.bam.size() > 70) {
		bam[0x03] = 0x80;  // double-sided flag
		uint8_t bam2[256];
		if (!d64_read_sector(D71_BAM2_TRACK, 0, bam2)) return false;
		for (int t = 36; t <= 70; ++t) {
			bam[D71_SIDE1_COUNT_OFF + (t - 36)] = cat.bam[t].freeCount;
			const int b = 3 * (t - 36);
			bam2[b + 0] = cat.bam[t].bits[0];
			bam2[b + 1] = cat.bam[t].bits[1];
			bam2[b + 2] = cat.bam[t].bits[2];
		}
		if (!d64_write_sector(D71_BAM2_TRACK, 0, bam2)) return false;
	}
	return d64_write_sector(18, 0, bam);
}

bool PetIEEE::d64_load_dir_sector(uint8_t t, uint8_t s, std::array<uint8_t, 256>& sec)
{
	return d64_read_sector(t, s, sec.data());
}

bool PetIEEE::d64_write_dir_sector(uint8_t t, uint8_t s, const std::array<uint8_t, 256>& sec)
{
	return d64_write_sector(t, s, sec.data());
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

// -----------------------------------------------------------------------------
// Allocate a free data block, preferring 'preferredTrack' and using 'interleave'.
// Skips Track 18 (DOS/BAM/Directory).
// -----------------------------------------------------------------------------
bool PetIEEE::d64_alloc_block(D64Catalog& cat, int preferredTrack, int interleave, D64Block& out)
{
	const int nt = d64_num_tracks();
	int t = preferredTrack;
	if (t < 1 || t > nt) t = 17;
	if (t == 18 || t == D71_BAM2_TRACK) t = 17; // never start on a DOS/BAM track

	auto try_track = [&](int tt) -> bool {
		if (tt == 18) return false;                          // side-0 dir/BAM track
		if (nt > 35 && tt == D71_BAM2_TRACK) return false;   // side-1 BAM track (.d71)
		const int ms = d64_sectors_per_track(tt);
		if (ms <= 0) return false;

		auto take = [&](int s) -> bool {
			if (!d64_bam_is_free(cat, tt, s)) return false;
			d64_bam_set_used(cat, tt, s, true);
			out.track = (uint8_t)tt;
			out.sector = (uint8_t)s;
			return true;
			};

		// Interleave-preferred pass. A plain (offset*interleave)%ms walk only
		// visits every sector when gcd(interleave,ms)==1; for the 18-sector
		// tracks (25..30) with interleave 10 (gcd 2) it reached just 9 of 18
		// sectors, so half the track was unallocatable and the disk could
		// report "full" with space remaining. Spread by 'interleave' here for
		// fragmentation, then guarantee coverage with a linear sweep below.
		int s = 0;
		for (int n = 0; n < ms; ++n) {
			if (take(s)) return true;
			s += interleave;
			if (s >= ms) s -= ms;
		}
		// Coverage guarantee: pick up any sector the interleave walk skipped.
		for (int s2 = 0; s2 < ms; ++s2)
			if (take(s2)) return true;
		return false;
		};

	if (try_track(t)) return true;

	// Spiral search outward from preferred track
	for (int rad = 1; rad <= nt; ++rad) {
		for (int sign = -1; sign <= 1; sign += 2) {
			int tt = t + sign * rad;
			if (tt < 1 || tt > nt) continue;
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
			// end of chain. CBM-DOS stores the OFFSET of the last used byte in
			// byte[1] (data begins at offset 2), so for 'chunk' data bytes that
			// is chunk+1; a full sector is 0xFF. Readers recover count = byte1-1.
			// (Verified against real 1541 images: full final sectors read 0xFF,
			//  which is impossible under a raw byte-count convention.)
			buf[0] = 0x00;
			buf[1] = (uint8_t)(chunk + 1); // offset of last used byte (1..255)
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
	if (!d64_parse_bam(cat) || !d64_parse_directory(cat)) {
		set_status(74, "DRIVE NOT READY", 0, 0);
		return false;
	}

	const std::string petUp = to_upper_ascii(petName);

	// Detect existing entry but DO NOT delete it yet
	int existingIdx = -1;
	const bool hasExisting = d64_find_dir_entry(cat, petUp, existingIdx);

	// Count the blocks a replaced file would return to the free pool.
	int oldBlocks = 0;
	if (hasExisting && existingIdx >= 0 && cat.entries[existingIdx].startT) {
		int t = cat.entries[existingIdx].startT, s = cat.entries[existingIdx].startS;
		uint8_t sec[256];
		for (int g = 0; g < 100000 && t != 0; ++g) {
			if (!d64_read_sector(t, s, sec)) break;
			++oldBlocks; const int nt = sec[0], ns = sec[1]; t = nt; s = ns;
		}
	}

	// Capacity pre-flight: needed data blocks vs. allocatable free space
	// (excludes the DOS/BAM tracks the allocator skips) PLUS the blocks a
	// replaced file will free. Rejecting up front means we never overwrite the
	// old file's sectors on a save that cannot fit, and lets a replace REUSE the
	// old blocks instead of demanding room for two copies at once.
	const int needBlocks = (len < 1) ? 1 : (int)((len + 253) / 254);
	int allocFree = 0;
	for (int t = 1; t <= cat.tracks; ++t) {
		if (t == 18) continue;
		if (cat.tracks > 35 && t == D71_BAM2_TRACK) continue;
		allocFree += cat.bam[t].freeCount;
	}
	if (needBlocks > allocFree + oldBlocks) {
		set_status(72, "DISK FULL", 0, 0);
		return false;
	}

	// 1) Replacing: free the old chain in-memory FIRST so the new chain can
	//    reuse those blocks. Nothing is committed until the final BAM write +
	//    flush, so aborting leaves the on-disk image intact.
	if (hasExisting && existingIdx >= 0 && cat.entries[existingIdx].startT) {
		if (!d64_free_chain(cat, cat.entries[existingIdx].startT, cat.entries[existingIdx].startS)) {
			set_status(25, "WRITE ERROR", 0, 0); return false;
		}
	}

	// 2) Allocate and write the NEW chain (capacity was checked above).
	D64DirEntry newEnt{};
	if (!d64_write_file_chain(cat, type, petUp, data, len, newEnt)) {
		set_status(72, "DISK FULL", 0, 0);
		return false;
	}

	// 3) Remove the old directory entry now that the new chain exists.
	if (hasExisting && existingIdx >= 0) {
		if (!d64_scratch_dir_entry(cat, existingIdx)) { set_status(25, "WRITE ERROR", 0, 0); return false; }
	}

	// 4) Append the new directory entry (may grow dir; still no BAM write yet)
	int newIdx = -1;
	if (!d64_append_dir_entry(cat, newEnt, newIdx)) {
		set_status(72, "DISK FULL", 0, 0);
		return false;
	}

	// 5) Sanity log and single commit
	d64_log_bam_sanity(cat);

	if (!d64_write_bam_sector(cat) || !d64_flush_image_to_disk()) {
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}

	set_status(0, "OK", 0, 0);
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

// --------------------------- D64 SCRATCH SUPPORT -----------------------------

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

	// Free a chain **in memory** (don't write BAM yet)
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
			continue; // try next file; don't leave partially scratched state
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
		return 0; // don't flush a half-baked image
	}

	// Flush the whole image once
	if (!d64_flush_image_to_disk()) {
		LOG_ERROR("[D64] SCRATCH: flush_image_to_disk failed after %d deletes", scratched);
		return 0;
	}

	LOG_DEBUG("[D64] SCRATCH: deleted %d file(s).", scratched);
	return scratched;
}

// -----------------------------------------------------------------------------
// RENAME (CMD15 "R:new=old"): rewrite the directory entry's name in place.
// -----------------------------------------------------------------------------
bool PetIEEE::d64_rename(const std::string& oldName, const std::string& newName)
{
	if (!isD64Mounted()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	D64Catalog cat{};
	if (!d64_parse_bam(cat) || !d64_parse_directory(cat)) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	const std::string oldU = to_upper_ascii(oldName);
	const std::string newU = to_upper_ascii(newName);

	int idx = -1;
	if (!d64_find_dir_entry(cat, oldU, idx)) { set_status(62, "FILE NOT FOUND", 0, 0); return false; }
	int dummy = -1;
	if (d64_find_dir_entry(cat, newU, dummy)) { set_status(63, "FILE EXISTS", 0, 0); return false; }

	const D64DirEntry& e = cat.entries[idx];
	std::array<uint8_t, 256> sec{};
	if (!d64_load_dir_sector(e.dirT, e.dirS, sec)) { set_status(25, "WRITE ERROR", 0, 0); return false; }

	const int base = 0x20 * e.slot;
	const std::string pn = to_pet_upper_a0_padded_16(newU);
	for (int j = 0; j < 16; ++j) sec[base + 0x05 + j] = (uint8_t)pn[j];

	if (!d64_write_dir_sector(e.dirT, e.dirS, sec) || !d64_flush_image_to_disk()) {
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}
	set_status(0, "OK", 0, 0);
	return true;
}

// -----------------------------------------------------------------------------
// COPY (CMD15 "C:new=old"): duplicate a file under a new name (same type).
// -----------------------------------------------------------------------------
bool PetIEEE::d64_copy(const std::string& srcName, const std::string& dstName)
{
	if (!isD64Mounted()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	D64Catalog cat{};
	if (!d64_parse_bam(cat) || !d64_parse_directory(cat)) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	const std::string srcU = to_upper_ascii(srcName);
	const std::string dstU = to_upper_ascii(dstName);

	int sidx = -1;
	if (!d64_find_dir_entry(cat, srcU, sidx)) { set_status(62, "FILE NOT FOUND", 0, 0); return false; }
	int didx = -1;
	if (d64_find_dir_entry(cat, dstU, didx)) { set_status(63, "FILE EXISTS", 0, 0); return false; }

	const D64DirEntry e = cat.entries[sidx];

	// Read the source file's raw chain (CBM last-sector byte convention: count = byte1-1).
	std::vector<uint8_t> data;
	{
		uint8_t sec[256];
		int t = e.startT, s = e.startS;
		for (int guard = 0; guard < 100000 && t != 0; ++guard) {
			if (!d64_read_sector(t, s, sec)) { set_status(25, "WRITE ERROR", 0, 0); return false; }
			const int nt = sec[0], ns = sec[1];
			if (nt == 0) {
				int used = ns - 1; if (used < 0) used = 0; if (used > 254) used = 254;
				data.insert(data.end(), sec + 2, sec + 2 + used);
				break;
			}
			data.insert(data.end(), sec + 2, sec + 256);
			t = nt; s = ns;
		}
	}

	const uint8_t type = (uint8_t)(0x80 | (e.fileType & 0x0F));
	// d64_save_file re-parses, allocates, writes the new chain, and sets status.
	return d64_save_file(dstU, data.empty() ? nullptr : data.data(), data.size(), type);
}

// -----------------------------------------------------------------------------
// NEW / HEADER (CMD15 "N:name,id"): write a fresh BAM + empty directory.
// A supplied id sets the disk ID; an empty id keeps the existing one (quick header).
// This wipes all files (full reformat of the mounted image, size preserved).
// -----------------------------------------------------------------------------
bool PetIEEE::d64_format(const std::string& diskName, const std::string& id)
{
	if (!isD64Mounted()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	uint8_t cur[256];
	const bool haveCur = d64_read_sector(18, 0, cur); // for ID preservation

	const bool ds = d64_double_sided();

	uint8_t bam[256] = { 0 };
	bam[0] = 18; bam[1] = 1;   // first directory sector
	bam[2] = 0x41;             // DOS version 'A'
	bam[3] = ds ? 0x80 : 0x00; // double-sided flag (1571)

	for (int t = 1; t <= 35; ++t) {
		const int n = d64_sectors_per_track(t);
		uint8_t bits[3] = { 0,0,0 };
		for (int s = 0; s < n; ++s) bits[s >> 3] |= (uint8_t)(1u << (s & 7));
		int freec = n;
		if (t == 18) { bits[0] &= (uint8_t)~0x03; freec = n - 2; } // 18/0 BAM + 18/1 dir used
		const int off = 4 + (t - 1) * 4;
		bam[off + 0] = (uint8_t)freec; bam[off + 1] = bits[0]; bam[off + 2] = bits[1]; bam[off + 3] = bits[2];
	}

	// 1571 side 1: all-free bitmaps in 53/0, free counts back into 18/0 ($DD+).
	// The side-1 BAM sector (53/0) is the only reserved block on side 1.
	uint8_t bam2[256] = { 0 };
	if (ds) {
		for (int t = 36; t <= 70; ++t) {
			const int n = d64_sectors_per_track(t);
			uint8_t bits[3] = { 0,0,0 };
			for (int s = 0; s < n; ++s) bits[s >> 3] |= (uint8_t)(1u << (s & 7));
			int freec = n;
			if (t == D71_BAM2_TRACK) { bits[0] &= (uint8_t)~0x01; freec = n - 1; } // 53/0 used
			bam[D71_SIDE1_COUNT_OFF + (t - 36)] = (uint8_t)freec;
			const int b = 3 * (t - 36);
			bam2[b + 0] = bits[0]; bam2[b + 1] = bits[1]; bam2[b + 2] = bits[2];
		}
	}

	const std::string pn = to_pet_upper_a0_padded_16(diskName);
	for (int i = 0; i < 16; ++i) bam[0x90 + i] = (uint8_t)pn[i];
	bam[0xA0] = 0xA0; bam[0xA1] = 0xA0;

	const std::string up = to_upper_ascii(id);
	uint8_t id0 = 0xA0, id1 = 0xA0;
	if (!up.empty()) { id0 = (uint8_t)up[0]; id1 = (up.size() >= 2) ? (uint8_t)up[1] : (uint8_t)0xA0; }
	else if (haveCur) { id0 = cur[0xA2]; id1 = cur[0xA3]; }
	bam[0xA2] = id0; bam[0xA3] = id1;
	bam[0xA4] = 0xA0;
	bam[0xA5] = '2'; bam[0xA6] = 'A';   // DOS type "2A"
	bam[0xA7] = 0xA0; bam[0xA8] = 0xA0; bam[0xA9] = 0xA0; bam[0xAA] = 0xA0;

	uint8_t dir[256] = { 0 };
	dir[0] = 0x00; dir[1] = 0xFF;       // single empty directory sector

	if (!d64_write_sector(18, 0, bam) || !d64_write_sector(18, 1, dir)) {
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}
	if (ds && !d64_write_sector(D71_BAM2_TRACK, 0, bam2)) {
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}
	if (!d64_flush_image_to_disk()) {
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}
	set_status(0, "OK", 0, 0);
	return true;
}

// -----------------------------------------------------------------------------
// VALIDATE / COLLECT (CMD15 "V"): rebuild the BAM from the directory + file
// chains, reclaiming orphaned blocks. Unclosed ("splat") files are removed.
// -----------------------------------------------------------------------------
bool PetIEEE::d64_validate()
{
	if (!isD64Mounted()) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	D64Catalog cat{};
	if (!d64_parse_bam(cat) || !d64_parse_directory(cat)) { set_status(74, "DRIVE NOT READY", 0, 0); return false; }

	// Start from an all-free BAM (valid sectors only), covering both sides.
	const int nt = d64_num_tracks();
	for (int t = 1; t <= nt; ++t) {
		D64BamTrack bt{};
		const int n = d64_sectors_per_track(t);
		for (int s = 0; s < n; ++s) set_bit_24(bt.bits, s, true);
		bt.freeCount = (uint8_t)n;
		cat.bam[t] = bt;
	}

	auto mark_chain = [&](int t, int s) -> void {
		for (int guard = 0; guard < 100000 && t != 0; ++guard) {
			const int spt = d64_sectors_per_track(t);
			if (spt <= 0 || s < 0 || s >= spt) break;   // invalid link: stop walking
			d64_bam_set_used(cat, t, s, true);
			uint8_t sec[256];
			if (!d64_read_sector(t, s, sec)) break;
			t = sec[0]; s = sec[1];
		}
		};

	// System blocks: BAM (18/0) and the directory chain (18/1 -> ...).
	d64_bam_set_used(cat, 18, 0, true);
	if (nt > 35) d64_bam_set_used(cat, D71_BAM2_TRACK, 0, true); // side-1 BAM (.d71)
	mark_chain(cat.dirFirstT ? cat.dirFirstT : 18, cat.dirFirstS ? cat.dirFirstS : 1);

	// Each closed file marks its chain; unclosed ("splat") files are removed.
	for (size_t i = 0; i < cat.entries.size(); ++i) {
		const D64DirEntry& e = cat.entries[i];
		if ((e.fileType & 0x0F) == 0) continue;          // already deleted
		if ((e.fileType & 0x80) == 0) {                  // not closed -> splat
			LOG_INFO("[D64] VALIDATE: removing unclosed file \"%s\"", e.name);
			d64_scratch_dir_entry(cat, (int)i);
			continue;
		}
		if (e.startT) mark_chain(e.startT, e.startS);
	}

	if (!d64_write_bam_sector(cat) || !d64_flush_image_to_disk()) {
		set_status(25, "WRITE ERROR", 0, 0);
		return false;
	}
	set_status(0, "OK", 0, 0);
	return true;
}