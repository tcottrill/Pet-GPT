// Standalone MOS 6545 CRTC tests. Compiles mos6545.cpp alone.
#include <cstdio>
#include <cstdint>
#include "mos6545.h"

// --- tiny assert framework (matches the other suites) ---
static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);} } while(0)
#define CHECK_EQ(a,b) do { ++g_checks; long _va=(long)(a), _vb=(long)(b); \
    if(_va!=_vb){ ++g_fail; std::printf("FAIL %s:%d  CHECK_EQ(%s,%s)  got %ld != %ld\n", \
    __FILE__, __LINE__, #a, #b, _va, _vb);} } while(0)

static void wr(Mos6545& c, int r, uint8_t v) { c.writeAddr((uint8_t)r); c.writeData(v); }

static void test_reset_state() {
    Mos6545 c;
    for (int i = 0; i < 18; ++i) CHECK_EQ(c.reg(i), 0);
    CHECK_EQ(c.cols(), 0);                 // "not programmed yet" signal for the glue
    CHECK_EQ(c.readStatus(), 0x00);
    CHECK_EQ(c.geometryEpoch(), 0);
}

// The 8032 editor programs the file at init; verify the decoded geometry.
// (R1=40: the 8032 character clock counts 2-byte units - glue doubles it.)
static void test_8032_style_init_geometry() {
    Mos6545 c;
    wr(c, 0, 49);  wr(c, 1, 40);  wr(c, 2, 41);  wr(c, 3, 0x0F);
    wr(c, 4, 32);  wr(c, 5, 3);   wr(c, 6, 25);  wr(c, 7, 29);
    wr(c, 8, 0);   wr(c, 9, 7);   wr(c, 10, 0);  wr(c, 11, 0);
    wr(c, 12, 0x10); wr(c, 13, 0x00);
    CHECK_EQ(c.cols(), 40);                // raw R1 (x2 applied by the glue)
    CHECK_EQ(c.rows(), 25);
    CHECK_EQ(c.scanlinesPerChar(), 8);     // R9 + 1
    CHECK_EQ(c.screenStart(), 0x1000);     // (R12<<8 | R13) & 0x3FFF
}

static void test_register_select_masks_to_5_bits() {
    Mos6545 c;
    c.writeAddr(0x2E);                     // 0x2E & 0x1F = 14
    CHECK_EQ(c.selectedReg(), 14);
    c.writeData(0x12);
    CHECK_EQ(c.reg(14), 0x12);
}

static void test_write_only_registers_read_zero() {
    Mos6545 c;
    wr(c, 1, 40); wr(c, 6, 25); wr(c, 12, 0x10);
    c.writeAddr(1);  CHECK_EQ(c.readData(), 0);
    c.writeAddr(6);  CHECK_EQ(c.readData(), 0);
    c.writeAddr(12); CHECK_EQ(c.readData(), 0);
    // ...but the stored values still drive geometry
    CHECK_EQ(c.cols(), 40);
}

static void test_cursor_registers_read_back_masked() {
    Mos6545 c;
    wr(c, 14, 0xFF); wr(c, 15, 0xAB);
    c.writeAddr(14); CHECK_EQ(c.readData(), 0x3F);   // R14 is 6 bits
    c.writeAddr(15); CHECK_EQ(c.readData(), 0xAB);
    CHECK_EQ(c.cursorAddr(), 0x3FAB);
}

static void test_light_pen_is_read_only() {
    Mos6545 c;
    wr(c, 16, 0x55); wr(c, 17, 0x66);
    c.writeAddr(16); CHECK_EQ(c.readData(), 0);
    c.writeAddr(17); CHECK_EQ(c.readData(), 0);
}

static void test_unimplemented_select_ignored() {
    Mos6545 c;
    c.writeAddr(18); c.writeData(0xFF);    // selects nonexistent R18
    c.writeAddr(31); c.writeData(0xFF);
    for (int i = 0; i < 18; ++i) CHECK_EQ(c.reg(i), 0);
}

static void test_r12_screen_start_mask() {
    Mos6545 c;
    wr(c, 12, 0xFF); wr(c, 13, 0x34);
    CHECK_EQ(c.screenStart(), 0x3F34);     // R12 masked to 6 bits
}

static void test_status_vertical_retrace() {
    Mos6545 c;
    CHECK_EQ(c.readStatus() & 0x80, 0x00);
    c.setVerticalRetrace(true);
    CHECK_EQ(c.readStatus() & 0x80, 0x80);
    c.setVerticalRetrace(false);
    CHECK_EQ(c.readStatus() & 0x80, 0x00);
}

static void test_geometry_epoch_semantics() {
    Mos6545 c;
    const uint32_t e0 = c.geometryEpoch();
    wr(c, 1, 40);                          // geometry reg changes -> bump
    CHECK(c.geometryEpoch() > e0);
    const uint32_t e1 = c.geometryEpoch();
    wr(c, 1, 40);                          // same value -> no bump
    CHECK_EQ(c.geometryEpoch(), e1);
    wr(c, 14, 0x12); wr(c, 15, 0x34);      // cursor moves -> no bump
    CHECK_EQ(c.geometryEpoch(), e1);
    wr(c, 13, 0x50);                       // screen start moves -> bump
    CHECK(c.geometryEpoch() > e1);
}

// Frame timing derived from the register file, in character clocks (= CPU
// cycles on the 8032): line = R0+1; frame = (R4+1)*(R9+1)+R5 lines; active
// video = R6*(R9+1) lines, remainder is vertical blank.
static void test_frame_timing_formula() {
    Mos6545 c;
    // Synthetic ~60 Hz geometry: 50-cycle line, 41*8+5 = 333 lines.
    wr(c, 0, 49); wr(c, 4, 40); wr(c, 5, 5); wr(c, 6, 25); wr(c, 9, 7);
    CHECK_EQ(c.frameCycles(), 50 * 333);           // 16650 -> 60.06 Hz
    CHECK_EQ(c.vblankCycles(), 50 * (333 - 200));  // 6650 (133 blank lines)

    // ~50 Hz variant: more total lines, same active window.
    wr(c, 4, 49);                                  // 50*8+5 = 405 lines
    CHECK_EQ(c.frameCycles(), 50 * 405);           // 20250 -> 49.4 Hz
    CHECK_EQ(c.vblankCycles(), 50 * (405 - 200));
}

static void test_frame_timing_unprogrammed_is_nonsense() {
    Mos6545 c;
    // Reset state must yield an obviously-insane value so the glue's sanity
    // window rejects it and keeps the fixed 40-col timing.
    CHECK_EQ(c.frameCycles(), 1);
    CHECK_EQ(c.vblankCycles(), 1);   // total 1 line, active 0 -> 1 cycle
    // Display taller than total (half-programmed): vblank clamps to 0.
    wr(c, 4, 0); wr(c, 9, 0); wr(c, 6, 25);
    CHECK_EQ(c.vblankCycles(), 0);
}

int main() {
    test_reset_state();
    test_8032_style_init_geometry();
    test_register_select_masks_to_5_bits();
    test_write_only_registers_read_zero();
    test_cursor_registers_read_back_masked();
    test_light_pen_is_read_only();
    test_unimplemented_select_ignored();
    test_r12_screen_start_mask();
    test_status_vertical_retrace();
    test_geometry_epoch_semantics();
    test_frame_timing_formula();
    test_frame_timing_unprogrammed_is_nonsense();
    std::printf("\n%s  (%d checks, %d failures)\n",
                g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
