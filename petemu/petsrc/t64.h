#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <utility>

// T64 directory addresses are exclusive; payloads omit the PRG address prefix.
namespace t64 {
struct Program { std::string name; std::vector<uint8_t> prg; };
inline bool read(const std::vector<uint8_t>& data, std::vector<Program>& programs,
                 std::string& error) {
    programs.clear();
    error.clear();
    auto fail = [&](const char* message) { programs.clear(); error = message; return false; };
    if (data.size() < 64) return fail("Truncated T64 header.");
    if (std::memcmp(data.data(), "C64 tape image file", 19) != 0 &&
        std::memcmp(data.data(), "C64S tape image file", 20) != 0 &&
        std::memcmp(data.data(), "C64S tape file", 14) != 0)
        return fail("Unrecognized T64 signature.");
    auto word = [&](size_t p) { return unsigned(data[p]) | (unsigned(data[p+1]) << 8); };
    auto dword = [&](size_t p) { return uint32_t(word(p)) | (uint32_t(word(p+2)) << 16); };
    const size_t slots = word(34), directoryEnd = 64 + slots * 32;
    if (!slots || directoryEnd > data.size() || word(36) > slots)
        return fail("Invalid or truncated T64 directory.");
    // Scan all slots: deleted entries can leave holes in the directory.
    for (size_t i = 0; i < slots; ++i) {
        const size_t p = 64 + i * 32;
        if (data[p] != 1 || (data[p+1] & 7) != 2) continue;
        const unsigned start = word(p+2), end = word(p+4);
        const size_t offset = dword(p+8);
        if (offset < directoryEnd || offset >= data.size())
            return fail("T64 program has an invalid data offset.");
        size_t limit = data.size();
        for (size_t j = 0; j < slots; ++j) {
            const size_t q = 64 + j * 32;
            if (j == i || !data[q]) continue;
            const size_t other = dword(q+8);
            if (other == offset) return fail("T64 programs share a data offset.");
            if (other > offset && other < limit) limit = other;
        }
        // Older CONV64 archives use $C3C6 instead of a real end address.
        const size_t length = end == 0xC3C6 ? limit - offset :
            (end > start ? end - start : 0);
        if (!length || length > limit-offset || length > 65536u-start)
            return fail("T64 program is truncated or has an invalid address range.");
        Program program;
        for (size_t n = 0; n < 16; ++n) {
            unsigned c = data[p+16+n];
            if (!c) break;
            if (c == 0xA0) c = ' ';
            if (c >= 0xC1 && c <= 0xDA) c -= 0x80;
            program.name += c >= 32 && c <= 126 ? char(c) : '?';
        }
        while (!program.name.empty() && program.name.back() == ' ') program.name.pop_back();
        if (program.name.empty()) program.name = "(unnamed)";
        program.prg.push_back(uint8_t(start));
        program.prg.push_back(uint8_t(start >> 8));
        program.prg.insert(program.prg.end(), data.begin()+offset, data.begin()+offset+length);
        programs.push_back(std::move(program));
    }
    if (programs.empty()) return fail("This T64 contains no supported PRG entries.");
    return true;
}
}
