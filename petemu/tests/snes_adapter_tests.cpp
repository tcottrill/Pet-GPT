// Standalone test for SnesAdapter: replays the PETSCII Robots read protocol
// (latch high/low, then 12x: read DATA, clock high/low) and checks the bits.
#include <cstdio>
#include <cstdint>
#include "snes_adapter.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do{ ++g_checks; if(!(c)){ ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);} }while(0)

// Replay the read protocol; collect the 12 raw DATA(PA6) levels the game reads.
static void readPad(SnesAdapter& a, bool dataHigh[12])
{
    a.onPortAWrite(SnesAdapter::LATCH); // latch high
    a.onPortAWrite(0x00);               // latch low
    for (int i = 0; i < 12; ++i) {
        dataHigh[i] = a.dataBit();          // game reads PA6 here
        a.onPortAWrite(SnesAdapter::CLOCK); // clock high
        a.onPortAWrite(0x00);               // clock low (shift)
    }
}

static void test_invert_true_pressed_high()  // PETSCII Robots convention
{
    SnesAdapter a; a.reset(); a.setInvert(true);
    uint16_t mask = (1 << SnesAdapter::BTN_B) | (1 << SnesAdapter::BTN_START) | (1 << SnesAdapter::BTN_R);
    a.setButtons(mask);
    bool bits[12]; readPad(a, bits);
    for (int i = 0; i < 12; ++i) CHECK(bits[i] == (bool)((mask >> i) & 1)); // high == pressed
}

static void test_invert_false_pressed_low()  // real SNES / pet-invaders convention
{
    SnesAdapter a; a.reset(); a.setInvert(false);
    uint16_t mask = (1 << SnesAdapter::BTN_A) | (1 << SnesAdapter::BTN_LEFT);
    a.setButtons(mask);
    bool bits[12]; readPad(a, bits);
    for (int i = 0; i < 12; ++i) CHECK(bits[i] == (bool)!((mask >> i) & 1)); // pressed -> low
}

static void test_idle_is_released()          // no stuck fire when nothing pressed
{
    { SnesAdapter a; a.reset(); a.setInvert(false); a.setButtons(0);
      CHECK(a.dataBit() == true);            // at-rest line high = released (active-low)
      bool b[12]; readPad(a, b); for (int i = 0; i < 12; ++i) CHECK(b[i] == true); }
    { SnesAdapter a; a.reset(); a.setInvert(true); a.setButtons(0);
      bool b[12]; readPad(a, b); for (int i = 0; i < 12; ++i) CHECK(b[i] == false); }
}

static void test_disabled_ignores_buttons()
{
    SnesAdapter a; a.reset(); a.setInvert(false);
    a.setEnabled(false);
    a.setButtons(0xFFFF);                    // disabled => no buttons reach the wire
    bool b[12]; readPad(a, b);
    for (int i = 0; i < 12; ++i) CHECK(b[i] == true); // all released
}

int main()
{
    test_invert_true_pressed_high();
    test_invert_false_pressed_low();
    test_idle_is_released();
    test_disabled_ignores_buttons();
    std::printf("\n%s  (%d checks, %d failures)\n", g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
