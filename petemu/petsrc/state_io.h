// -----------------------------------------------------------------------------
// state_io.h
// Save-state file helpers: a little-endian writer/reader pair and a parsed
// container. Header only, no dependencies beyond the C++ standard library.
//
// File layout: "PETGPTST", u32 format version, then chunks of
//   { 4-char tag, u32 payload length, payload }.
// A reader ignores chunks it does not know, so newer files stay readable as
// long as the chunks it needs keep their layout.
//
// Reading never throws: a read past the end (or any other misuse) sets a sticky
// failure flag and returns zero values, so a loader can read a whole payload and
// check failed() once.
// -----------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace petstate {
constexpr char kMagic[8] = {'P', 'E', 'T', 'G', 'P', 'T', 'S', 'T'};
constexpr uint32_t kFormatVersion = 1;
}

// CRC-32 (the zlib polynomial), bit by bit: only used on 64 KB at save/load time.
inline uint32_t stateCrc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
	crc = ~crc;
	for (size_t i = 0; i < size; ++i) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
	}
	return ~crc;
}

class StateWriter {
public:
	explicit StateWriter(std::vector<uint8_t>& out) : out_(out) {}

	void u8(uint8_t v) { out_.push_back(v); }
	void u16(uint16_t v) { u8(uint8_t(v)); u8(uint8_t(v >> 8)); }
	void u32(uint32_t v) { u16(uint16_t(v)); u16(uint16_t(v >> 16)); }
	void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
	void i32(int32_t v) { u32(uint32_t(v)); }
	void flag(bool v) { u8(v ? 1 : 0); }
	void bytes(const void* data, size_t size) {
		const auto* p = static_cast<const uint8_t*>(data);
		out_.insert(out_.end(), p, p + size);
	}
	void str(const std::string& s) { u32(uint32_t(s.size())); bytes(s.data(), s.size()); }

	// "PETGPTST" + format version; call once, first.
	void header() { bytes(petstate::kMagic, sizeof petstate::kMagic); u32(petstate::kFormatVersion); }

	// A chunk's length is patched in by endChunk(). Chunks do not nest.
	void beginChunk(const char tag[4]) {
		bytes(tag, 4);
		lengthAt_ = out_.size();
		u32(0);
	}
	void endChunk() {
		const size_t length = out_.size() - lengthAt_ - 4;
		for (int i = 0; i < 4; ++i) out_[lengthAt_ + size_t(i)] = uint8_t(length >> (8 * i));
	}

private:
	std::vector<uint8_t>& out_;
	size_t lengthAt_ = 0;
};

// One chunk of a file: the tag and a view of the payload (valid while the
// buffer it points into is).
struct StateChunkRef {
	char tag[5] = {};
	const uint8_t* data = nullptr;
	size_t size = 0;
};

class StateReader {
public:
	StateReader() = default;
	StateReader(const uint8_t* data, size_t size) : p_(data), end_(data + size) {}

	bool failed() const { return failed_; }
	size_t remaining() const { return size_t(end_ - p_); }
	bool atEnd() const { return p_ == end_; }
	void fail() { failed_ = true; p_ = end_; }

	uint8_t u8() { if (remaining() < 1) { fail(); return 0; } return *p_++; }
	uint16_t u16() { const uint16_t lo = u8(); return uint16_t(lo | (uint16_t(u8()) << 8)); }
	uint32_t u32() { const uint32_t lo = u16(); return lo | (uint32_t(u16()) << 16); }
	uint64_t u64() { const uint64_t lo = u32(); return lo | (uint64_t(u32()) << 32); }
	int32_t i32() { return int32_t(u32()); }
	// Any value but 0 and 1 is a corrupt file.
	bool flag() { const uint8_t v = u8(); if (v > 1) fail(); return v == 1; }
	bool bytes(void* dst, size_t size) {
		if (remaining() < size) { fail(); return false; }
		if (size) std::memcpy(dst, p_, size);
		p_ += size;
		return true;
	}
	std::string str(size_t maxLength = 1u << 16) {
		const uint32_t length = u32();
		if (length > maxLength || remaining() < length) { fail(); return {}; }
		std::string s(reinterpret_cast<const char*>(p_), length);
		p_ += length;
		return s;
	}
	void skip(size_t size) { if (remaining() < size) fail(); else p_ += size; }

	// Magic and version; false (and failed()) when this is not a state file or
	// it comes from a newer format than this program knows.
	bool header(uint32_t* version = nullptr) {
		char magic[sizeof petstate::kMagic] = {};
		if (!bytes(magic, sizeof magic) || std::memcmp(magic, petstate::kMagic, sizeof magic) != 0) { fail(); return false; }
		const uint32_t v = u32();
		if (version) *version = v;
		if (failed_ || v == 0 || v > petstate::kFormatVersion) { fail(); return false; }
		return true;
	}

	// The next chunk, without copying. False at a clean end of data, and also
	// (with failed() set) when the chunk header or payload is cut short.
	bool nextChunk(StateChunkRef& chunk) {
		if (atEnd()) return false;
		char tag[4] = {};
		if (!bytes(tag, 4)) return false;
		const uint32_t length = u32();
		if (failed_ || remaining() < length) { fail(); return false; }
		std::memcpy(chunk.tag, tag, 4); chunk.tag[4] = 0;
		chunk.data = p_; chunk.size = length;
		p_ += length;
		return true;
	}
	// Drops the next chunk whatever it is.
	bool skipChunk() { StateChunkRef ignored; return nextChunk(ignored); }

private:
	const uint8_t* p_ = nullptr;
	const uint8_t* end_ = nullptr;
	bool failed_ = false;
};

// A whole file, checked for structure (magic, version, every chunk inside the
// buffer, no trailing bytes) but not for content. Chunks may repeat; find()
// returns the first of a tag.
class StateFile {
public:
	StateFile() = default;
	StateFile(const StateFile&) = delete;             // chunks point into bytes_
	StateFile& operator=(const StateFile&) = delete;

	bool parse(std::vector<uint8_t>&& bytes, std::string& error) {
		bytes_ = std::move(bytes);
		chunks_.clear();
		StateReader r(bytes_.data(), bytes_.size());
		if (bytes_.size() < sizeof petstate::kMagic || std::memcmp(bytes_.data(), petstate::kMagic, sizeof petstate::kMagic) != 0) {
			error = "this is not a Pet-GPT save state file";
			return false;
		}
		uint32_t version = 0;
		if (!r.header(&version)) {
			error = version > petstate::kFormatVersion ? "this state was saved by a newer Pet-GPT (format version " + std::to_string(version) + ")"
			                                           : "the state file header is damaged";
			return false;
		}
		StateChunkRef chunk;
		while (r.nextChunk(chunk)) chunks_.push_back(chunk);
		if (r.failed()) {
			error = "the state file is truncated or damaged";
			return false;
		}
		return true;
	}

	bool has(const char* tag) const { return findChunk(tag) != nullptr; }
	// The raw payload of a chunk, for blobs with their own format (the CPU's).
	bool chunkData(const char* tag, const uint8_t*& data, size_t& size) const {
		const StateChunkRef* c = findChunk(tag);
		if (!c) return false;
		data = c->data; size = c->size;
		return true;
	}
	// A reader over one chunk's payload; failed() reader if the chunk is missing.
	StateReader find(const char* tag) const {
		const StateChunkRef* c = findChunk(tag);
		if (!c) { StateReader missing; missing.fail(); return missing; }
		return StateReader(c->data, c->size);
	}

private:
	const StateChunkRef* findChunk(const char* tag) const {
		for (const auto& c : chunks_) if (std::memcmp(c.tag, tag, 4) == 0) return &c;
		return nullptr;
	}
	std::vector<uint8_t> bytes_;
	std::vector<StateChunkRef> chunks_;
};
