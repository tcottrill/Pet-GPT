// Diagnostic: what does "A Bright Shining Star" write to the VIA PCR ($E84C)?
// Boots a 2001N (BASIC 2), injects the exported demo PRG at its load address,
// types SYS 1040, then samples PCR at scanline granularity and reports every
// change with its beam position.
//
// Usage: demo_charset_diag.exe <romdir> <demo.prg> [<d64path>]
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>

#include "pet_machine.h"
#include "pet2001io.h"
#include "sys_log.h"

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

int main(int argc, char** argv) {
    const std::string romdir = argc > 1 ? argv[1] : "..\\..\\x64\\Release\\roms\\basic2";
    const std::string prgp   = argc > 2 ? argv[2] : "..\\..\\x64\\Release\\files\\demo.prg";
    const std::string d64p   = argc > 3 ? argv[3] : "";

    PetMachine m;
    m.bus().setRamSize(32 * 1024);

    struct { const char* f; uint16_t base; } roms[] = {
        { "basic-2-c000.901465-01.bin", 0xC000 },
        { "basic-2-d000.901465-02.bin", 0xD000 },
        { "edit-2-n.901447-24.bin",     0xE000 },
        { "kernal-2.901465-03.bin",     0xF000 },
    };
    std::vector<uint8_t> rom;
    for (auto& r : roms) {
        if (!read_file(romdir + "\\" + r.f, rom)) { std::printf("FAIL: rom %s\n", r.f); return 1; }
        if (!m.loadRom(rom.data(), rom.size(), r.base)) { std::printf("FAIL: install %s\n", r.f); return 1; }
    }
    // Character ROM: single 2KB part, graphics bank + text bank.
    static std::vector<uint8_t> charRom, csGfx, csTxt;
    if (read_file(romdir + "\\characters-2.901447-10.bin", charRom) && charRom.size() >= 0x800) {
        csGfx.assign(charRom.begin(), charRom.begin() + 0x400);
        csTxt.assign(charRom.begin() + 0x400, charRom.begin() + 0x800);
        m.setVideoCharsets(csGfx.data(), csTxt.data());
    }
    m.reset();

    bool booted = false;
    for (int f = 0; f < 600 && !booted; ++f) { frame(m); booted = count_ready(m) > 0; }
    if (!booted) { std::printf("FAIL: no READY\n"); return 1; }
    std::printf("booted.\n");

    if (!d64p.empty()) {
        if (!m.io().setIeeeD64Image(d64p)) std::printf("warn: d64 mount failed\n");
        else std::printf("d64 mounted.\n");
    }

    // Inject the PRG.
    std::vector<uint8_t> prg;
    if (!read_file(prgp, prg) || prg.size() < 3) { std::printf("FAIL: prg %s\n", prgp.c_str()); return 1; }
    const uint16_t load = (uint16_t)(prg[0] | (prg[1] << 8));
    for (size_t i = 2; i < prg.size(); ++i)
        m.bus().writeByte((uint16_t)(load + i - 2), prg[i]);
    std::printf("injected %zu bytes at $%04X\n", prg.size() - 2, load);

    // Type: S Y S space 1 0 4 0 RETURN  (graphics keyboard positions)
    struct { int r, c; } keys[] = {
        {5,0},{3,2},{5,0},{9,2},      // S Y S space
        {6,6},{8,6},{4,6},{8,6},      // 1 0 4 0
        {6,5},                        // RETURN
    };
    for (auto& k : keys) press(m, k.r, k.c);
    std::printf("SYS 1040 typed. sampling PCR...\n");

    // Sample PCR each scanline (64 cycles) for 60 s emulated.
    uint8_t last = m.bus().readByte(0xE84C);
    std::printf("PCR at start: %02X\n", last);
    long cyc = 0;
    int changes = 0;
    bool probed = false;
    const long FRAME = 16640;
    for (long line = 0; line < 60L * 60L * 260L; ++line) {
        m.runCycles(64); cyc += 64;
        uint8_t p = m.bus().readByte(0xE84C);
        if (p != last) {
            ++changes;
            if (changes <= 80)
                std::printf("PCR %02X -> %02X  frame=%ld line=%ld  (CA2 mode=%d%d%d)\n",
                    last, p, cyc / FRAME, (cyc % FRAME) / 64,
                    (p >> 3) & 1, (p >> 2) & 1, (p >> 1) & 1);
            last = p;
        }
        if (changes == 80) { std::printf("... (further changes counted silently)\n"); ++changes; }

        // Once, while the demo has its "lowercase" PCR active: draw screen
        // code 1 at the top-left cell and see which glyph came out. Graphics
        // bank 'A' has lit pixels in glyph row 0; text bank 'a' does not.
        if (!probed && !csGfx.empty() && cyc / FRAME >= 300 && last == 0x12) {
            m.bus().writeByte(0x8000, 0x01);
            const uint32_t* fb = m.video().framebuffer();
            bool topLit = false;
            for (int x = 0; x < 16; ++x)
                if ((fb[x] & 0x00FFFFFF) != 0) { topLit = true; break; }
            std::printf("charset probe @frame %ld (PCR=%02X): code 1 renders as %s\n",
                cyc / FRAME, last, topLit ? "'A' (GRAPHICS bank - WRONG)" : "'a' (TEXT bank - correct)");
            probed = true;
        }
    }
    std::printf("done. total PCR changes seen: %d, final PCR=%02X, PC=$%04X\n",
        changes, last, (unsigned)m.cpu().PC);
    return 0;
}
