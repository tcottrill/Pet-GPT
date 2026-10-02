#include "pet_machine.h"
#include <cstdio>
#include <cstring>
#include <crtdbg.h>

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
int main() {
    memset(chars+8,255,8); // glyph 1 lit; spaces and glyph 0 dark
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
        m.writeByte(0x8802,0); // mirrored write must redraw the shifted cell
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
    // Debug CRT counts live allocations, including the CPU and its internal storage.
    _CrtMemState before{},after{},delta{};
    _CrtMemCheckpoint(&before);
    { PetMachine machine; }
    _CrtMemCheckpoint(&after);
    CHECK(!_CrtMemDifference(&delta,&before,&after));
    printf("PET machine: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
