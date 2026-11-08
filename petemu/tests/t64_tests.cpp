#include "../petsrc/t64.h"
#include <cstdio>
#include <cstdlib>

static int checks = 0;
static void check(bool ok) { ++checks; if (!ok) { std::printf("FAILED check %d\n", checks); std::exit(1); } }
static std::vector<uint8_t> archive() {
    std::vector<uint8_t> b(134, 0);
    std::memcpy(b.data(), "C64S tape image file", 20);
    b[33]=1; b[34]=2; b[36]=2;
    for (int i=0; i<2; ++i) {
        int p=64+32*i;
        b[p]=1; b[p+1]=0x82; b[p+2]=1; b[p+3]=4;
        b[p+4]=4; b[p+5]=4; b[p+8]=128+3*i;
        std::memcpy(b.data()+p+16, i ? "SECOND" : "FIRST", i ? 6 : 5);
    }
    b[128]=0x11; b[129]=0x22; b[130]=0x33;
    b[131]=0x44; b[132]=0x55; b[133]=0x66;
    return b;
}
int main() {
    std::vector<t64::Program> programs;
    std::string error;
    auto b=archive();
    check(t64::read(b, programs, error));
    check(programs.size()==2 && programs[0].name=="FIRST" && programs[1].name=="SECOND");
    check(programs[0].prg==std::vector<uint8_t>({1,4,0x11,0x22,0x33}));
    check(programs[1].prg==std::vector<uint8_t>({1,4,0x44,0x55,0x66}));
    b[64]=0; b[36]=1;
    check(t64::read(b, programs, error) && programs.size()==1 && programs[0].name=="SECOND");
    b=archive(); b[68]=0xC6; b[69]=0xC3;
    check(t64::read(b, programs, error) && programs[0].prg.size()==5);
    b=archive(); b[65]=0x81;
    check(t64::read(b, programs, error) && programs.size()==1);
    b[97]=0x81;
    check(!t64::read(b, programs, error) && programs.empty() && !error.empty());
    b=archive(); b[72]=64;
    check(!t64::read(b, programs, error));
    b=archive(); b[104]=128;
    check(!t64::read(b, programs, error));
    b=archive(); b[68]=5;
    check(!t64::read(b, programs, error));
    b=archive(); b[68]=1;
    check(!t64::read(b, programs, error));
    b=archive(); b[107]=0xFF;
    check(!t64::read(b, programs, error) && programs.empty());
    b=archive(); b[34]=3;
    check(!t64::read(b, programs, error));
    b=archive(); b[36]=3;
    check(!t64::read(b, programs, error));
    b=archive(); b[0]='X';
    check(!t64::read(b, programs, error));
    b=archive(); b.resize(133);
    check(!t64::read(b, programs, error) && programs.empty());
    for (size_t n=0; n<128; ++n) {
        b=archive(); b.resize(n);
        check(!t64::read(b, programs, error));
    }
    std::printf("T64: %d checks passed\n", checks);
}
