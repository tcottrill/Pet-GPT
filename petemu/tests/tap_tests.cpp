#include "pet_machine.h"
#include "pet_roms.h"
#include <cstdio>
#include <fstream>
#include <cstring>
#include <filesystem>
namespace Log {
bool open(const std::string&) { return true; }
void close() {}
void write(Level, const char*, const char*, int, const char*, ...) {}
void setLevel(Level) {}
void setConsoleOutputEnabled(bool) {}
}
static int checks=0, failures=0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL %d: %s\n",__LINE__,#x); } } while(0)
static std::vector<uint8_t> image(std::vector<uint8_t> payload, int version=1, int platform=3) {
    std::vector<uint8_t> b(20,0);
    memcpy(b.data(),"C64-TAPE-RAW",12); b[12]=version; b[13]=platform;
    for (int n=0;n<4;++n) b[16+n]=uint8_t(payload.size()>>(n*8));
    b.insert(b.end(),payload.begin(),payload.end()); return b;
}
static void cycles(Pet2001IO& io, int n) { while(n--) io.cycle(); }
static void screen(PetMachine& m) {
    for(int y=0;y<25;++y) { for(int x=0;x<40;++x) {
        int c=m.bus().readByte(0x8000+y*40+x)&127;
        putchar(c<32 ? c+64 : c);
    } putchar('\n'); }
}
static bool shows(PetMachine& m, const char* text, int cells=1000) {
    for(int a=0;a<cells-int(strlen(text));++a) {
        bool match=true;
        for(size_t n=0;n<strlen(text);++n)
            if((m.bus().readByte(uint16_t(0x8000+a+n))&127)!=(uint8_t(text[n])&63)) match=false;
        if(match) return true;
    }
    return false;
}
static void command(PetMachine& m, const char* text, bool original=false) {
    const uint16_t count=original ? 0x20d : 0x9e;
    const uint16_t buffer=original ? 0x20f : 0x26f;
    while (*text) {
        for (int n=0;n<100 && m.bus().readByte(count);++n) m.runCycles(20000);
        m.bus().writeByte(buffer,uint8_t(*text++)); m.bus().writeByte(count,1);
        m.runCycles(30000);
    }
}
int main(int argc, char** argv) {
    std::string error;
    PetTape tape;
    CHECK(tape.attach(image({1,2,0,3,0,0}),error));
    CHECK(tape.mounted() && !tape.playing()); tape.play();
    for(int i=0;i<20;++i) CHECK(!tape.tick(false));
    for(int i=0;i<7;++i) CHECK(!tape.tick(true));
    CHECK(tape.tick(true)); CHECK(tape.position()==1);
    tape.stop(); for(int i=0;i<20;++i) CHECK(!tape.tick(true));
    tape.play(); for(int i=0;i<15;++i) CHECK(!tape.tick(true));
    CHECK(tape.tick(true)); CHECK(!tape.tick(true)); CHECK(!tape.tick(true));
    CHECK(tape.tick(true)); CHECK(tape.atEnd() && !tape.playing());
    tape.play(); CHECK(!tape.playing()); tape.rewind(); CHECK(tape.position()==0 && !tape.atEnd());
    CHECK(tape.attach(image({0},0),error)); tape.play();
    for(int i=0;i<2047;++i) CHECK(!tape.tick(true)); CHECK(tape.tick(true));
    auto valid=image({1});
    for(size_t n=0;n<20;++n) { auto b=valid; b.resize(n); CHECK(!tape.attach(b,error)); }
    CHECK(!tape.attach(image({1},2),error));
    CHECK(!tape.attach(image({1},1,2),error));
    CHECK(!tape.attach(image({0,0,0,0}),error));
    CHECK(!tape.attach(image({0,1,0}),error));
    CHECK(!tape.attach(image({}),error));
    auto bad=valid; bad[16]=2; CHECK(!tape.attach(bad,error));
    bad=valid; bad[0]='X'; CHECK(!tape.attach(bad,error));
    CHECK(tape.mounted() && tape.atEnd()); // failed attach leaves prior media intact
    CHECK(tape.attach(image({0,0xA0,0x86,1},1,0),error)); // 100000 C64 cycles
    tape.play(); int ticks=0; do { ++ticks; } while(!tape.tick(true));
    CHECK(ticks==101497); tape.eject(); CHECK(!tape.mounted());
    {
        PetMachine m; auto& io=m.io();
        CHECK(io.tape().attach(image({2,2,2}),error));
        io.write(0x11,0x05); // PA register, falling-edge CA1 IRQ enabled
        CHECK((io.read(0x10)&0x30)==0x30);
        io.tape().play(); CHECK((io.read(0x10)&0x30)==0x20);
        cycles(io,40); CHECK(io.tape().position()==0); // CB2 input: motor off
        io.write(0x13,0x3c); cycles(io,40); CHECK(io.tape().position()==0); // manual high
        io.write(0x13,0x34); // manual low: motor on
        io.read(0x10); cycles(io,15); CHECK(!(io.read(0x11)&0x80));
        cycles(io,1); CHECK(io.read(0x11)&0x80); CHECK(m.bus().irqLevel());
        io.read(0x10); CHECK(!(io.read(0x11)&0x80));
        cycles(io,5); io.write(0x13,0x3c); cycles(io,50);
        CHECK(io.tape().position()==1);
        io.write(0x13,0x34); cycles(io,10); CHECK(io.tape().position()==1);
        cycles(io,1); CHECK(io.tape().position()==2);
        io.reset(); CHECK(!io.tape().playing() && io.tape().position()==2);
        CHECK(io.read(0x10)==0); // DDRA register after reset
        io.write(0x11,4); CHECK((io.read(0x10)&0x30)==0x30);
    }
    if(argc>1) {
        // Optional real-ROM round trip: capture SAVE's PB3 transitions, then
        // replay the resulting TAP through the production LOAD path.
        PetMachine m;
        CHECK(load_pet2_romset(m,argv[1],true));
        m.bus().setRamSize(32768); m.reset(); m.runCycles(3000000);
        command(m,"10 PRINT\"TAP OK\"\r");
        m.runCycles(100000);
        const unsigned end=m.bus().readByte(0x2a)|(m.bus().readByte(0x2b)<<8);
        std::vector<uint8_t> expected;
        for(unsigned a=0x401;a<end;++a) expected.push_back(m.bus().readByte(a));
        CHECK(expected.size()>8 && expected.size()<100);
        CHECK(m.io().tape().attach(image({0,255,255,255,0,255,255,255,0,255,255,255}),error));
        command(m,"SAVE\"TAPTEST\"\r");
        m.io().tape().play();
        std::vector<uint8_t> payload;
        uint64_t elapsed=0,last=0; bool previous=false, started=false;
        for(int n=0;n<40000000;) {
            int c=m.runCycles(1); n+=c; elapsed+=c;
            const bool level=(m.io().read(0x40)&8)!=0;
            // Datasette write circuitry inverts PB3: rising write-line edges
            // correspond to the falling read edges represented by TAP v1.
            if(!previous && level) {
                if(started) {
                    const uint32_t pulse=uint32_t(elapsed-last);
                    payload.push_back(0); payload.push_back(uint8_t(pulse));
                    payload.push_back(uint8_t(pulse>>8)); payload.push_back(uint8_t(pulse>>16));
                }
                last=elapsed; started=true;
            }
            previous=level;
            if(started && elapsed-last>2000000) break;
        }
        CHECK(payload.size()>1000);
        printf("ROM SAVE captured %zu pulses, expected %zu bytes\n",payload.size()/4,expected.size());
        auto tap=image(payload);
        if(argc>2) { std::ofstream out(argv[2],std::ios::binary); out.write((const char*)tap.data(),tap.size()); }
        m.reset(); m.runCycles(3000000);
        for(size_t i=0;i<expected.size();++i) m.bus().writeByte(uint16_t(0x401+i),0xA5);
        CHECK(m.io().tape().attach(tap,error));
        command(m,"LOAD\r"); m.io().tape().play();
        m.runCycles(40000000);
        bool equal=true;
        for(size_t i=0;i<expected.size();++i) if(m.bus().readByte(uint16_t(0x401+i))!=expected[i]) equal=false;
        CHECK(equal);
        CHECK(shows(m,"FOUND TAPTEST"));
        command(m,"RUN\r"); m.runCycles(100000);
        CHECK(shows(m,"TAP OK"));
        if(!equal) screen(m);
        const auto root=std::filesystem::path(argv[1]).parent_path();
        // argv[1] may have a trailing slash, so resolve the common ROM root.
        const auto romRoot=root.filename()=="pet2001n" ? root.parent_path() : root;
        for(int model : {1,4,12,8}) {
            PetMachine other;
            const char* folder=model==1 ? "pet2001" : model==4 ? "pet4000-9" : model==12 ? "pet4000-12" : "cbm8032";
            const std::string dir=(romRoot/folder).string()+"/";
            bool loaded=model==1 ? load_pet1_romset(other,dir) : model==8 ? load_pet8032_romset(other,dir) : load_pet4_romset(other,dir,model==12);
            CHECK(loaded); if(!loaded) continue;
            other.bus().setRamSize(model==1 ? 8192 : 32768);
            other.bus().setScreenWindow(model==8);
            other.video().setColumns(model==8 ? 80 : 40);
            other.io().configureCrtc(model==8 || model==12,model==8 ? 2 : 1);
            other.reset(); other.runCycles(3000000);
            CHECK(other.io().tape().attach(tap,error));
            for(size_t i=0;i<expected.size();++i) other.bus().writeByte(uint16_t(0x401+i),0xA5);
            command(other,"LOAD\r",model==1); other.io().tape().play();
            other.runCycles(40000000);
            bool restored=true;
            for(size_t i=0;i<expected.size();++i) if(other.bus().readByte(uint16_t(0x401+i))!=expected[i]) restored=false;
            CHECK(restored);
            command(other,"RUN\r",model==1); other.runCycles(100000);
            CHECK(shows(other,"TAP OK",model==8 ? 2000 : 1000));
            printf("ROM profile %s: %s\n",folder,restored ? "program restored" : "FAILED");
            if(!restored) screen(other);
        }
    }
    printf("TAP: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
