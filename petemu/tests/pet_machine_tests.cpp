#include "pet_machine.h"
#include <cstdio>
#include <cstring>
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

namespace Log {
bool open(const std::string&) { return true; }
void close() {}
void write(Level, const char*, const char*, int, const char*, ...) {}
void setLevel(Level) {}
void setConsoleOutputEnabled(bool) {}
}
static int failures=0, checks=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n",__LINE__,#x); } } while(0)
static uint8_t chars[1024] = {};
static constexpr uint32_t black=0xff000000;
static void crtc(PetMem& m, int reg, int value) {
    m.writeByte(0xe880,(uint8_t)reg); m.writeByte(0xe881,(uint8_t)value);
}

// ---- Save-state helpers ------------------------------------------------------
// A tiny machine-code program (VIA T1 interrupt, screen writes, CRTC register
// writes and, on the 8296 configuration, expansion-RAM banking), so a state taken
// anywhere in its run has timers, an interrupt and the raster in flight. No ROM
// files are needed: a 4 KB ROM image with the vectors and the IRQ handler is built here.
static std::vector<uint8_t> statesProgram(bool expansion) {
    std::vector<uint8_t> c;
    auto put = [&](std::initializer_list<int> v) { for (int x : v) c.push_back(uint8_t(x)); };
    put({0x78});                                                  // SEI
    if (expansion) put({0xA9,0xE0, 0x8D,0xF0,0xFF});              // LDA #$E0 / STA $FFF0: expansion on, I/O and screen stay visible
    put({0xA9,0x40, 0x8D,0x4B,0xE8,                               // ACR: T1 free-run
         0xA9,0xC0, 0x8D,0x4E,0xE8,                               // IER: T1 interrupt
         0xA9,0xA0, 0x8D,0x44,0xE8,                               // T1 latch low
         0xA9,0x00, 0x8D,0x45,0xE8, 0x58});                       // T1 high (arms), CLI
    const int loop = 0x400 + int(c.size());
    put({0xEE,0x01,0x02, 0xAD,0x01,0x02, 0x8D,0x05,0x80,         // INC $0201 / LDA $0201 / STA $8005
         0x29,0x0F, 0xAA, 0x9D,0x10,0x80,                         // AND #$0F / TAX / STA $8010,X
         0xA9,0x0D, 0x8D,0x80,0xE8,                               // CRTC: select R13
         0x8A, 0x29,0x03, 0x8D,0x81,0xE8});                       // TXA / AND #3 / STA $E881 (screen start low)
    if (expansion) put({0xAD,0x01,0x02, 0x29,0x04, 0x09,0xE0, 0x8D,0xF0,0xFF,   // bank bit 2 of the counter into $FFF0
                        0xAD,0x01,0x02, 0x8D,0x00,0x90});                       // and store through the window
    put({0x4C, loop & 0xFF, loop >> 8});                          // JMP loop
    return c;
}
static void statesSetup(PetMachine& machine, bool cbm8296) {
    static uint8_t rom[4096];
    const uint8_t irq[] = {0x48, 0xAD,0x44,0xE8, 0xEE,0x00,0x02, 0xAD,0x00,0x02, 0x8D,0x02,0x80, 0x68, 0x40};
    memcpy(rom + 0x100, irq, sizeof irq);
    rom[0xFFC] = 0x00; rom[0xFFD] = 0x04;                         // reset -> $0400
    rom[0xFFE] = 0x00; rom[0xFFF] = 0xF1;                         // IRQ -> $F100
    auto& m = machine.bus();
    m.setRamSize(32768);
    machine.setVideoCharsets(chars, chars);
    if (!cbm8296) {
        m.setScreenWindow(false); machine.video().setColumns(40); machine.io().configureCrtc(false, 1);
    } else {
        m.setScreenWindow(true); machine.video().setColumns(80); machine.io().configureCrtc(true, 2);
        m.setExpansion(true); m.setScreenWindow4K(); machine.video().setScreen4K(true);
        crtc(m, 0, 49); crtc(m, 1, 40); crtc(m, 3, 0x24); crtc(m, 4, 40); crtc(m, 5, 5);
        crtc(m, 6, 25); crtc(m, 7, 32); crtc(m, 9, 7);
    }
    m.installROM(0xF000, rom, sizeof rom);
    if (cbm8296) {
        // The program maps expansion RAM over the ROM, so the vectors and the handler must be in it too.
        m.writeByte(0xFFF0, 0xE0);
        for (int i : {0x100,0x101,0x102,0x103,0x104,0x105,0x106,0x107,0x108,0x109,0x10A,0x10B,0x10C,0x10D,0x10E, 0xFFC,0xFFD,0xFFE,0xFFF})
            m.writeByte(uint16_t(0xF000 + i), rom[i]);
        m.writeByte(0xFFF0, 0x00);
    }
    const auto program = statesProgram(cbm8296);
    for (size_t i = 0; i < program.size(); ++i) m.writeByte(uint16_t(0x400 + i), program[i]);
    machine.reset();
}
static std::vector<uint8_t> statesSnapshot(PetMachine& machine) {
    std::vector<uint8_t> bytes; StateWriter w(bytes);
    w.header(); machine.saveState(w);
    return bytes;
}
static bool statesLoad(PetMachine& machine, std::vector<uint8_t> bytes, std::string& error) {
    StateFile file;
    if (!file.parse(std::move(bytes), error)) return false;
    return machine.loadState(file, error);
}
static uint64_t fnv(uint64_t h, const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}
// One entry per instruction: registers, cycle count, IRQ line, VIA IFR, CRTC status and RAM.
static std::vector<uint64_t> statesTrace(PetMachine& machine, int cycles) {
    std::vector<uint64_t> trace;
    int done = 0, step = 0;
    auto& cpu = machine.cpu();
    while (done < cycles) {
        const int c = machine.runCycles(1);
        done += c;
        uint64_t h = 1469598103934665603ULL;
        const uint16_t pc = cpu.get_pc();
        const uint8_t regs[] = {cpu.A, cpu.X, cpu.Y, cpu.S, cpu.P, uint8_t(machine.bus().irqLevel()), uint8_t(c),
            machine.bus().readByte(0xE84D), machine.bus().readByte(0xE880), machine.bus().readByte(0xE844 + 1) };
        h = fnv(h, &pc, sizeof pc); h = fnv(h, regs, sizeof regs);
        const int ticks = cpu.get6502ticks(0); h = fnv(h, &ticks, sizeof ticks);
        h = fnv(h, machine.bus().ramData(), 0x300);
        h = fnv(h, machine.bus().ramData() + 0x8000, 0x1000);
        if ((step++ & 63) == 0) h = fnv(h, machine.bus().ramData(), 65536);
        trace.push_back(h);
    }
    return trace;
}
// Payload offset of a chunk in a file image, or 0.
static size_t statesChunk(const std::vector<uint8_t>& bytes, const char* tag) {
    size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const size_t length = bytes[at + 4] | (bytes[at + 5] << 8) | (bytes[at + 6] << 16) | (size_t(bytes[at + 7]) << 24);
        if (memcmp(&bytes[at], tag, 4) == 0) return at + 8;
        at += 8 + length;
    }
    return 0;
}

int main() {
    memset(chars+8,255,8); // glyph 1 lit; spaces and glyph 0 dark
    {
        // state_io: primitives, chunks, unknown chunks, truncation.
        const uint8_t check123[] = {'1','2','3','4','5','6','7','8','9'};
        CHECK(stateCrc32(check123, sizeof check123) == 0xCBF43926u);
        std::vector<uint8_t> b; StateWriter w(b);
        w.header();
        w.beginChunk("AAAA");
        w.u8(0xAB); w.u16(0xBEEF); w.u32(0xDEADBEEFu); w.u64(0x0123456789ABCDEFull); w.i32(-5);
        w.flag(true); w.flag(false); w.str("h\xC3\xA9llo"); const uint8_t raw[3] = {1, 2, 3}; w.bytes(raw, 3);
        w.endChunk();
        w.beginChunk("ZZZZ"); w.u32(7); w.endChunk();            // a chunk this reader does not know
        w.beginChunk("BBBB"); w.u16(0x1234); w.endChunk();
        CHECK(b[20] == 0xAB && b[21] == 0xEF && b[22] == 0xBE);   // little endian, after 12 + 8 header bytes
        StateFile f; std::string err;
        CHECK(f.parse(std::vector<uint8_t>(b), err));
        StateReader r = f.find("AAAA");
        CHECK(r.u8() == 0xAB); CHECK(r.u16() == 0xBEEF); CHECK(r.u32() == 0xDEADBEEFu);
        CHECK(r.u64() == 0x0123456789ABCDEFull); CHECK(r.i32() == -5);
        CHECK(r.flag() == true); CHECK(r.flag() == false); CHECK(r.str() == "h\xC3\xA9llo");
        uint8_t back[3] = {}; CHECK(r.bytes(back, 3) && back[2] == 3);
        CHECK(r.atEnd() && !r.failed());
        r.u8(); CHECK(r.failed());                                // reading past the end sets the flag
        StateReader z = f.find("BBBB");
        CHECK(z.u16() == 0x1234 && !z.failed());                  // found past the unknown chunk
        CHECK(!f.has("QQQQ")); CHECK(f.find("QQQQ").failed());
        StateReader seq(b.data() + 12, b.size() - 12);
        StateChunkRef c;
        CHECK(seq.skipChunk() && seq.nextChunk(c) && memcmp(c.tag, "ZZZZ", 4) == 0 && c.size == 4);
        CHECK(seq.nextChunk(c) && memcmp(c.tag, "BBBB", 4) == 0 && !seq.nextChunk(c) && !seq.failed());
        uint8_t two = 2; StateReader bad(&two, 1); bad.flag(); CHECK(bad.failed());
        StateReader shortStr(b.data(), 3); shortStr.str(); CHECK(shortStr.failed());
        for (size_t cut : {size_t(0), size_t(5), size_t(11), size_t(13), size_t(19), size_t(25), b.size() - 1}) {
            StateFile g; CHECK(!g.parse(std::vector<uint8_t>(b.begin(), b.begin() + long(cut)), err));
        }
        { auto t = b; t[0] = 'X'; StateFile g; CHECK(!g.parse(std::move(t), err)); }
        { auto t = b; t[8] = 2; StateFile g; CHECK(!g.parse(std::move(t), err) && err.find("newer") != std::string::npos); }
        { auto t = b; t[8] = 0; StateFile g; CHECK(!g.parse(std::move(t), err)); }
        { auto t = b; t[16] = 0xFF; t[17] = 0xFF; StateFile g; CHECK(!g.parse(std::move(t), err)); }   // chunk length beyond the file
    }
    for (const bool cbm8296 : {false, true}) {
        // Whole-machine determinism: save mid-run, load into a NEW machine, both must trace identically.
        PetMachine a;
        statesSetup(a, cbm8296);
        a.runCycles(20000);
        const auto snapshot = statesSnapshot(a);
        const auto first = statesTrace(a, 60000);
        CHECK((a.bus().peekByte(0x0201) | a.bus().peekByte(0x0200)) != 0);   // the program ran and took timer interrupts
        CHECK(first.front() != first.back());
        CHECK(a.bus().expansion() == cbm8296);
        PetMachine b;
        statesSetup(b, cbm8296);
        std::string err;
        CHECK(statesLoad(b, snapshot, err));
        CHECK(statesSnapshot(b) == snapshot);                     // every saved byte came back
        const auto second = statesTrace(b, 60000);
        CHECK(first.size() == second.size());
        CHECK(first == second);

        // Damaged files: refused, and the running machine is byte for byte unchanged.
        const auto before = statesSnapshot(a);
        auto refused = [&](const char* what, std::vector<uint8_t> bytes) {
            std::string e;
            const bool ok = statesLoad(a, std::move(bytes), e);
            ++checks; if (ok || e.empty()) { ++failures; printf("FAIL: %s was not refused with a message\n", what); }
            ++checks; if (statesSnapshot(a) != before) { ++failures; printf("FAIL: %s changed the machine\n", what); }
        };
        refused("truncated file", std::vector<uint8_t>(snapshot.begin(), snapshot.begin() + long(snapshot.size() / 2)));
        refused("foreign file", std::vector<uint8_t>(64, 'x'));
        refused("empty file", {});
        { auto t = snapshot; t[0] = 'Q'; refused("wrong magic", t); }
        { auto t = snapshot; t[8] = 9; refused("newer version", t); }
        { auto t = snapshot; t[statesChunk(t, "VID ") - 8] = 'X'; refused("missing video chunk", t); }
        { auto t = snapshot; t[statesChunk(t, "CPU ")] = '9'; refused("bad CPU blob", t); }
        { auto t = snapshot; t[statesChunk(t, "MEM ") + 2] = 2; refused("RAM size out of range", t); }
        { auto t = snapshot; t[statesChunk(t, "IO  ") + 1] = 7; refused("bad CRTC bytes per character", t); }
        { auto t = snapshot; t[statesChunk(t, "CRTC") + 28] = 0x7F; refused("CRTC counter out of range", t); }
        { auto t = snapshot; t[statesChunk(t, "VID ") + 3] = 0x40; refused("video size out of range", t); }
        { auto t = snapshot; const size_t tape = statesChunk(t, "TAPE"); t[tape] = 1; t[tape + 9] = 5; refused("tape position beyond its image", t); }
    }
    {
        PetMachine machine;
        auto& m=machine.bus(); auto& v=machine.video();
        machine.setVideoCharsets(chars,chars);
        m.writeByte(0x8000,1);
        m.reset();
        CHECK(m.readByte(0x8000)==1);
        CHECK(v.framebuffer()[0]!=black);
        m.setScreenWindow(true); v.setColumns(80);
        machine.io().configureCrtc(true, 2);
        crtc(m,6,25); crtc(m,9,7);
        m.writeByte(0x8000,0); m.writeByte(0x8002,1);
        crtc(m,1,40); crtc(m,13,1);
        CHECK(v.framebuffer()[0]!=black);
        m.writeByte(0x8802,0); // Upper half of the window is open on CRTC PETs.
        CHECK(m.readByte(0x8802)==0xff);
        CHECK(v.framebuffer()[0]!=black);
        m.writeByte(0x8002,0);
        CHECK(v.framebuffer()[0]==black);
        m.writeByte(0x87fe,1); m.writeByte(0x8000,1);
        crtc(m,12,3); crtc(m,13,255); // byte offset 2046, wrap after 2 cells
        CHECK(v.framebuffer()[0]!=black);
        CHECK(v.framebuffer()[16]!=black);
        v.setCharset(true); // full redraw uses the same addressing
        CHECK(v.framebuffer()[0]!=black && v.framebuffer()[16]!=black);
        m.reset(); // surviving CRTC state and RAM must remain coherent
        CHECK(v.framebuffer()[0]!=black && v.framebuffer()[16]!=black);
        m.writeByte(0x8000,0); m.writeByte(0x83fe,1);
        m.setScreenWindow(false); v.setColumns(40);
        machine.io().configureCrtc(false, 1);
        CHECK(v.framebuffer()[0]==black); // discrete 40-col display starts at zero
    }
    {
        PetMachine machine;
        auto& m = machine.bus();
        auto& v = machine.video();
        m.setRamSize(32768);
        machine.setVideoCharsets(chars, chars);
        machine.io().configureCrtc(false, 1);
        machine.reset();
        m.writeByte(0x8000, 1);

        // The discrete PET fetches column zero 24 clocks after VIDEO ON rises.
        for (int i = 0; i < 3840 + 24 - 4; ++i) machine.stepCycle();
        m.writeByte(0x0200, 0x8d); // STA $8000, A is zero after reset
        m.writeByte(0x0201, 0x00);
        m.writeByte(0x0202, 0x80);
        machine.cpu().set_pc(0x0200);
        CHECK(machine.runCycles(4) == 4);
        CHECK(m.peekByte(0x8000) == 0);
        for (int i = 3840 + 24; i < 16640; ++i) machine.stepCycle();

        // The store lands after the video fetch in that clock. Only the next
        // scanline and subsequent frames should see the new character.
        CHECK(v.framebuffer()[0] != black);
        CHECK(v.framebuffer()[2 * v.fbWidth()] == black);
        for (int i = 0; i < 16640; ++i) machine.stepCycle();
        CHECK(v.framebuffer()[0] == black);
    }
    {
        for (const int bytesPerClock : {1, 2}) {
            PetMachine machine;
            auto& m = machine.bus();
            auto& v = machine.video();
            const bool wide = bytesPerClock == 2;
            m.setRamSize(32768);
            machine.setVideoCharsets(chars, chars);
            m.setScreenWindow(wide);
            v.setColumns(wide ? 80 : 40);
            machine.io().configureCrtc(true, bytesPerClock);
            crtc(m, 0, 7); // Eight character clocks per line.
            crtc(m, 1, 2);
            crtc(m, 3, 0x20); // Two lines of VSYNC.
            crtc(m, 4, 3);
            crtc(m, 6, 1); // Two displayed scanlines.
            crtc(m, 7, 2); // VSYNC starts after four lines.
            crtc(m, 9, 1);
            machine.reset();
            for (int i = 0; i < bytesPerClock; ++i) m.writeByte(0x8000 + i, 1);
            for (int i = 0; i < bytesPerClock; ++i) machine.stepCycle();
            for (int i = 0; i < bytesPerClock; ++i) m.writeByte(0x8000 + i, 0);
            for (int i = bytesPerClock; i < 16; ++i) machine.stepCycle();
            CHECK((m.readByte(0xe880) & 0x20) != 0); // Vertical display ended.
            CHECK((m.readByte(0xe840) & 0x20) != 0); // VSYNC has yet to start.
            for (int i = 16; i < 32; ++i) machine.stepCycle();
            CHECK((m.readByte(0xe840) & 0x20) == 0);
            for (int i = 32; i < 48; ++i) machine.stepCycle();
            CHECK((m.readByte(0xe840) & 0x20) != 0);
            for (int i = 48; i < 64; ++i) machine.stepCycle();
            CHECK(v.framebuffer()[0] != black);
            CHECK(v.framebuffer()[wide ? 8 : 15] != black);
            CHECK(v.framebuffer()[16] == black);
            CHECK(v.framebuffer()[2 * v.fbWidth()] == black);

            // LDA's final bus read lands immediately before the VSYNC edge.
            // Starting the instruction one clock later sees the new level.
            m.writeByte(0x0200, 0xad);
            m.writeByte(0x0201, 0x40);
            m.writeByte(0x0202, 0xe8);
            for (int i = 0; i < 28; ++i) machine.stepCycle();
            machine.cpu().set_pc(0x0200);
            CHECK(machine.runCycles(4) == 4);
            CHECK((machine.cpu().A & 0x20) != 0);
            for (int i = 32; i < 64 + 29; ++i) machine.stepCycle();
            machine.cpu().set_pc(0x0200);
            CHECK(machine.runCycles(4) == 4);
            CHECK((machine.cpu().A & 0x20) == 0);
        }
    }
    {
        Pet2001Video v; v.setCharsets(chars,chars); v.reset(); v.write(0,1);
        v.setVideoBlank(true); v.update(100); v.setColumns(80);
        CHECK(v.framebuffer()[0]==black);
        v.setVideoBlank(false); CHECK(v.framebuffer()[0]!=black);
        const auto snapshot=v.save();
        v.setVideoBlank(true); v.update(50); v.load(snapshot); v.update(60);
        CHECK(v.framebuffer()[0]!=black);
        v.setVideoBlank(true); v.update(100); const auto dark=v.save();
        v.setVideoBlank(false); v.load(dark); v.update(100);
        CHECK(v.framebuffer()[0]==black);
        v.setVideoBlank(false); CHECK(v.framebuffer()[0]!=black);
    }
    {
        // IEEE DAV (PIA2 CB2) and NDAC (PIA2 CA2) are pulled low only in manual-low
        // output mode. A program that zeroes the control registers makes the pins
        // inputs, which float high: the kernal then sees an idle bus (VIA PB7 = DAV,
        // PB0 = NDAC) instead of waiting forever for DAV to rise.
        PetMachine machine;
        auto& m = machine.bus();
        m.writeByte(0xE823, 0x34); CHECK((m.readByte(0xE840) & 0x80) == 0);   // manual low
        m.writeByte(0xE823, 0x3C); CHECK((m.readByte(0xE840) & 0x80) != 0);   // manual high
        m.writeByte(0xE823, 0x34); m.writeByte(0xE823, 0x00);
        CHECK((m.readByte(0xE840) & 0x80) != 0);                              // input: floats high
        m.writeByte(0xE821, 0x34); CHECK((m.readByte(0xE840) & 0x01) == 0);
        m.writeByte(0xE821, 0x3C); CHECK((m.readByte(0xE840) & 0x01) != 0);
        m.writeByte(0xE821, 0x34); m.writeByte(0xE821, 0x00);
        CHECK((m.readByte(0xE840) & 0x01) != 0);
    }
    {
        // 8096/8296 expansion at $FFF0: 64 KB in four 16 KB blocks over $8000-$FFFF.
        static uint8_t rom[0x4000];
        memset(rom, 0xA5, sizeof rom);
        PetMachine machine;
        auto& m = machine.bus();
        m.setRamSize(32768);
        m.installROM(0xC000, rom, sizeof rom);

        // Absent: the register write is an ignored ROM write and nothing maps.
        m.writeByte(0xFFF0, 0x80); m.writeByte(0x9000, 0x11); m.writeByte(0xC000, 0x22);
        CHECK(!m.expansion());
        CHECK(m.readByte(0x9000) == 0xFF && m.readByte(0xC000) == 0xA5);

        // Present, register 0: RAM, ROM, screen and I/O as before.
        m.setExpansion(true);
        CHECK(m.expansion());
        m.writeByte(0xFFF0, 0x00);
        m.writeByte(0x1000, 0x5A); m.writeByte(0x8000, 0x33); m.writeByte(0x9000, 0x11); m.writeByte(0xC000, 0x22);
        CHECK(m.readByte(0x1000) == 0x5A && m.readByte(0x8000) == 0x33);
        CHECK(m.readByte(0x9000) == 0xFF && m.readByte(0xC000) == 0xA5);
        m.writeByte(0xE842, 0xA5);                      // VIA DDRB
        CHECK(m.readByte(0xE842) == 0xA5);

        // $80: $8000-$FFFF is expansion RAM; register 0 hides it again.
        m.writeByte(0xFFF0, 0x80);
        m.writeByte(0x9000, 0x11); m.writeByte(0xC000, 0x22);
        CHECK(m.readByte(0x9000) == 0x11 && m.readByte(0xC000) == 0x22);
        CHECK(m.peekByte(0x9000) == 0x11);
        m.writeByte(0xFFF0, 0x00);
        CHECK(m.readByte(0x9000) == 0xFF && m.readByte(0xC000) == 0xA5);
        m.writeByte(0xFFF0, 0x80);
        CHECK(m.readByte(0x9000) == 0x11 && m.readByte(0xC000) == 0x22);

        // Block select: bit 2 for $8000-$BFFF, bit 3 for $C000-$FFFF.
        m.writeByte(0xFFF0, 0x84);
        CHECK(m.readByte(0x9000) != 0x11);
        m.writeByte(0x9000, 0x33);
        CHECK(m.readByte(0x9000) == 0x33);
        m.writeByte(0xFFF0, 0x80);
        CHECK(m.readByte(0x9000) == 0x11);
        m.writeByte(0xFFF0, 0x88);
        CHECK(m.readByte(0xC000) != 0x22);
        m.writeByte(0xC000, 0x44);
        CHECK(m.readByte(0xC000) == 0x44);
        m.writeByte(0xFFF0, 0x80);
        CHECK(m.readByte(0xC000) == 0x22);

        // Write protect: bit 0 for $8000-$BFFF, bit 1 for $C000-$FFFF; reads still work.
        m.writeByte(0xFFF0, 0x81);
        m.writeByte(0x9000, 0x99);
        CHECK(m.readByte(0x9000) == 0x11);
        m.writeByte(0xFFF0, 0x82);
        m.writeByte(0xC000, 0x99);
        CHECK(m.readByte(0xC000) == 0x22);
        m.writeByte(0x9000, 0x12);                      // $8000-$BFFF is not protected under $82
        CHECK(m.readByte(0x9000) == 0x12);
        m.writeByte(0xFFF0, 0x80);                      // register loads even though $FFF0 was protected
        m.writeByte(0x9000, 0x11);

        // Screen peek-through (bit 5): $8000 stays the screen memory.
        m.writeByte(0xFFF0, 0xA0);
        m.writeByte(0x8000, 0x41);
        CHECK(m.readByte(0x8000) == 0x41);
        m.writeByte(0xFFF0, 0x80);
        m.writeByte(0x8000, 0x42);                      // goes to expansion RAM, not the screen
        CHECK(m.readByte(0x8000) == 0x42);
        m.writeByte(0xFFF0, 0x00);
        CHECK(m.readByte(0x8000) == 0x41);

        // I/O peek-through (bit 6): $E842 is the VIA unless that bit is clear.
        m.writeByte(0xFFF0, 0xC0);
        CHECK(m.readByte(0xE842) == 0xA5);
        m.writeByte(0xFFF0, 0x80);
        m.writeByte(0xE842, 0x5A);                      // plain RAM, the VIA is not touched
        CHECK(m.readByte(0xE842) == 0x5A);
        m.writeByte(0xFFF0, 0xC0);
        CHECK(m.readByte(0xE842) == 0xA5);

        // Reset clears the register; so does removing the expansion.
        m.writeByte(0xFFF0, 0x80);
        m.reset();
        CHECK(m.readByte(0x9000) == 0xFF && m.readByte(0xC000) == 0xA5);
        m.writeByte(0xFFF0, 0x80);
        m.setExpansion(false);
        m.setExpansion(true);
        CHECK(m.readByte(0xC000) == 0xA5);
    }
    {
        // 8296 4 KB screen: $8800-$8FFF is real memory the CRTC can show.
        PetMachine machine;
        auto& m = machine.bus();
        auto& v = machine.video();
        machine.setVideoCharsets(chars, chars);
        m.setScreenWindow(true); v.setColumns(80);
        machine.io().configureCrtc(true, 2);
        m.writeByte(0x8800, 0x22);
        CHECK(m.readByte(0x8800) == 0xFF);              // 2 KB boards: open bus
        m.setScreenWindow4K(); v.setScreen4K(true);
        v.setColumns(80);                               // a model load keeps the 4 KB screen
        m.writeByte(0x8000, 0x11); m.writeByte(0x8800, 0x22); m.writeByte(0x8FFF, 0x33);
        CHECK(m.readByte(0x8000) == 0x11 && m.readByte(0x8800) == 0x22 && m.readByte(0x8FFF) == 0x33);
        CHECK(m.peekByte(0x8800) == 0x22);
        crtc(m, 6, 25); crtc(m, 9, 7); crtc(m, 1, 40); crtc(m, 13, 0);
        crtc(m, 12, 0x14);                              // MA $1400 = byte offset $2800, wraps to $800
        m.writeByte(0x8800, 0);
        CHECK(v.framebuffer()[0] == black);
        m.writeByte(0x8800, 1);
        CHECK(v.framebuffer()[0] != black);
        crtc(m, 12, 0x10);                              // MA $1000 = byte offset $2000, wraps to 0
        m.writeByte(0x8000, 0);
        CHECK(v.framebuffer()[0] == black);
        m.writeByte(0x8000, 1);
        CHECK(v.framebuffer()[0] != black);
        v.setScreen4K(false); m.setScreenWindow(true);
        CHECK(m.readByte(0x8800) == 0xFF);
    }
    {
        // 4 KB character ROM (8296-D): CRTC R12 bit 5 picks the lower or upper pair. Glyph 1's
        // top row lights a different pixel in each of the four 1 KB sets, so the pixel names the set.
        static uint8_t gfxLow[1024], txtLow[1024], gfxUp[1024], txtUp[1024];
        gfxLow[8] = 0x80; txtLow[8] = 0x40; gfxUp[8] = 0x20; txtUp[8] = 0x10;   // pixels 0, 1, 2, 3
        PetMachine machine;
        auto& m = machine.bus();
        auto& v = machine.video();
        machine.setVideoCharsets(gfxLow, txtLow);
        v.setCharsetsUpper(gfxUp, txtUp);
        m.setScreenWindow(true); v.setColumns(80);
        machine.io().configureCrtc(true, 2);
        crtc(m, 6, 25); crtc(m, 9, 7); crtc(m, 1, 40); crtc(m, 13, 0);
        m.writeByte(0x8000, 1);
        auto lit = [&] { int mask = 0; for (int x = 0; x < 4; ++x) if (v.framebuffer()[x] != black) mask |= 1 << x; return mask; };
        CHECK(lit() == 1);                              // lower bank, graphics
        crtc(m, 12, 0x20);
        CHECK(lit() == 4);                              // R12 bit 5: upper bank, still graphics
        v.setCharset(true);
        CHECK(lit() == 8);                              // upper bank, text
        crtc(m, 12, 0x00);
        CHECK(lit() == 2);                              // back to the lower bank, text kept
        v.setCharset(false);
        CHECK(lit() == 1);
        crtc(m, 12, 0x30);                              // bit 4 (video invert) does not matter
        CHECK(lit() == 4);
        machine.setVideoCharsets(gfxLow, txtLow);       // a one-pair install drops the second pair
        m.writeByte(0x8000, 1);
        CHECK(lit() == 1);
        crtc(m, 12, 0x20);
        CHECK(lit() == 1);                              // no upper pair: the bank is ignored
        v.setCharBank(true);
        CHECK(lit() == 1);
    }
    {
        // BASIC 1 disk traps: only active on a ROM with the BASIC 1 bytes at the
        // trapped addresses, and only while enabled.
        PetMachine machine;
        CHECK(!machine.basic1DiskTrapsActive());     // no ROM installed
        static uint8_t f000[4096] = {};
        const uint8_t load[] = {0x20,0x62,0xF4,0x20,0xB6,0xF0,0x20,0x22,0xF4,0x20,0x39,0xF3,0x20,0x87,0xF1};
        const uint8_t save[] = {0x20,0x1C,0xF7,0x20,0x62,0xF4,0x20,0xBA,0xF0};
        memcpy(f000+0x375,load,sizeof load);
        f000[0x3CC]=0xAD; f000[0x3CD]=0x0B; f000[0x3CE]=0x02;
        f000[0x579]=0xA0; f000[0x57A]=0x24;
        memcpy(f000+0x6C0,save,sizeof save);
        f000[0x735]=0x60;
        CHECK(machine.loadRom(f000,sizeof f000,0xF000));
        CHECK(machine.basic1DiskTrapsActive());
        machine.setBasic1DiskTraps(false);
        CHECK(!machine.basic1DiskTrapsActive());
        machine.setBasic1DiskTraps(true);
        CHECK(machine.basic1DiskTrapsActive());
        f000[0x735]=0xEA;                           // one byte off: another ROM
        CHECK(machine.loadRom(f000,sizeof f000,0xF000));
        CHECK(!machine.basic1DiskTrapsActive());
    }
    // Debug CRT counts live allocations, including the CPU and its internal storage.
#if defined(_MSC_VER) && defined(_DEBUG)
    _CrtMemState before{},after{},delta{};
    _CrtMemCheckpoint(&before);
    { PetMachine machine; }
    _CrtMemCheckpoint(&after);
    CHECK(!_CrtMemDifference(&delta,&before,&after));
#endif
    printf("PET machine: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
