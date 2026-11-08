#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

// Read-only cassette transport. TAP v0/v1 records intervals between read
// pulses, not program bytes. Time advances only with PLAY and motor enabled.
class PetTape {
public:
    bool attach(const std::vector<uint8_t>& file, std::string& error) {
        error.clear();
        auto fail = [&](const char* s) { error = s; return false; };
        if (file.size() < 20 || std::memcmp(file.data(), "C64-TAPE-RAW", 12))
            return fail("Invalid TAP header (expected C64-TAPE-RAW).");
        if (file[12] > 1) return fail("Only TAP versions 0 and 1 are supported; half-wave TAP v2 is not supported.");
        uint32_t clock = 0;
        if (file[13] == 3 && file[14] <= 1) clock = 1000000; // PET
        if (file[13] == 0) { // Legacy images were usually timed on a C64.
            const uint32_t clocks[] = {985248, 1022730, 1022730, 1023440};
            if (file[14] < 4) clock = clocks[file[14]];
        }
        if (!clock) return fail("Unsupported TAP machine or video standard (use a PET-compatible PET/C64 capture).");
        uint32_t size = uint32_t(file[16]) | (uint32_t(file[17]) << 8) |
            (uint32_t(file[18]) << 16) | (uint32_t(file[19]) << 24);
        if (!size || size != file.size()-20) return fail("Empty TAP or incorrect TAP data size.");
        std::vector<uint32_t> pulses;
        uint64_t remainder = 0;
        for (size_t p = 20; p < file.size();) {
            uint32_t cycles = uint32_t(file[p++]) * 8;
            if (!cycles) {
                if (!file[12]) cycles = 2048; // v0 overflow: duration not recorded
                else {
                    if (file.size()-p < 3) return fail("Truncated TAP extended pulse.");
                    cycles = uint32_t(file[p]) | (uint32_t(file[p+1]) << 8) | (uint32_t(file[p+2]) << 16);
                    p += 3;
                    if (!cycles) return fail("TAP contains a zero-duration pulse.");
                }
            }
            // Preserve elapsed time across rounding when converting capture clocks.
            const uint64_t scaled = uint64_t(cycles) * 1000000 + remainder;
            uint32_t ticks = uint32_t(scaled / clock);
            remainder = scaled % clock;
            if (!ticks) return fail("TAP pulse is shorter than one PET cycle.");
            pulses.push_back(ticks);
        }
        pulses_ = std::move(pulses);
        rewind();
        return true;
    }
    void play() { if (mounted() && !atEnd()) playing_ = true; }
    void stop() { playing_ = false; }
    void rewind() { stop(); position_ = 0; remaining_ = mounted() ? pulses_[0] : 0; }
    void eject() { pulses_.clear(); rewind(); }
    bool mounted() const { return !pulses_.empty(); }
    bool playing() const { return playing_; }
    bool atEnd() const { return mounted() && position_ == pulses_.size(); }
    size_t position() const { return position_; }
    // A true result is one cassette read event on this CPU cycle.
    bool tick(bool motor) {
        if (!playing_ || !motor || !remaining_) return false;
        if (--remaining_) return false;
        ++position_;
        if (position_ == pulses_.size()) stop();
        else remaining_ = pulses_[position_];
        return true;
    }
private:
    std::vector<uint32_t> pulses_;
    size_t position_ = 0;
    uint32_t remaining_ = 0;
    bool playing_ = false;
};
