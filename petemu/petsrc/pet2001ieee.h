#ifndef PET2001IEEE_H
#define PET2001IEEE_H

#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include "sys_log.h"
#include <stdarg.h>

// --- Debug toggles -----------------------------------------------------------
// Set to 1 to enable open/close/serve and short previews in LOG_DEBUG
#ifndef IEEE_TRACE
#define IEEE_TRACE 0
#endif

// -----------------------------------------------------------------------------
// PetIEEE
// Emulation of PET IEEE-488 device #8, with a minimal virtual drive.
//
// This class models:
//   - Layers 1/2 (IEEE-488 on PET): data and handshake lines and timing.
//   - Layer 3 (TALK/LISTEN/SECOND plus Commodore OPEN/CLOSE).
//   - Layer 4 (subset of Commodore DOS): PRG load, dir "$", SEQ read.
//
// Notes:
//   - The PET imposes two 64 us timeouts in layer 2. A talker should
//     prefetch the next byte while DAV=1 to avoid sender timeout.
//   - ATN rising must not force an early first TALK byte; the receiver
//     (PET) controls pacing by releasing NRFD. See .cpp notes.
//
// New in this version (fixes):
//   - Removed ATN-rising pre-presentation of first TALK byte to avoid
//     duplicate-first-byte on SEQ reads.
//   - Added PRG/LOAD name normalizer (quotes, 0: prefix, commas), in
//     addition to the existing SEQ normalizer.
//   - Verbose function comments with bus layer tagging.
//
// Features retained:
//   - Host-folder PRG load/save (SAVE disabled when D64 mounted).
//   - D64 PRG load and directory ($).
//   - D64 SEQ streaming for OPEN/INPUT# (read).
// -----------------------------------------------------------------------------
// ======== BEGIN: D64 write/allocate support ========
// Simple T/S pair
struct D64Block {
	uint8_t track = 0;
	uint8_t sector = 0;
	bool valid() const { return track != 0; }
};

// Parsed BAM entry for a single track
struct D64BamTrack {
	uint8_t freeCount = 0;   // number of free sectors on this track
	uint8_t bits[3] = { 0,0,0 }; // 24-bit bitmap; 1=free, 0=used
};

// Cached directory entry (minimal fields for PRG/SEQ/USR)
struct D64DirEntry {
	uint8_t fileType = 0x00;     // $82 PRG, $81 SEQ, $83 USR, $84 REL
	uint8_t startT = 0, startS = 0;
	char     name[17] = { 0 };     // ASCII upper; we A0-pad on write
	uint16_t sizeSectors = 0;
	// where this entry lives in the directory chain (for updates)
	uint8_t dirT = 0, dirS = 0;
	uint8_t slot = 0;            // 0..7
};

// Small, in-memory DB reflecting BAM + directory for quick updates
struct D64Catalog {
	bool valid = false;
	std::vector<D64BamTrack> bam;      // 1-based index [1..tracks]
	std::vector<D64DirEntry> entries;  // visible directory entries
	uint8_t dirFirstT = 18;            // 18/1 normally
	uint8_t dirFirstS = 1;
	bool is40 = false;                  // not used for write in this version
	int  tracks = 35;                   // 35 (1541/.d64) or 70 (1571/.d71)
};

enum : int {
	D64_FILE_INTERLEAVE = 10,
	D64_DIR_INTERLEAVE = 3
};
// ======== END: D64 write/allocate support ========

class PetIEEE {
	// Test-only access seam: lets the standalone unit test reach private D64
	// helpers. No effect on production builds (the struct is only defined by
	// the test translation unit).
	friend struct PetIEEE_TestAccess;
public:
	PetIEEE();

	enum State {
		STATE_IDLE = 0,
		STATE_LISTEN,   // listening to controller (name/SAVE setup)
		STATE_FNAME,    // collecting OPEN/CLOSE name under LISTEN
		STATE_LOAD,     // TALKing (send bytes: LOAD or SEQ stream)
		STATE_SAVE,     // LISTENing (receive bytes)
		STATE_SAVE1     // legacy SAVE (old ROM path)
	};
	State state;

	// -----------------------------------------------------------------------------
// d64_log_bam_sanity
// Log BAM per-track freeCount and bit-pop sanity for debugging mismatches.
// Call this right before writing BAM (and optionally after).
// -----------------------------------------------------------------------------
	void d64_log_bam_sanity(const D64Catalog& cat) const;

	// Save a file into the mounted D64 (PRG/SEQ/USR). If name exists, it is scratched and replaced.
	// type: 0x82=PRG, 0x81=SEQ, 0x83=USR
	bool d64_save_file(const std::string& petName, const uint8_t* data, size_t len, uint8_t type);

	// Convenience: save PRG
	bool d64_save_prg(const std::string& petName, const std::vector<uint8_t>& bytes);

	// Reset internal state machine and signals.
	void reset();

	// Mount a host folder (empty string disables). Example: "./files"
	void setHostRoot(const std::string& dir);
	const std::string& getHostRoot() const { return hostRoot; }

	// Mount a .d64 disk image (empty string disables). Example: "./files/DISK.D64"
	// Read-only. Returns true on success.
	bool setD64Image(const std::string& path);
	const std::string& getD64Image() const { return d64Path; }

	// True while a .d64 image is mounted (bytes loaded). Used by the host to
	// enable/grey the Eject menu item.
	bool isD64Mounted() const { return !d64.empty(); }

	// Pre-load bytes for an upcoming LOAD. 'addr' is 16-bit load address.
	// 'bytes' is the payload WITHOUT header. We will prepend the 2-byte header internally.
	void ieeeLoadData(uint16_t addr, const std::vector<uint8_t>& bytes);

	bool scratchHostFile(const std::string& name);

	int scratch_host_patterns(const std::vector<std::string>& tokens);

	// IEEE-488 bus (Layers 1/2): Data and handshake lines (glue layer calls these).
	void DIOout(uint8_t d8);   // device drives DIO when TALKing
	uint8_t DIOin() const;

	bool NDACin() const;
	void NDACout(bool flag);

	bool NRFDin() const;
	void NRFDout(bool flag);

	bool EOIin() const;
	void EOIout(bool flag);

	void ATNout(bool flag);

	void DAVout(bool flag);
	bool DAVin() const;

	bool SRQin() const;

	// Query the current high-level IEEE state. Used by the PET I/O glue
  // to avoid holding handshake lines low when the bus is logically idle.
	State getState() const { return state; }
	bool isIdle() const { return state == STATE_IDLE; }

private:
	// Internal state machine (Layer 3/4 control)
	

	// Bus signal latches (Layer 1/2)
	uint8_t dio;
	bool ndac_i, ndac_o;
	bool nrfd_i, nrfd_o;
	bool atn;
	bool dav_i, dav_o;
	bool srq;
	bool eoi_i, eoi_o;

	// Device internals
	static constexpr int MY_ADDRESS = 8;
	std::string filename;           // last received filename (ASCII-ish)
	bool oldRom;                    // legacy ROM mode flag
	std::vector<uint8_t> load_data; // buffer for LOAD/stream (includes header for LOAD when applicable)
	size_t data_index;              // next byte index into load_data
	std::string save_data;          // bytes captured during SAVE (raw stream)

	// Buffer for command channel text (SA 15) sent via PRINT#15,"...".
	std::string cmd15_buf_;

	// Channel / secondary-address tracking for OPEN/SEQ
	uint8_t last_listen_sa;         // SA received with 0xF0..0xFF while LISTENing

	std::string status15_;  // last DOS status line, e.g. "00,OK,00,00"

	void set_status(uint8_t code, const char* msg, uint8_t track, uint8_t sector);

	// per-SA SEQ write state
	std::array<std::vector<uint8_t>, 16> seq_write_buf_;
	std::array<std::string, 16>          seq_write_name_;
	std::array<bool, 16>                  seq_write_active_{}; // value-init: all false
	std::array<uint8_t, 16>              seq_write_type_{};    // 0x81 SEQ / 0x82 PRG / 0x83 USR

	// Direct-access ('#') buffer channels: DOS block commands U1/U2/B-P.
	// Used by block-level software (e.g. the Zork interpreters) instead of
	// named files: OPEN n,8,sa,"#" then U1 reads a raw sector into the
	// channel buffer, B-P positions within it, U2 writes it back.
	std::array<std::array<uint8_t, 256>, 16> da_buf_{};
	std::array<bool, 16>    da_active_{};
	std::array<uint8_t, 16> da_ptr_{};   // B-P position (also PRINT# write cursor)

	void open_direct_access_channel(uint8_t sa);
	bool cmd_block_u(const std::string& up);   // U1/UA read, U2/UB write, UI/UJ
	bool cmd_buffer_pointer(const std::string& up); // B-P
	bool cmd_block_alloc_free(const std::string& up, bool alloc); // B-A / B-F
	static bool parse_ints_after(const std::string& s, size_t pos, int* out, int want);

	struct Stream {
		std::vector<uint8_t> data;  // raw SEQ bytes (no PRG header)
		size_t index = 0;           // read cursor
		std::string name;           // debug/trace
		bool active() const { return !data.empty(); }
		void reset() { data.clear(); index = 0; name.clear(); }
	};
	std::array<Stream, 16> streams; // SA 0..15
	uint8_t current_talk_sa;        // SA selected by TALK-secondary (0x60..0x6F), 0xFF if none

	// --- Handshake FSM for TALK (device is talker, PET is listener) ---
	enum HS {
		HS_IDLE = 0,
		HS_WAIT_NRFD_H,
		HS_WAIT_NDAC_H,
		HS_WAIT_NRFD_L
	};
	HS   hs_ = HS_IDLE;
	bool last_nrfd_ = true;
	bool last_ndac_ = true;

	// Present current byte on DIO and pull DAV low (does not advance data_index)
	inline void hs_present_byte_() {
		const uint8_t logical = load_data[data_index];
		const uint8_t busval = (uint8_t)(logical ^ 0xFF);
		dio = busval;
		dav_i = false;
#if IEEE_TRACE
		LOG_DEBUG("TALK put byte[%zu/%zu]=%02X bus=%02X",
			(size_t)data_index, load_data.size(), logical, busval);
		if (data_index + 1 == load_data.size()) {
			LOG_DEBUG("TALK last byte -> assert EOI");
		}
#endif
		if (data_index + 1 == load_data.size()) {
			eoi_i = false;
		}
	}

	// Debug helpers
	const char* hs_str() const {
		switch (hs_) {
		case HS_IDLE: return "IDLE";
		case HS_WAIT_NRFD_H: return "WAIT_NRFD_H";
		case HS_WAIT_NDAC_H: return "WAIT_NDAC_H";
		case HS_WAIT_NRFD_L: return "WAIT_NRFD_L";
		default: return "?";
		}
	}
	void hs_debug(const char* tag) const {
#if IEEE_TRACE
		LOG_DEBUG("HS %s: hs=%s NRFD=%d NDAC=%d DAV=%d idx=%zu/%zu sa=%u",
			tag, hs_str(), nrfd_o ? 1 : 0, ndac_o ? 1 : 0, dav_i ? 1 : 0,
			(size_t)data_index, load_data.size(), (unsigned)current_talk_sa);
#else
		(void)tag;
#endif
	}
	// Host folder backend
	std::string hostRoot;           // "./files" or empty

	// D64/D71 backend
	std::string d64Path;
	std::vector<uint8_t> d64;       // entire image; empty if not mounted
	int d64_tracks_ = 35;           // 35 (.d64/1541) or 70 (.d71/1571); set at mount

	// Core helpers
	void dataIn(uint8_t d8);
	void saveFile(const std::string& fname, const std::string& contents);

	// One visible directory entry (shared by the D64 and folder list builders).
	struct DirEntry { int blocks; std::string name; std::string type; };

	// Shared directory-PRG emitter: 16-padded reverse-video header
	// ("NAME"  ID DOS), one formatted line per entry (right-aligned blocks,
	// tight-quoted name, aligned type column), $14 DELETE chars to erase BASIC's
	// printed line numbers, and an "N BLOCKS FREE." footer. Used by both the D64
	// and folder directory builders so they look identical.
	bool buildDirectoryPRG_common(std::vector<uint8_t>& out, uint16_t startAddr,
		const std::string& diskName, const std::string& id, const std::string& dos,
		const std::vector<DirEntry>& entries, int freeBlocks);

	// Folder helpers ('match' is an optional CBM wildcard pattern for "$"; empty = all)
	bool buildDirectoryPRG_Folder(std::vector<uint8_t>& out, uint16_t startAddr = 0x0401, const std::string& match = "");
	bool loadHostPRG_Folder(const std::string& petName, std::vector<uint8_t>& payload, uint16_t& loadAddr);

	// D64/D71 helpers (1541 35-track / 1571 70-track, read-write)
	bool buildDirectoryPRG_D64(std::vector<uint8_t>& out, uint16_t startAddr = 0x0401, const std::string& match = "");
	bool loadHostPRG_D64(const std::string& petName, std::vector<uint8_t>& payload, uint16_t& loadAddr);

	// D64 named-file reader for OPEN/INPUT#/GET#. typeFilter: 0 = any data
	// type (SEQ/PRG/USR), 1 = SEQ, 2 = PRG, 3 = USR (from the ",S"/",P"/",U"
	// suffix of the OPEN text). Name may contain CBM wildcards (* ?).
	bool openD64SEQ_for_read(const std::string& petName, uint8_t sa, uint8_t typeFilter = 0);

	static std::string strip_drive_prefix(const std::string& n); // drop "0:" if present

	// D64/D71 low-level
	static int d64_sectors_per_track(int track);                      // 1..70 (side-1 mirrors side-0 zones)
	int  d64_num_tracks() const { return d64_tracks_; }               // 35 (.d64) or 70 (.d71)
	bool d64_double_sided() const { return d64_tracks_ > 35; }
	// 1571 split-BAM constants: side-1 free counts live in 18/0, bitmaps in 53/0.
	static constexpr int D71_BAM2_TRACK = 53;
	static constexpr int D71_SIDE1_COUNT_OFF = 0xDD;  // 18/0: 35 count bytes for tracks 36..70
	size_t d64_sector_offset(int track, int sector) const;            // returns byte offset into d64 vector
	bool d64_read_sector(int track, int sector, uint8_t* dst) const;  // 256 bytes

	// Utils
	static std::string to_upper_ascii(std::string s);
	static std::string petscii_dirent_to_ascii(const uint8_t* name16);

	// New: PRG/LOAD name normalizer (separate from SEQ open normalizer)
	static std::string normalize_name_for_prg(const std::string& raw);

	// Small logging helpers (routed to your logger)
	static inline void trace(const char* msg) {
#if IEEE_TRACE
		LOG_DEBUG("%s", msg);
#else
		(void)msg;
#endif
	}
	static inline void tracef(const char* fmt, ...) {
#if IEEE_TRACE
		char buf[512];
		va_list ap; va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		LOG_DEBUG("%s", buf);
#else
		(void)fmt;
#endif
	}
	static inline void trace_hex_preview(const char* tag, const uint8_t* p, size_t n) {
#if IEEE_TRACE
		const size_t k = (n > 32 ? 32 : n);
		char line[256]; char* out = line; int rem = (int)sizeof(line);
		int w = snprintf(out, rem, "%s (%zu bytes): ", tag, n);
		if (w < 0) return; out += w; rem -= w;
		for (size_t i = 0; i < k && rem > 3; ++i) { w = snprintf(out, rem, "%02X ", p[i]); out += w; rem -= w; }
		LOG_DEBUG("%s", line);
		out = line; rem = (int)sizeof(line);
		w = snprintf(out, rem, "%s ASCII: ", tag); out += w; rem -= w;
		for (size_t i = 0; i < k && rem > 2; ++i) { const uint8_t c = p[i]; *out++ = (c >= 32 && c <= 126) ? (char)c : '.'; --rem; }
		*out = 0; LOG_DEBUG("%s", line);
#else
		(void)tag; (void)p; (void)n;
#endif
	}
	bool d64_free_chain(D64Catalog& cat, uint8_t t, uint8_t s);
	// --- New low-level write/persist ---
	bool d64_write_sector(int track, int sector, const uint8_t* src); // 256 bytes
	bool d64_flush_image_to_disk(); // overwrite d64Path with current 'd64' vector

	// --- Catalog parse ---
	bool d64_parse_bam(D64Catalog& out);
	bool d64_parse_directory(D64Catalog& out);

	// --- BAM helpers ---
	bool d64_bam_is_free(const D64Catalog& cat, int track, int sector) const;
	void d64_bam_set_used(D64Catalog& cat, int track, int sector, bool used);
	bool d64_write_bam_sector(const D64Catalog& cat);

	// --- Directory helpers ---
	bool d64_load_dir_sector(uint8_t t, uint8_t s, std::array<uint8_t, 256>& sec);
	bool d64_write_dir_sector(uint8_t t, uint8_t s, const std::array<uint8_t, 256>& sec);
	bool d64_find_dir_entry(const D64Catalog& cat, const std::string& petNameUpper, int& idx);
	bool d64_scratch_dir_entry(D64Catalog& cat, int idx);
	bool d64_append_dir_entry(D64Catalog& cat, const D64DirEntry& src, int& outIndex);

	// dir growth
	bool d64_find_last_dir_sector(const D64Catalog& cat, uint8_t& outT, uint8_t& outS);
	bool d64_alloc_next_dir_sector_on_track18(D64Catalog& cat,
		uint8_t prevT, uint8_t prevS,
		uint8_t& newT, uint8_t& newS);

	// --- Allocation ---
	bool d64_alloc_block(D64Catalog& cat, int preferredTrack, int interleave, D64Block& out);
	bool d64_free_block(D64Catalog& cat, int track, int sector); // not used yet

	// --- File write ---
	bool d64_write_file_chain(D64Catalog& cat, uint8_t type, const std::string& petNameUpper,
		const uint8_t* data, size_t len, D64DirEntry& outEntry);

	// --- Name helpers ---
	static std::string to_pet_upper_a0_padded_16(const std::string& ascii);

	// While LISTENing and receiving data bytes for SA, append to stream buffer if SEQ,S,W
	void accept_byte_for_possible_seq_write(uint8_t sa, uint8_t b);

	// On CLOSE of SA: if SEQ,S,W was open, commit to D64 and tear down
	bool close_seq_write_channel(uint8_t sa);

	// Arm a SEQ/PRG/USR write (or append) channel. type: 0x81/0x82/0x83.
	// append pre-loads the existing file so the commit extends it.
	bool open_seq_write_channel(uint8_t sa, const std::string& rawName,
	                            uint8_t type = 0x81, bool append = false);

	// D64 scratch helpers
	int  d64_scratch_patterns(const std::vector<std::string>& patterns);

	// D64 command-channel operations (each sets channel-15 status itself)
	bool d64_rename(const std::string& oldName, const std::string& newName);
	bool d64_copy(const std::string& srcName, const std::string& dstName);
	bool d64_format(const std::string& diskName, const std::string& id);
	bool d64_validate();

	// Host-folder command-channel operations (PRG-only backend)
	bool host_rename(const std::string& oldName, const std::string& newName);
	bool host_copy(const std::string& srcName, const std::string& dstName);

	// Command channel handler (SA 15)
	bool process_command_channel_string(const std::string& rawIn);

	// File not found handler
	void not_found_handler(const char* type, const std::string& name, uint16_t addr);
};

#endif // PET2001IEEE_H
