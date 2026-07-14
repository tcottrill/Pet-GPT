// ieee_helpers.h
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <iterator>

// If you want the PET-side helper too:
#include "pet_machine.h"

namespace ieee_helpers {

    // -----------------------------------------------------------------------------
    // Basic file helpers
    // -----------------------------------------------------------------------------
    inline bool read_all_file(const std::filesystem::path& p,
        std::vector<uint8_t>& out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        out.assign(std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>());
        return !out.empty();
    }

    inline bool write_all_file(const std::filesystem::path& p,
        const std::vector<uint8_t>& bytes)
    {
        std::ofstream f(p, std::ios::binary);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        return static_cast<bool>(f);
    }

    // If you still want the external loader:
    inline bool LoadPrgIntoIEEE(PetMachine& m,
        const std::string& prgPath,
        uint16_t fallbackAddr = 0x0801)
    {
        std::vector<uint8_t> file;
        if (!read_all_file(prgPath, file)) {
            std::cerr << "LoadPrgIntoIEEE: can't read " << prgPath << "\n";
            return false;
        }

        uint16_t addr = fallbackAddr;
        size_t payload_off = 0;
        if (file.size() >= 2) {
            addr = static_cast<uint16_t>(file[0]) |
                (static_cast<uint16_t>(file[1]) << 8);
            payload_off = 2;
        }

        std::vector<uint8_t> payload(file.begin() + payload_off, file.end());
        m.bus().io().ieeeLoadData(addr, payload);

        std::cout << "[IEEE] Primed " << payload.size()
            << " bytes at $" << std::hex << addr << std::dec
            << " - now type:  LOAD\"\",8  then  RUN\n";
        return true;
    }

    // -----------------------------------------------------------------------------
    // Hex / ASCII helpers
    // -----------------------------------------------------------------------------
    inline char dbg_printable(uint8_t c)
    {
        return (c >= 32 && c <= 126) ? static_cast<char>(c) : '.';
    }

    inline std::string dump_hex(const uint8_t* p, size_t n)
    {
        static const char* hexd = "0123456789ABCDEF";
        std::string out;
        out.reserve(n * 3);
        for (size_t i = 0; i < n; ++i) {
            uint8_t b = p[i];
            out.push_back(hexd[b >> 4]);
            out.push_back(hexd[b & 0xF]);
            if (i + 1 < n) out.push_back(' ');
        }
        return out;
    }

    inline std::string dump_ascii(const uint8_t* p, size_t n)
    {
        std::string out;
        out.reserve(n);
        for (size_t i = 0; i < n; ++i)
            out.push_back(dbg_printable(p[i]));
        return out;
    }

    inline std::string dump_hex_str(const std::string& s)
    {
        return dump_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    // Dump the 16-byte PETSCII dir name (raw hex + naive ASCII view)
    static void log_dirent_name_hex_ascii(const uint8_t name16[16]) {
        std::string hex = dump_hex(name16, 16);
        std::string asc; asc.reserve(16);
        for (int i = 0; i < 16; ++i) asc.push_back(dbg_printable(name16[i]));
        LOG_DEBUG("DIR NAME raw16 hex=[%s] ascii='%s'", hex.c_str(), asc.c_str());
    }

    inline std::string dir_name_to_ascii(const uint8_t* p16)
    {
        int len = 16;
        while (len > 0 && p16[len - 1] == 0xA0) len--;
        std::string s;
        s.reserve(len);
        for (int i = 0; i < len; ++i)
            s.push_back(static_cast<char>(p16[i]));
        return s;
    }

    // -----------------------------------------------------------------------------
    // Type / formatting helpers for directory listing
    // -----------------------------------------------------------------------------
    inline const char* base_type(uint8_t tcode)
    {
        switch (tcode & 0x0F) {
        case 1: return "SEQ";
        case 2: return "PRG";
        case 3: return "USR";
        case 4: return "REL";
        default: return "DEL";
        }
    }

    inline std::string render_type_token(uint8_t ftype, uint8_t recLenIfREL)
    {
        bool closed = (ftype & 0x80) != 0;
        bool locked = (ftype & 0x40) != 0;
        const char* core = base_type(ftype);
        std::string out;

        if (!closed) out.push_back('*');
        out += core;
        if (locked) out.push_back('<');

        if ((ftype & 0x0F) == 4) {
            char buf[8];
            std::snprintf(buf, sizeof(buf), ",%d", (int)recLenIfREL);
            out += buf;
        }
        return out;
    }

    inline std::string fmt_blocks_3(int n)
    {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%3d", n);
        return std::string(buf);
    }

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

        // Save-with-replace: strip a leading '@' and an optional "@:" current-
        // drive colon (leaving "@0:"/"@1:" for the drive-prefix block below).
        // Without this, OPEN"@0:NAME,S,W" stored a file literally named
        // "@0:NAME" instead of replacing NAME - the SEQ twin of the PRG-name bug.
        if (!s.empty() && s.front() == '@') {
            s.erase(0, 1);
            if (!s.empty() && s.front() == ':') s.erase(0, 1);
        }

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

        // Uppercase ASCII A-Z
        for (auto& ch : s) {
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
        }

        return s;
    }

} // namespace ieee_helpers
