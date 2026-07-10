// Standalone VIA 6522 timing tests. Compiles via6522.cpp alone.
#include <cstdio>
#include <cstdint>
#include <string>
#include "via6522.h"
#include "sys_log.h"

// --- no-op Log stubs so via6522.cpp links without log.cpp / Windows threads ---
namespace Log {
    bool open(const std::string&) { return true; }
    void close() {}
    void write(Level, const char*, const char*, int, const char*, ...) {}
    void setLevel(Level) {}
    void setConsoleOutputEnabled(bool) {}
}

// --- tiny assert framework ---
static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);} } while(0)
#define CHECK_EQ(a,b) do { ++g_checks; long _va=(long)(a), _vb=(long)(b); \
    if(_va!=_vb){ ++g_fail; std::printf("FAIL %s:%d  CHECK_EQ(%s,%s)  got %ld != %ld\n", \
    __FILE__, __LINE__, #a, #b, _va, _vb);} } while(0)

// --- VIA register offsets ---
enum { R_ORB=0,R_ORA=1,R_DDRB=2,R_DDRA=3,R_T1CL=4,R_T1CH=5,R_T1LL=6,R_T1LH=7,
       R_T2CL=8,R_T2CH=9,R_SR=10,R_ACR=11,R_PCR=12,R_IFR=13,R_IER=14 };

static void tickN(VIA6522& v, int n){ for(int i=0;i<n;i++) v.tick(); }

// --- tests ---
static void test_reset_defaults(){
    VIA6522 v; v.reset();
    CHECK_EQ(v.getIFR(), 0x00);
    CHECK_EQ(v.getACR(), 0x00);
    CHECK_EQ(v.getPCR(), 0x00);
    CHECK_EQ(v.getCB2Output(), true);   // CB2 idles high after reset
}

// Measure steady-state period between consecutive timer underflow flags.
static int nextFlagGap(VIA6522& v, uint8_t mask, int maxc){
    for(int i=1;i<=maxc;i++){ v.tick(); if(v.getIFR()&mask){ v.writeReg(R_IFR, mask); return i; } }
    return -1;
}
static void test_t1_freerun_period_is_N_plus_2(){
    VIA6522 v; v.reset();
    const int N = 100;
    v.writeReg(R_ACR, 0x40);                 // T1 free-run
    v.writeReg(R_T1CL, N & 0xFF);
    v.writeReg(R_T1CH, (N>>8) & 0xFF);       // arm + transfer latch->counter
    (void)nextFlagGap(v, 0x40, N+10);        // transient (first interval)
    int p2 = nextFlagGap(v, 0x40, N+10);     // steady state
    int p3 = nextFlagGap(v, 0x40, N+10);
    CHECK_EQ(p2, N+2);
    CHECK_EQ(p3, N+2);
}

static void test_t2_plain_is_one_shot(){
    VIA6522 v; v.reset();
    const int N = 80;
    v.writeReg(R_ACR, 0x00);                 // plain timed T2 (not SR)
    v.writeReg(R_T2CL, N & 0xFF);
    v.writeReg(R_T2CH, (N>>8) & 0xFF);       // arm T2
    int first=-1;
    for(int i=1;i<=N+5;i++){ v.tick(); if(v.getIFR()&0x20){ first=i; break; } }
    CHECK(first>0);                          // it fires
    v.writeReg(R_IFR, 0x20);                 // clear IFR5
    bool refired=false;
    for(int i=0;i<0x20000;i++){ v.tick(); if(v.getIFR()&0x20){ refired=true; break; } }
    CHECK(!refired);                         // one-shot: must NOT fire again
}

// Faulty Robots Method B: ACR=$10, write T2CL only, SR pattern -> CB2 square wave.
static void test_methodB_cb2_bit_period(){
    VIA6522 v; v.reset();
    const int N = 50;
    v.writeReg(R_ACR, 0x10);          // SR mode 4: shift out free-run at T2 rate
    v.writeReg(R_T2CL, N & 0xFF);     // low byte only (T2CH never written -> stays 0)
    v.writeReg(R_SR, 0x55);           // 01010101 -> CB2 toggles every shifted bit
    v.cb2_reset_edge_log();
    tickN(v, 2*(N+2)*8);
    const CB2Edge* e = v.cb2_get_edges();
    CHECK(v.cb2_get_edge_count() >= 4);
    CHECK_EQ((long)(e[2].cycle - e[1].cycle), 2*(N+2));   // skip first transient edge
    CHECK_EQ((long)(e[3].cycle - e[2].cycle), 2*(N+2));
}

// Prove SR-mode reload ignores the T2 HIGH latch byte.
static void test_methodB_ignores_t2_high_byte(){
    VIA6522 v; v.reset();
    const int N = 40;
    v.writeReg(R_T2CH, 0x02);         // forces t2_latch high = 0x02 (and a long initial count)
    v.writeReg(R_ACR, 0x10);          // SR mode 4
    v.writeReg(R_T2CL, N & 0xFF);     // low byte; demo never updates high
    v.writeReg(R_SR, 0x55);
    tickN(v, 0x0300);                 // let the initial 0x02xx countdown drain
    v.cb2_reset_edge_log();
    tickN(v, 2*(N+2)*8);
    const CB2Edge* e = v.cb2_get_edges();
    CHECK(v.cb2_get_edge_count() >= 4);
    CHECK_EQ((long)(e[2].cycle - e[1].cycle), 2*(N+2));   // NOT 2*(0x0240+2)
}

// Method A clock: T1 free-run IRQ, cleared by reading T1CL (like peskytone's handler).
static int nextT1Irq(VIA6522& v, int maxc){
    for(int i=1;i<=maxc;i++){ v.tick(); if(v.irqLine()){ v.readReg(R_T1CL); return i; } }
    return -1;
}
static void test_methodA_t1_irq_period(){
    VIA6522 v; v.reset();
    const int N = 300;
    v.writeReg(R_IER, 0xC0);                 // enable T1 interrupt (bit6)
    v.writeReg(R_ACR, 0x40);                 // T1 free-run
    v.writeReg(R_T1CL, N & 0xFF);
    v.writeReg(R_T1CH, (N>>8) & 0xFF);
    (void)nextT1Irq(v, N+10);                // transient
    int p2 = nextT1Irq(v, N+10);
    int p3 = nextT1Irq(v, N+10);
    CHECK_EQ(p2, N+2);
    CHECK_EQ(p3, N+2);
}

// Method A output: PCR mode 6/7 drives CB2 low/high and logs an edge each change.
static void test_methodA_pcr_drives_cb2(){
    VIA6522 v; v.reset();
    v.cb2_reset_edge_log();
    v.writeReg(R_PCR, 0xC0); CHECK_EQ(v.getCB2Output(), false); // mode 6 fixed low
    v.writeReg(R_PCR, 0xE0); CHECK_EQ(v.getCB2Output(), true);  // mode 7 fixed high
    v.writeReg(R_PCR, 0xC0); CHECK_EQ(v.getCB2Output(), false);
    CHECK(v.cb2_get_edge_count() >= 3);
}

// note 24: period word $011C=284 -> T1 = 284*2 = 568 -> IRQ period 570.
static void test_demo_methodA_note24(){
    VIA6522 v; v.reset();
    const int R = 568;
    v.writeReg(R_IER, 0xC0);
    v.writeReg(R_ACR, 0x40);
    v.writeReg(R_T1CL, R & 0xFF);
    v.writeReg(R_T1CH, (R>>8) & 0xFF);
    (void)nextT1Irq(v, R+10);
    CHECK_EQ(nextT1Irq(v, R+10), R+2);   // 570
}
// note 26: period $00FD=253 -> T2CL=253 -> CB2 bit period 2*(253+2)=510.
static void test_demo_methodB_note26(){
    VIA6522 v; v.reset();
    const int N = 253;
    v.writeReg(R_ACR, 0x10);
    v.writeReg(R_T2CL, N & 0xFF);
    v.writeReg(R_SR, 0x55);
    v.cb2_reset_edge_log();
    tickN(v, 2*(N+2)*6);
    const CB2Edge* e = v.cb2_get_edges();
    CHECK(v.cb2_get_edge_count() >= 4);
    CHECK_EQ((long)(e[2].cycle - e[1].cycle), 510);
}

// Regression: a song switches Method B (T2/SR) -> Method A (T1 IRQ, SR off) ->
// Method B. T2 becomes a plain one-shot during Method A and its counter wanders.
// Re-entering Method B (peskytone @sr: ACR, T2CL, SR) must resume shifting at the
// correct rate (bit period 2*(N+2)), not stall until T2 slowly wraps around.
static void test_methodB_resumes_after_methodA(){
    VIA6522 v; v.reset();
    const int N = 50;
    v.writeReg(R_ACR, 0x10);                 // Method B
    v.writeReg(R_T2CL, N & 0xFF);
    v.writeReg(R_SR, 0x55);
    tickN(v, 2*(N+2)*4);                      // shifting underway
    v.writeReg(R_ACR, 0xC0);                  // Method A: SR off, T2 plain one-shot
    tickN(v, 4000);                           // T2 counter wanders high
    v.writeReg(R_ACR, 0x10);                  // new Method B note, like @sr
    v.writeReg(R_T2CL, N & 0xFF);
    v.writeReg(R_SR, 0x55);
    v.cb2_reset_edge_log();
    tickN(v, 2*(N+2)*10);                     // several bit periods
    const CB2Edge* e = v.cb2_get_edges();
    CHECK(v.cb2_get_edge_count() >= 5);                  // shifting resumed
    CHECK_EQ((long)(e[3].cycle - e[2].cycle), 2*(N+2));  // at the correct rate
}

// pet-invaders parks the SR free-running (ACR=$10) with an ALL-ZERO pattern as
// its "sound off" state. That must stay silent: no byte-boundary phantom pulse.
static void test_methodB_zero_pattern_is_silent(){
    VIA6522 v; v.reset();
    v.writeReg(R_ACR, 0x10);          // SR mode 4 free-run
    v.writeReg(R_T2CL, 0xFF);         // slow shift
    v.writeReg(R_SR, 0x00);           // all zeros -> CB2 must stay low
    v.cb2_reset_edge_log();
    tickN(v, 2*(255+2)*8*4);          // several byte periods
    CHECK_EQ((long)v.cb2_get_edge_count(), 0); // no toggling -> silent
}
// Sanity: a NON-zero free-run pattern still toggles (Faulty Robots path).
static void test_methodB_nonzero_pattern_sounds(){
    VIA6522 v; v.reset();
    v.writeReg(R_ACR, 0x10);
    v.writeReg(R_T2CL, 0x20);
    v.writeReg(R_SR, 0x55);
    v.cb2_reset_edge_log();
    tickN(v, 2*(0x20+2)*8*4);
    CHECK(v.cb2_get_edge_count() >= 8); // still produces sound
}

int main(){
    test_reset_defaults();
    test_methodB_resumes_after_methodA();
    test_methodB_zero_pattern_is_silent();
    test_methodB_nonzero_pattern_sounds();
    test_t1_freerun_period_is_N_plus_2();
    test_t2_plain_is_one_shot();
    test_methodB_cb2_bit_period();
    test_methodB_ignores_t2_high_byte();
    test_methodA_t1_irq_period();
    test_methodA_pcr_drives_cb2();
    test_demo_methodA_note24();
    test_demo_methodB_note26();
    std::printf("\n%s  (%d checks, %d failures)\n", g_fail? "TESTS FAILED":"ALL TESTS PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
