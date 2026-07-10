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

// ---------------------------------------------------------------------------
// Tier-2 datasheet-accuracy tests (2026-07-10 review fixes)
// ---------------------------------------------------------------------------

// Reading/writing ORA clears IFR1 (CA1) and IFR0 (CA2) in handshake input
// modes. Reading/writing ORB clears IFR4 (CB1) and IFR3 (CB2) likewise.
static void test_porta_access_clears_ca_flags(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x00);            // CA1 neg edge; CA2 mode 000 (input neg, handshake)
    v.writeReg(R_IFR, 0x7F);            // start clean
    v.setCA1(true);  v.tick();
    v.setCA1(false); v.tick();          // CA1 falling edge -> IFR1
    v.setCA2(true);  v.tick();
    v.setCA2(false); v.tick();          // CA2 falling edge -> IFR0
    CHECK((v.getIFR() & 0x02) != 0);
    CHECK((v.getIFR() & 0x01) != 0);
    (void)v.readReg(R_ORA);             // ORA read clears both
    CHECK_EQ(v.getIFR() & 0x03, 0x00);
    // Same for ORA WRITE
    v.setCA1(true); v.tick(); v.setCA1(false); v.tick();
    v.setCA2(true); v.tick(); v.setCA2(false); v.tick();
    CHECK_EQ(v.getIFR() & 0x03, 0x03);
    v.writeReg(R_ORA, 0x00);
    CHECK_EQ(v.getIFR() & 0x03, 0x00);
}

// In CA2 INDEPENDENT input mode (PCR CA2 mode 001), port access must NOT
// clear IFR0 (only an explicit IFR write may).
static void test_porta_independent_ca2_flag_survives(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x02);            // CA2 mode 001: independent, negative edge
    v.writeReg(R_IFR, 0x7F);
    v.setCA2(true);  v.tick();
    v.setCA2(false); v.tick();          // neg edge -> IFR0
    CHECK((v.getIFR() & 0x01) != 0);
    (void)v.readReg(R_ORA);
    CHECK((v.getIFR() & 0x01) != 0);    // survives ORA access
    v.writeReg(R_IFR, 0x01);
    CHECK_EQ(v.getIFR() & 0x01, 0x00);  // explicit IFR write clears
}

static void test_portb_access_clears_cb_flags(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x00);            // CB1 neg edge; CB2 mode 000 (input neg, handshake)
    v.writeReg(R_IFR, 0x7F);
    v.setCB1(true);  v.tick();
    v.setCB1(false); v.tick();          // CB1 falling edge -> IFR4
    v.setCB2(true);  v.tick();
    v.setCB2(false); v.tick();          // CB2 falling edge -> IFR3
    CHECK_EQ(v.getIFR() & 0x18, 0x18);
    (void)v.readReg(R_ORB);             // ORB read clears both
    CHECK_EQ(v.getIFR() & 0x18, 0x00);
    v.setCB1(true); v.tick(); v.setCB1(false); v.tick();
    v.setCB2(true); v.tick(); v.setCB2(false); v.tick();
    CHECK_EQ(v.getIFR() & 0x18, 0x18);
    v.writeReg(R_ORB, 0x00);            // ORB write clears both
    CHECK_EQ(v.getIFR() & 0x18, 0x00);
    // Independent CB2 (mode 001 = PCR $20): flag survives ORB access
    v.writeReg(R_PCR, 0x20);
    v.writeReg(R_IFR, 0x7F);
    v.setCB2(true); v.tick(); v.setCB2(false); v.tick();
    CHECK((v.getIFR() & 0x08) != 0);
    (void)v.readReg(R_ORB);
    CHECK((v.getIFR() & 0x08) != 0);
}

// Register $F (ORA no-handshake) must not clear flags or trigger handshake.
static void test_anh_no_side_effects(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x00);
    v.writeReg(R_IFR, 0x7F);
    v.setCA1(true);  v.tick();
    v.setCA1(false); v.tick();          // IFR1 set
    CHECK((v.getIFR() & 0x02) != 0);
    (void)v.readReg(0x0F);              // ANH read: flags untouched
    CHECK((v.getIFR() & 0x02) != 0);
    v.writeReg(0x0F, 0x55);             // ANH write: flags untouched, ORA updated
    CHECK((v.getIFR() & 0x02) != 0);
    CHECK_EQ(v.getORA(), 0x55);
}

// Writing the T1 high-order LATCH (reg 7) clears IFR6 (the IRQ-handler
// "re-program without reload" idiom acknowledges T1 this way).
static void test_t1lh_write_clears_ifr6(){
    VIA6522 v; v.reset();
    const int N = 30;
    v.writeReg(R_ACR, 0x40);            // free-run
    v.writeReg(R_T1CL, N); v.writeReg(R_T1CH, 0);
    int hit=-1; for(int i=1;i<=N+5;i++){ v.tick(); if(v.getIFR()&0x40){hit=i;break;} }
    CHECK(hit>0);
    v.writeReg(R_T1LH, 0x00);           // latch write acks T1
    CHECK_EQ(v.getIFR() & 0x40, 0x00);
}

// One-shot T1 keeps counting after timeout (only the IRQ is one-shot).
static void test_t1_oneshot_keeps_counting(){
    VIA6522 v; v.reset();
    const int N = 50;
    v.writeReg(R_ACR, 0x00);            // one-shot
    v.writeReg(R_T1CL, N); v.writeReg(R_T1CH, 0);
    int hit=-1; for(int i=1;i<=N+5;i++){ v.tick(); if(v.getIFR()&0x40){hit=i;break;} }
    CHECK(hit>0);
    v.writeReg(R_IFR, 0x40);
    uint16_t c1 = (uint16_t)(v.readReg(R_T1CL) | (v.readReg(R_T1CH) << 8));
    tickN(v, 10);
    uint16_t c2 = (uint16_t)(v.readReg(R_T1CL) | (v.readReg(R_T1CH) << 8));
    CHECK_EQ((uint16_t)(c1 - c2), 10);  // still decrementing
    bool refired=false;
    for(int i=0;i<0x20000;i++){ v.tick(); if(v.getIFR()&0x40){refired=true;break;} }
    CHECK(!refired);                    // IRQ stays one-shot until re-armed
}

// ACR7 one-shot: writing T1C-H drives PB7 LOW; timeout drives it HIGH.
static void test_pb7_oneshot_pulse(){
    VIA6522 v; v.reset();
    const int N = 20;
    v.writeReg(R_DDRB, 0x80);           // PB7 output
    v.writeReg(R_ACR, 0x80);            // PB7 under T1, one-shot
    v.writeReg(R_T1CL, N); v.writeReg(R_T1CH, 0);
    v.tick();
    CHECK_EQ(v.readReg(R_ORB) & 0x80, 0x00);   // low during the pulse
    tickN(v, N+3);                              // through underflow
    CHECK_EQ(v.readReg(R_ORB) & 0x80, 0x80);   // high after timeout
}

// ACR5: T2 counts PB6 negative edges, not PHI2.
static void test_t2_pulse_count_mode(){
    VIA6522 v; v.reset();
    v.writeReg(R_ACR, 0x20);            // T2 pulse-counting
    v.writeReg(R_T2CL, 4); v.writeReg(R_T2CH, 0);
    v.writeReg(R_IFR, 0x20);
    tickN(v, 200);                      // PHI2 must NOT count it down
    CHECK_EQ(v.getIFR() & 0x20, 0x00);
    uint8_t pb = 0xFF;
    for (int i = 0; i < 5; ++i) {       // 5 PB6 negative edges
        v.setPortBInput((uint8_t)(pb & ~0x40)); v.tick();
        v.setPortBInput(pb);             v.tick();
    }
    CHECK((v.getIFR() & 0x20) != 0);    // counted down through zero
}

// CB2 handshake output (mode 100): ORB write drives CB2 low and it STAYS
// low until the next active CB1 transition.
static void test_cb2_handshake_holds_until_cb1(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x80);            // CB2 mode 100; CB1 neg edge
    v.setCB1(true); v.tick();           // prime CB1 high (no trigger)
    v.writeReg(R_ORB, 0x00);            // data taken -> CB2 low
    CHECK_EQ(v.getCB2Output(), false);
    tickN(v, 20);
    CHECK_EQ(v.getCB2Output(), false);  // holds low (no auto-release)
    v.setCB1(false); v.tick();          // CB1 active edge -> release
    CHECK_EQ(v.getCB2Output(), true);
}

// CB2 pulse output (mode 101): one-cycle low pulse after ORB write.
static void test_cb2_pulse_one_cycle(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0xA0);            // CB2 mode 101
    v.tick();
    CHECK_EQ(v.getCB2Output(), true);   // idles high
    v.writeReg(R_ORB, 0x00);
    CHECK_EQ(v.getCB2Output(), false);  // pulse starts
    v.tick();
    CHECK_EQ(v.getCB2Output(), false);  // low for the pulse cycle
    v.tick();
    CHECK_EQ(v.getCB2Output(), true);   // released without any CB1 edge
}

// Real 6522 CA2 decode: modes 110/111 are MANUAL output low/high
// (the code previously treated 2/3 as manual and 6/7 as independent input).
static void test_ca2_manual_modes_real_encoding(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x0C); v.tick();  // CA2 mode 110 -> manual LOW
    CHECK_EQ(v.getCA2Output(), false);
    v.writeReg(R_PCR, 0x0E); v.tick();  // CA2 mode 111 -> manual HIGH
    CHECK_EQ(v.getCA2Output(), true);
}

// CA2 handshake output (mode 100): ORA READ or WRITE drives CA2 low; it
// stays low until the next active CA1 transition.
static void test_ca2_handshake_on_ora_access(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x08);            // CA2 mode 100; CA1 neg edge
    v.setCA1(true); v.tick();           // prime CA1 high
    (void)v.readReg(R_ORA);             // read triggers handshake
    CHECK_EQ(v.getCA2Output(), false);
    tickN(v, 20);
    CHECK_EQ(v.getCA2Output(), false);  // holds
    v.setCA1(false); v.tick();          // CA1 active edge releases
    CHECK_EQ(v.getCA2Output(), true);
    v.setCA1(true); v.tick();
    v.writeReg(R_ORA, 0x12);            // write also triggers
    CHECK_EQ(v.getCA2Output(), false);
    v.setCA1(false); v.tick();
    CHECK_EQ(v.getCA2Output(), true);
}

// ACR0: IRA latches on the CA1 active edge; reads return the latched value
// until the next active edge.
static void test_input_latching_porta(){
    VIA6522 v; v.reset();
    v.writeReg(R_PCR, 0x00);            // CA1 neg edge
    v.writeReg(R_DDRA, 0x00);           // all inputs
    v.writeReg(R_ACR, 0x01);            // enable PA latching
    v.setPortAInput(0xAA);
    v.setCA1(true);  v.tick();
    v.setCA1(false); v.tick();          // active edge latches 0xAA
    v.setPortAInput(0x55);              // pins change afterwards
    CHECK_EQ(v.readReg(R_ORA), 0xAA);   // latched value, not live pins
    v.writeReg(R_ACR, 0x00);            // latching off -> live pins again
    CHECK_EQ(v.readReg(R_ORA), 0x55);
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
    // Tier-2 datasheet-accuracy fixes
    test_porta_access_clears_ca_flags();
    test_porta_independent_ca2_flag_survives();
    test_portb_access_clears_cb_flags();
    test_anh_no_side_effects();
    test_t1lh_write_clears_ifr6();
    test_t1_oneshot_keeps_counting();
    test_pb7_oneshot_pulse();
    test_t2_pulse_count_mode();
    test_cb2_handshake_holds_until_cb1();
    test_cb2_pulse_one_cycle();
    test_ca2_manual_modes_real_encoding();
    test_ca2_handshake_on_ora_access();
    test_input_latching_porta();
    std::printf("\n%s  (%d checks, %d failures)\n", g_fail? "TESTS FAILED":"ALL TESTS PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
