// One-off probe: mount a real .d71 through the production backend and dump its
// directory listing (read path) + free count. Usage: d71_probe.exe <path.d71>
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "pet2001ieee.h"
#include "sys_log.h"

namespace Log {
    bool open(const std::string&) { return true; }
    void close() {}
    void write(Level, const char*, const char*, int, const char*, ...) {}
    void setLevel(Level) {}
    void setConsoleOutputEnabled(bool) {}
}

// Reach buildDirectoryPRG_D64 (private) via the existing test seam.
struct PetIEEE_TestAccess {
    static bool build_dir(PetIEEE& e, std::vector<uint8_t>& out, const std::string& m) {
        return e.buildDirectoryPRG_D64(out, 0x0401, m);
    }
};

int main(int argc, char** argv) {
    const std::string path = argc > 1 ? argv[1]
        : "..\\..\\x64\\Release\\files\\stationfall.d71";
    PetIEEE e;
    if (!e.setD64Image(path)) { std::printf("FAIL mount %s\n", path.c_str()); return 1; }
    std::printf("mounted: %s (mounted=%d)\n", path.c_str(), e.isD64Mounted() ? 1 : 0);

    std::vector<uint8_t> dir;
    if (!PetIEEE_TestAccess::build_dir(e, dir, "")) { std::printf("FAIL build dir\n"); return 1; }

    // Decode the PETSCII directory PRG into readable lines (skip BASIC link
    // bytes; print printable chars, newline at each end-of-line 0x00).
    std::printf("--- directory ---\n");
    size_t i = 2;                     // skip load address
    while (i + 1 < dir.size()) {
        const uint16_t nextptr = (uint16_t)(dir[i] | (dir[i + 1] << 8));
        if (nextptr == 0) break;      // 0000 next-line pointer = end of program
        i += 4;                       // skip next-line pointer + line number
        std::string line;
        while (i < dir.size() && dir[i] != 0x00) {
            uint8_t c = dir[i++];
            if (c == 0x12 || c == 0x92) continue;      // RVS on/off
            if (c == 0x14) { if (!line.empty()) line.pop_back(); continue; } // DELETE
            line.push_back((c >= 32 && c < 127) ? (char)c : '.');
        }
        ++i;                          // line terminator
        std::printf("  %s\n", line.c_str());
    }
    return 0;
}
