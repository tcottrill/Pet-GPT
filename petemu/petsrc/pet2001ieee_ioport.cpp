#include "pet2001ieee.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include "ieee_helpers.h"
using namespace ieee_helpers;  // or qualify each helper call explicitly

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

	// Save-with-replace: strip a leading '@' and an optional "@:" current-drive
	// colon. "@0:"/"@1:" is left for the drive-prefix block below. (Harmless for
	// LOAD names, which never start with '@'.)
	if (!s.empty() && s.front() == '@') {
		s.erase(0, 1);
		if (!s.empty() && s.front() == ':') s.erase(0, 1);
	}

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
// set_status
// Build a CBM DOS-style status string: "cc,MSG,tt,ss"
//
// cc   - 2-digit error code
// MSG  - status message (e.g. "OK", "FILE NOT FOUND")
// tt   - track (00 if not applicable)
// ss   - sector (00 if not applicable)
// -----------------------------------------------------------------------------
void PetIEEE::set_status(uint8_t code, const char* msg, uint8_t track, uint8_t sector)
{
	char buf[64];

	if (track > 99) track = 99;
	if (sector > 99) sector = 99;

	std::snprintf(buf, sizeof(buf), "%02u,%s,%02u,%02u",
		(unsigned)code,
		msg ? msg : "OK",
		(unsigned)track,
		(unsigned)sector);

	status15_ = buf;
	std::string final_status = status15_ + "\r";
	streams[15].data.assign(final_status.begin(), final_status.end());
	streams[15].index = 0;

	LOG_DEBUG("STATUS15 set: '%s'", status15_.c_str());
}

// -----------------------------------------------------------------------------
// Command-channel handler (CMD15) - mostly directory / scratch / rename
// -----------------------------------------------------------------------------

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
	auto ltrim = [](std::string& x) {
		while (!x.empty() && (x.front() == ' ' || x.front() == '\r' || x.front() == '\n'))
			x.erase(0, 1);
		};
	auto rtrim = [](std::string& x) {
		while (!x.empty() && (x.back() == ' ' || x.back() == '\r' || x.back() == '\n'))
			x.pop_back();
		};
	ltrim(raw);
	rtrim(raw);

	LOG_DEBUG("CMD15 trimmed hex=[%s] text='%s'",
		dump_hex_str(raw).c_str(), raw.c_str());

	if (raw.empty()) {
		LOG_WARN("CMD15 empty after trim");
		return false;
	}

	// Uppercase copy for verb detection
	std::string up = raw;
	for (auto& c : up) {
		if (c >= 'a' && c <= 'z')
			c = (char)(c - 32);
	}

	LOG_DEBUG("CMD15 upper hex=[%s] text='%s'",
		dump_hex_str(up).c_str(), up.c_str());

	// Verb = first char; an optional drive digit (0/1) may precede the ':'.
	const char verb = up[0];

	// Substring after "X:" / "X0:" / "X1:"; empty if there is no ':'.
	auto arg_after_verb = [&](void) -> std::string {
		size_t pos = 1;
		if (pos < up.size() && (up[pos] == '0' || up[pos] == '1')) ++pos;
		if (pos < up.size() && up[pos] == ':') return up.substr(pos + 1);
		return std::string();
		};
	// Split "new=old" -> {normalized new, normalized old (single source)}.
	auto split_eq = [&](std::string& dst, std::string& src) -> bool {
		const std::string a = arg_after_verb();
		const size_t eq = a.find('=');
		if (eq == std::string::npos) return false;
		dst = normalize_cmd_name(a.substr(0, eq));
		std::string s = a.substr(eq + 1);
		const size_t comma = s.find(',');         // ignore concat sources (rare)
		if (comma != std::string::npos) s.erase(comma);
		src = normalize_cmd_name(s);
		return !dst.empty() && !src.empty();
		};

	switch (verb) {
	case 'S': { // SCRATCH (delete); wildcards allowed: "S:NAME", "S0:AB*"
		size_t pos = 1;
		if (pos < up.size() && (up[pos] == '0' || up[pos] == '1')) ++pos;
		if (pos >= up.size() || up[pos] != ':') {
			LOG_WARN("CMD15: 'S' missing ':' (got '%s')", up.c_str());
			set_status(30, "SYNTAX ERROR", 0, 0);
			return false;
		}
		std::string arg = up.substr(pos + 1);
		rtrim(arg);

		std::vector<std::string> tokens;
		size_t start = 0;
		while (start <= arg.size()) {
			const size_t comma = arg.find(',', start);
			const std::string one = (comma == std::string::npos) ? arg.substr(start)
				: arg.substr(start, comma - start);
			std::string norm = normalize_cmd_name(one);
			if (!norm.empty()) tokens.push_back(norm);
			if (comma == std::string::npos) break;
			start = comma + 1;
		}
		if (tokens.empty()) { set_status(30, "SYNTAX ERROR", 0, 0); return false; }

		const int n = isD64Mounted() ? d64_scratch_patterns(tokens)
			: scratch_host_patterns(tokens);
		LOG_INFO("SCRATCH: patterns=%zu -> deleted %d file(s)", tokens.size(), n);
		set_status(1, "FILES SCRATCHED", (uint8_t)n, 0); // CBM: "01,FILES SCRATCHED,nn,00"
		return true;
	}

	case 'R': { // RENAME: "R:new=old"
		std::string dst, src;
		if (!split_eq(dst, src)) { set_status(30, "SYNTAX ERROR", 0, 0); return false; }
		LOG_INFO("CMD15 RENAME '%s' -> '%s'", src.c_str(), dst.c_str());
		return isD64Mounted() ? d64_rename(src, dst) : host_rename(src, dst);
	}

	case 'C': { // COPY: "C:new=old"
		std::string dst, src;
		if (!split_eq(dst, src)) { set_status(30, "SYNTAX ERROR", 0, 0); return false; }
		LOG_INFO("CMD15 COPY '%s' -> '%s'", src.c_str(), dst.c_str());
		return isD64Mounted() ? d64_copy(src, dst) : host_copy(src, dst);
	}

	case 'N': { // NEW / HEADER: "N:diskname,id" (id optional = keep existing)
		const std::string a = arg_after_verb();
		std::string nm = a, id;
		const size_t comma = a.find(',');
		if (comma != std::string::npos) { nm = a.substr(0, comma); id = a.substr(comma + 1); }
		nm = normalize_cmd_name(nm);
		id = normalize_cmd_name(id);
		LOG_INFO("CMD15 NEW name='%s' id='%s'", nm.c_str(), id.c_str());
		if (isD64Mounted()) return d64_format(nm, id);
		set_status(0, "OK", 0, 0); // host folder is not a formatted disk
		return true;
	}

	case 'I': // INITIALIZE: HLE re-reads the BAM per op, so this is a no-op.
		LOG_INFO("CMD15 INITIALIZE");
		set_status(0, "OK", 0, 0);
		return true;

	case 'V': // VALIDATE / COLLECT: rebuild BAM from the directory chains.
		LOG_INFO("CMD15 VALIDATE");
		if (isD64Mounted()) return d64_validate();
		set_status(0, "OK", 0, 0);
		return true;

	default:
		LOG_WARN("CMD15 unrecognized verb: '%s'", up.c_str());
		set_status(31, "SYNTAX ERROR", 0, 0);
		return false;
	}
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

	// NOTE: do NOT clear the mounted media here. hostRoot / d64Path / d64 are
	// peripheral state (the connected virtual drive and the disk in it), not
	// CPU/machine state. A real PET reset does not unplug device 8 or eject the
	// disk, so the mount must survive g_pet->reset(). Clearing it here made the
	// virtual drive go dead after the first reset (LOAD"$",8 -> FILE NOT FOUND,
	// hostRoot=""). The mount is established once in emu_init and replaced only
	// by an explicit setHostRoot()/setD64Image() call.

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
			// WARNING: This creates high log traffic; disable if too verbose
			//LOG_DEBUG("HS NDAC rise: byte accepted, idx now %zu of %zu", (size_t)data_index, load_data.size());

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
		// Global UNLISTEN trace, before per-state handling.
		if (d8 == 0x3F) {
			LOG_DEBUG("IEEE UNLISTEN seen: state=%d filename=\"%s\" last_listen_sa=%u",
				(int)state, filename.c_str(), (unsigned)last_listen_sa);
		}
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
				///////////////////////////////////////////////////////////////////////////////////////////////
				else {
					// Fall back to single-shot LOAD by filename if available
					if (!filename.empty()) {
						std::vector<uint8_t> pay;
						uint16_t addr = 0x0801;
						const std::string nameClean = normalize_name_for_prg(filename);

						// ---------------------------------------------------------------------
						// Directory: "$" or "$..." after optional drive prefix
						// ---------------------------------------------------------------------
						if (!nameClean.empty() && nameClean[0] == '$') {
							// Optional listing filter after '$': "$:A*", "$0:A*", "$A*".
							std::string dirPat = nameClean.substr(1); // drop '$'
							if (dirPat.size() >= 2 && (dirPat[0] == '0' || dirPat[0] == '1') && dirPat[1] == ':')
								dirPat.erase(0, 2);
							else if (!dirPat.empty() && dirPat[0] == ':')
								dirPat.erase(0, 1);

							std::vector<uint8_t> dirprg;
							bool ok = false;
							if (isD64Mounted()) ok = buildDirectoryPRG_D64(dirprg, 0x0401, dirPat);
							else                ok = buildDirectoryPRG_Folder(dirprg, 0x0401, dirPat);

							if (ok && dirprg.size() >= 2) {
								addr = (uint16_t)dirprg[0] | ((uint16_t)dirprg[1] << 8);
								pay.assign(dirprg.begin() + 2, dirprg.end());

								LOG_DEBUG(
									"DIR LOAD OK: addr=%04X size=%zu isD64=%d",
									(unsigned)addr, pay.size(), isD64Mounted() ? 1 : 0
								);
								set_status(0, "OK", 0, 0);  // Directory OK

								// Arm directory as a TALK stream
								ieeeLoadData(addr, pay);
								state = STATE_LOAD;
								dav_i = true;
								eoi_i = true;
								hs_ = HS_WAIT_NRFD_H;
								last_nrfd_ = nrfd_o;
								last_ndac_ = ndac_o;
								hs_debug("ARM (DIR)");
							}
							else {
								// Directory build failed -> standard CBM error 62
								not_found_handler("DIR", nameClean, 0x0801);
							}
						}

						// ---------------------------------------------------------------------
						// Regular PRG: "NAME"
						// ---------------------------------------------------------------------
						else if (!nameClean.empty()) {
							bool ok = false;
							if (isD64Mounted()) ok = loadHostPRG_D64(nameClean, pay, addr);
							else                ok = loadHostPRG_Folder(nameClean, pay, addr);

							if (ok) {
								LOG_DEBUG(
									"PRG LOAD OK: name=\"%s\" addr=%04X size=%zu hostRoot=\"%s\" isD64=%d",
									nameClean.c_str(), (unsigned)addr, pay.size(),
									hostRoot.c_str(), isD64Mounted() ? 1 : 0
								);
								set_status(0, "OK", 0, 0);  // Successful LOAD

								ieeeLoadData(addr, pay);
								state = STATE_LOAD;
								dav_i = true;
								eoi_i = true;
								hs_ = HS_WAIT_NRFD_H;
								last_nrfd_ = nrfd_o;
								last_ndac_ = ndac_o;
								hs_debug("ARM (PRG)");
							}
							else {
								// File Not Found -> standard CBM error 62
								not_found_handler("PRG", nameClean, addr);
							}
						}
					}
				}
				//////////////////////////////////////////////////////
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
			// NOTE: there used to be an "old PET ROM SAVE" branch here keyed on
			// d8 == 0x3F, but the UNLISTEN case (d8 == 0x3F) above already
			// consumes every 0x3F in STATE_IDLE, so it was unreachable. It was
			// the only path that set STATE_SAVE1, which is therefore now dead.
			break;

		case STATE_LISTEN:
			if (d8 == 0x3F) { // UNLISTEN
				// Execute command channel text if we were receiving it
				LOG_DEBUG("IEEE STATE_LISTEN: UNLISTEN, cmd15_buf_ size=%zu, filename=\"%s\"",
					(size_t)cmd15_buf_.size(), filename.c_str());

				if (!cmd15_buf_.empty()) {
					process_command_channel_string(cmd15_buf_);
					cmd15_buf_.clear();
				}
				// Keep SEQ open; commit happens on CLOSE SA.
				state = STATE_IDLE;
			}
			else if (d8 == 0x61) {
				// Legacy SAVE (PRG SAVE on SA=1) - this MUST be checked
				// BEFORE the 0x60-0x6F LISTEN-secondary branch, or it will
				// never trigger.
				LOG_DEBUG("IEEE LISTEN: SAVE command (0x61) while ATN low: "
					"state=%d last_listen_sa=%u filename=\"%s\"",
					(int)state, (unsigned)last_listen_sa, filename.c_str());

				save_data.clear();
				data_index = 0;
				state = STATE_SAVE;
			}
			else if (d8 >= 0x60 && d8 <= 0x6F) {
				// LISTEN-secondary (data channel select) for SEQ / other SAs
				last_listen_sa = (uint8_t)(d8 & 0x0F);
				LOG_DEBUG("IEEE STATE_LISTEN: LISTEN-secondary 0x%02X -> SA=%u",
					d8, (unsigned)last_listen_sa);
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
					LOG_DEBUG("IEEE FNAME finalize (LOAD/SAVE): sa=%u name=\"%s\" len=%zu",
						(unsigned)sa, filename.c_str(), (size_t)filename.size());
					tracef("FNAME finalize on SA=%u (LOAD/SAVE path) name=\"%s\"", sa, filename.c_str());
					state = STATE_IDLE;
					break;
				}
				// Correct SA routing:
				//  SA  0 = command/status
				//  SA  1 = PRG SAVE / LOAD
				//  SA  2..15 = SEQ channels

				if (filename.empty()) {
					// CLOSE channel
					if (sa >= 2 && sa <= 15) {
						streams[sa].reset();
						seq_write_active_[sa] = false;
						seq_write_buf_[sa].clear();
						seq_write_name_[sa].clear();
						tracef("CLOSE SEQ channel SA=%u", sa);
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
				if (isD64Mounted() && sa >= 2 && sa <= 15) {
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
						set_status(0, "OK", 0, 0);  // SEQ open OK
					}
					else {
						/*
						tracef("SEQ not found SA=%u \"%s\"", sa, filename.c_str());
						LOG_WARN("SEQ LOAD FAILED: SA=%u name=\"%s\" isD64=%d",
							sa, filename.c_str(), isD64Mounted() ? 1 : 0
						);

						// 62,FILE NOT FOUND,00,00
						set_status(62, "FILE NOT FOUND", 0, 0);

						// ----------------------------------------------------------
						// MATCH PRG & DIRECTORY NOT-FOUND BEHAVIOR:
						//
						// SEQ READ must STILL provide a valid TALK data stream,
						// consisting of exactly ONE BYTE plus EOI.
						//
						// Otherwise the PET hangs in its GET# / INPUT# loop waiting
						// for a data byte and an EOI from the drive.
						// ----------------------------------------------------------

						std::vector<uint8_t> nfPay = buildPrgNotFoundStub();

						// SEQ files do not include a header or load address;
						// use 0x0000. The PET ignores it for SEQ.
						uint16_t nfAddr = 0x0000;

						ieeeLoadData(nfAddr, nfPay);
						state = STATE_LOAD;
						dav_i = true;
						eoi_i = true;
						hs_ = HS_WAIT_NRFD_H;
						last_nrfd_ = nrfd_o;
						last_ndac_ = ndac_o;

						hs_debug("ARM (SEQ NOT FOUND stub)");
						*/
						not_found_handler("SEQ", clean, 0x0000);
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
		{
			// Address phase: UNLISTEN marks end of modern KERNAL SAVE.
			if (d8 == 0x3F) { // UNLISTEN -> SAVE complete
				const bool isD64 = isD64Mounted();
				LOG_DEBUG(
					"IEEE PRG SAVE UNLISTEN: state=%d last_listen_sa=%u filename=\"%s\" "
					"raw_bytes=%zu isD64=%d",
					(int)state,
					(unsigned)last_listen_sa,
					filename.c_str(),
					save_data.size(),
					isD64 ? 1 : 0
				);

				// Normalize the SAVE name the same way LOAD does: strip quotes,
				// "@"/"@0:" save-with-replace, "0:"/"1:" drive prefix and ",P,W"
				// mode tokens. Without this, SAVE"0:NAME" / SAVE"@0:NAME" stored a
				// garbled name that LOAD"NAME" could never find.
				const std::string saveNorm = normalize_name_for_prg(filename);
				const std::string& saveName = saveNorm.empty() ? filename : saveNorm;

				if (isD64) {
					// For D64, the KERNAL has already included the load address.
					std::vector<uint8_t> out(save_data.begin(), save_data.end());
					LOG_DEBUG("IEEE PRG SAVE -> D64: name=\"%s\" size=%zu",
						saveName.c_str(), out.size());
					d64_save_prg(saveName, out);
				}
				else {
					// Virtual drive: write exact PRG stream as received.
					LOG_DEBUG("IEEE PRG SAVE -> folder: name=\"%s.prg\" size=%zu",
						saveName.c_str(), save_data.size());
					saveFile(saveName, save_data);
				}

				state = STATE_IDLE;
			}
			break;
		}

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
		// Data phase (ATN high) - controller streams bytes
		// -----------------------
		if (d8 == 0x61) {
			LOG_DEBUG("IEEE WARNING: got 0x61 (SAVE) while ATN HIGH, state=%d last_listen_sa=%u",
				(int)state, (unsigned)last_listen_sa);
		}
		switch (state) {
		case STATE_FNAME:
			// OPEN/CLOSE text bytes (terminated by UNLISTEN in address phase)
			filename.push_back((char)d8);
			LOG_DEBUG("IEEE FNAME byte: sa=%u char=%02X '%c' len=%zu",
				(unsigned)last_listen_sa, d8, dbg_printable(d8),
				(size_t)filename.size());
			break;

		case STATE_LISTEN:
			// PRINT# data bytes for the currently selected LISTEN SA
			if (last_listen_sa >= 2 && last_listen_sa <= 15) {
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
		{
			// Legacy SAVE data. Also mirror into SEQ buffer if armed on this SA.
			save_data.push_back((char)d8);
			data_index++;

			LOG_DEBUG("IEEE STATE_SAVE data: byte=%02X idx=%zu total=%zu last_listen_sa=%u",
				d8,
				(size_t)data_index,
				(size_t)save_data.size(),
				(unsigned)last_listen_sa);

			if (last_listen_sa >= 2 && last_listen_sa <= 15) {
				const uint8_t sa = last_listen_sa;
				if (sa == 15) {
					cmd15_buf_.push_back((char)d8);
				}
				else {
					accept_byte_for_possible_seq_write(sa, d8);
				}
			}
			break;
		}

		default:
			break;
		}
	}
}

// -----------------------------------------------------------------------------
// arm_not_found_stub
// Arm a one-byte IEEE TALK response so the PET KERNAL terminates cleanly.
// This is used for PRG, DIR, and SEQ missing file cases.
//
// type   - "PRG", "DIR", "SEQ", etc. (for logging/debug)
// name   - filename attempted
// addr   - load address to pass to ieeeLoadData()
//          (ignored for SEQ; 0x0000 is safe; PRG uses 0x0801; DIR uses 0x0401)
// -----------------------------------------------------------------------------
void PetIEEE::not_found_handler(const char* type, const std::string& name, uint16_t addr)
{
	LOG_ERROR(
		"%s LOAD FAILED: name=\"%s\" hostRoot=\"%s\" isD64=%d",
		type,
		name.c_str(),
		hostRoot.c_str(),
		isD64Mounted() ? 1 : 0
	);

	// 62,FILE NOT FOUND,00,00
	set_status(62, "FILE NOT FOUND", 0, 0);

	// Tiny 1-byte stub payload for the TALK handshake
	static const std::vector<uint8_t> nfPay = { 0x00 };
	ieeeLoadData(addr, nfPay);

	state = STATE_LOAD;
	dav_i = true;
	eoi_i = true;
	hs_ = HS_WAIT_NRFD_H;
	last_nrfd_ = nrfd_o;
	last_ndac_ = ndac_o;

	char buf[64];
	snprintf(buf, sizeof(buf), "ARM (%s NOT FOUND stub)", type);
	hs_debug(buf);
}