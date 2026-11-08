// IEEE-488 bus-level LOAD regression test.
//
// The d64 suite covers the DOS layer only; this harness exercises the FULL
// stack the way a user does: boot the KERNAL, mount a real .d64, type
// LOAD"*",8,1 on the emulated keyboard, and byte-compare the RAM the KERNAL
// deposited against the file chain parsed directly from the image. It exists
// because a LOAD"*",8,1 garbage / SEQ-hang regression (2026-07-10) was
// invisible to every unit suite.
//
// Usage: ieee_bus_load_tests.exe <romdir> <d64path>
//   e.g. ieee_bus_load_tests.exe ..\..\x64\Release\roms\basic2 ..\..\x64\Release\files\ADVENTURE.d64

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "pet_machine.h"
#include "pet2001io.h"
#include "sys_log.h"

// --- no-op Log stubs (match the other suites) ---
namespace Log {
    bool open(const std::string&) { return true; }
    void close() {}
    void write(Level, const char*, const char*, int, const char*, ...) {}
    void setLevel(Level) {}
    void setConsoleOutputEnabled(bool) {}
}

static bool read_file(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return !out.empty();
}

// ---------------------------------------------------------------------------
// Minimal D64 reader (independent of the emulator's implementation, so a bug
// there can't hide a bug here). 35-track, byte1-of-last-sector = OFFSET of
// last used byte (data starts at offset 2 -> count = byte1 - 1).
// ---------------------------------------------------------------------------
static int d64_spt(int t) { return t <= 17 ? 21 : t <= 24 ? 19 : t <= 30 ? 18 : 17; }
static size_t d64_off(int t, int s) {
    size_t sec = 0;
    for (int i = 1; i < t; ++i) sec += d64_spt(i);
    return (sec + (size_t)s) * 256;
}

// Extract the FIRST closed PRG file (what LOAD"*" resolves to on a fresh mount).
static bool d64_first_prg(const std::vector<uint8_t>& img, std::vector<uint8_t>& file, std::string& name) {
    int t = 18, s = 1;
    for (int guard = 0; guard < 40 && t != 0; ++guard) {
        const uint8_t* sec = img.data() + d64_off(t, s);
        for (int e = 0; e < 8; ++e) {
            const uint8_t* ent = sec + e * 32;
            if ((ent[2] & 0x87) == 0x82) {                 // closed PRG
                int ft = ent[3], fs = ent[4];
                name.clear();
                for (int i = 0; i < 16 && ent[5 + i] != 0xA0; ++i) name += (char)ent[5 + i];
                file.clear();
                for (int g = 0; g < 1000 && ft != 0; ++g) {
                    const uint8_t* d = img.data() + d64_off(ft, fs);
                    if (d[0] == 0) { file.insert(file.end(), d + 2, d + 2 + (d[1] - 1)); ft = 0; }
                    else { file.insert(file.end(), d + 2, d + 256); ft = d[0]; fs = d[1]; }
                }
                return file.size() > 2;
            }
        }
        t = sec[0]; s = sec[1];
    }
    return false;
}

// ---------------------------------------------------------------------------
// Screen helpers
// ---------------------------------------------------------------------------
static int count_ready(PetMachine& m) {
    static const uint8_t R[6] = { 0x12, 0x05, 0x01, 0x04, 0x19, 0x2E }; // "READY."
    int n = 0;
    for (uint16_t a = 0x8000; a + 6 <= 0x83E8; ++a) {
        bool hit = true;
        for (int k = 0; k < 6; ++k) if (m.bus().readByte((uint16_t)(a + k)) != R[k]) { hit = false; break; }
        if (hit) ++n;
    }
    return n;
}
static bool screen_has_error(PetMachine& m) {
    static const uint8_t E[5] = { 0x05, 0x12, 0x12, 0x0F, 0x12 }; // "ERROR"
    for (uint16_t a = 0x8000; a + 5 <= 0x83E8; ++a) {
        bool hit = true;
        for (int k = 0; k < 5; ++k) if (m.bus().readByte((uint16_t)(a + k)) != E[k]) { hit = false; break; }
        if (hit) return true;
    }
    return false;
}
static void dump_screen(PetMachine& m, int rows) {
    for (int r = 0; r < rows; ++r) {
        char line[41] = {};
        for (int c = 0; c < 40; ++c) {
            uint8_t sc = m.bus().readByte((uint16_t)(0x8000 + r * 40 + c)) & 0x7F;
            line[c] = (sc >= 1 && sc <= 26) ? (char)('A' + sc - 1)
                    : (sc >= 0x30 && sc <= 0x39) ? (char)sc
                    : (sc == 0x20) ? ' '
                    : (sc == 0x2E) ? '.' : (sc == 0x22) ? '"' : (sc == 0x2C) ? ',' : (sc == 0x2A) ? '*' : '?';
        }
        std::printf("  |%s|\n", line);
    }
}

// ---------------------------------------------------------------------------
// Keyboard: feed the PET matrix directly (no host input involved)
// ---------------------------------------------------------------------------
static void frame(PetMachine& m) { m.runCycles(16666); }
static void press(PetMachine& m, int row, int col, int hold = 4, int gap = 4) {
    uint8_t rows[10]; std::memset(rows, 0xFF, 10);
    rows[row] &= (uint8_t)~(1u << col);
    m.io().setKeyrows(rows);
    for (int i = 0; i < hold; ++i) frame(m);
    std::memset(rows, 0xFF, 10);
    m.io().setKeyrows(rows);
    for (int i = 0; i < gap; ++i) frame(m);
}

int main(int argc, char** argv) {
    const std::string romdir = argc > 1 ? argv[1] : "..\\..\\x64\\Release\\roms\\basic2";
    const std::string d64p   = argc > 2 ? argv[2] : "..\\..\\x64\\Release\\files\\ADVENTURE.d64";

    // --- expected file content straight from the image ---
    std::vector<uint8_t> img, file; std::string fname;
    if (!read_file(d64p, img) || img.size() < 174848) { std::printf("FAIL: cannot read d64 %s\n", d64p.c_str()); return 1; }
    if (!d64_first_prg(img, file, fname)) { std::printf("FAIL: no PRG in %s\n", d64p.c_str()); return 1; }
    const uint16_t loadAddr = (uint16_t)(file[0] | (file[1] << 8));
    const size_t payload = file.size() - 2;
    std::printf("expect: \"%s\" -> $%04X, %zu bytes\n", fname.c_str(), loadAddr, payload);

    // --- boot ---
    PetMachine m;
    m.bus().setRamSize(32 * 1024);   // PetMem defaults to 0 = every RAM write dropped
    std::vector<uint8_t> rom;
    struct { const char* f; uint16_t base; } roms[] = {
        { "basic-2-c000.901465-01.bin", 0xC000 }, { "basic-2-d000.901465-02.bin", 0xD000 },
        { "edit-2-n.901447-24.bin",     0xE000 }, { "kernal-2.901465-03.bin",     0xF000 },
    };
    for (auto& r : roms) {
        if (!read_file(romdir + "\\" + r.f, rom) || !m.loadRom(rom.data(), rom.size(), r.base)) {
            std::printf("FAIL: rom %s\n", r.f); return 1;
        }
    }
    m.reset();
    std::printf("probe: vector=$%02X%02X rom[C000]=%02X rom[F000]=%02X\n",
                m.bus().readByte(0xFFFD), m.bus().readByte(0xFFFC),
                m.bus().readByte(0xC000), m.bus().readByte(0xF000));
    bool booted = false;
    for (int f = 0; f < 600 && !booted; ++f) {
        frame(m);
        if (f < 5 || f == 60 || f == 300)
            std::printf("probe: frame %d PC=$%04X\n", f, (unsigned)m.cpu().PC);
        booted = count_ready(m) > 0;
    }
    if (!booted) { std::printf("FAIL: no READY after boot (PC=$%04X)\n", (unsigned)m.cpu().PC); dump_screen(m, 6); return 1; }

    // --- mount + type LOAD"*",8,1 ---
    if (!m.io().setIeeeD64Image(d64p)) { std::printf("FAIL: mount\n"); return 1; }
    const int baselineReady = count_ready(m);

    struct { int r, c; } keys[] = {
        {4,4},{2,4},{4,0},{4,1},      // L O A D
        {1,0},{5,7},{1,0},            // " * "
        {7,3},{3,6},{7,3},{6,6},      // , 8 , 1
        {6,5},                        // RETURN
    };
    for (auto& k : keys) press(m, k.r, k.c);

    // --- run until RAM matches, an error prints, or timeout ---
    const int maxFrames = 3600; // 60 s emulated
    for (int f = 0; f < maxFrames; ++f) {
        frame(m);

        size_t match = 0;
        while (match < payload &&
               m.bus().readByte((uint16_t)(loadAddr + match)) == file[2 + match]) ++match;

        if (match == payload) {
            std::printf("ALL TESTS PASSED  (LOAD\"*\",8,1 delivered %zu/%zu bytes intact, frame %d)\n",
                        match, payload, f);
            return 0;
        }
        if (screen_has_error(m)) {
            std::printf("FAIL: KERNAL printed an ERROR during LOAD (frame %d, %zu/%zu bytes matched)\n",
                        f, match, payload);
            dump_screen(m, 10);
            return 1;
        }
        if (count_ready(m) > baselineReady && f > 60) {
            // LOAD finished (new READY prompt) but RAM doesn't match -> garbage
            std::printf("FAIL: LOAD completed but RAM is corrupt: first mismatch at offset %zu of %zu "
                        "(RAM $%04X = %02X, file byte = %02X)\n",
                        match, payload, (unsigned)(loadAddr + match),
                        m.bus().readByte((uint16_t)(loadAddr + match)), file[2 + match]);
            dump_screen(m, 10);
            return 1;
        }
    }
    size_t match = 0;
    while (match < payload &&
           m.bus().readByte((uint16_t)(loadAddr + match)) == file[2 + match]) ++match;
    std::printf("FAIL: LOAD hung (timeout; %zu/%zu bytes matched)\n", match, payload);
    dump_screen(m, 10);
    return 1;
}
