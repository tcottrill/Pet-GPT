// Standalone Pet2001Video tests. Compiles pet2001video.cpp alone (no Log deps).
// Covers the 80-column full-redraw fix, blank-timer behavior, and save/load
// vidram coverage.
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include "pet2001video.h"

// --- tiny assert framework (matches the other suites) ---
static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);} } while(0)
#define CHECK_EQ(a,b) do { ++g_checks; long _va=(long)(a), _vb=(long)(b); \
    if(_va!=_vb){ ++g_fail; std::printf("FAIL %s:%d  CHECK_EQ(%s,%s)  got %ld != %ld\n", \
    __FILE__, __LINE__, #a, #b, _va, _vb);} } while(0)

// Synthetic charsets: every glyph solid (all rows 0xFF) so any drawn cell has
// lit pixels. Two distinct arrays so setCharset() sees a real switch.
static uint8_t cs1[128 * 8];
static uint8_t cs2[128 * 8];

static const uint32_t BLACK = 0xFF000000u;

// Sample the top-left pixel of a character cell.
static uint32_t cellPixel(const Pet2001Video& v, int col, int row, int cols)
{
    const int cellW = (cols == 80) ? 8 : 16;
    const int x = col * cellW;
    const int y = row * 16;
    return v.framebuffer()[y * v.fbWidth() + x];
}

static Pet2001Video makeVideo(int cols)
{
    Pet2001Video v;
    v.setCharsets(cs1, cs2);
    v.reset();
    v.setColumns(cols);
    return v;
}

// The bug: redrawScreen() looped to VIDRAM_SIZE (1000) instead of visible_
// (2000 at 80 cols), so any full redraw blacked out the bottom half of an
// 8032 screen. setCharset() is one of the paths that triggers a full redraw.
static void test_80col_full_redraw_covers_bottom_half()
{
    Pet2001Video v = makeVideo(80);
    // Bottom-right cell (row 24, col 79 -> addr 1999). Solid glyph.
    v.write(1999, 0x01);
    CHECK(cellPixel(v, 79, 24, 80) != BLACK);   // incremental draw works

    v.setCharset(true);                          // full redraw (charset switch)
    CHECK(cellPixel(v, 79, 24, 80) != BLACK);   // bottom half must survive
    CHECK(cellPixel(v, 0, 0, 80) != BLACK);     // top half too (space glyph is solid here)
}

static void test_40col_full_redraw_unchanged()
{
    Pet2001Video v = makeVideo(40);
    v.write(999, 0x01);                          // bottom-right cell at 40 cols
    v.setCharset(true);
    CHECK(cellPixel(v, 39, 24, 40) != BLACK);
}

// Blank is requested by PIA1 CA2 going low; the screen actually goes dark
// only after the 100 ms delay elapses via update() calls.
static void test_blank_timer_fires_after_delay()
{
    Pet2001Video v = makeVideo(40);
    v.write(0, 0x01);
    CHECK(cellPixel(v, 0, 0, 40) != BLACK);

    v.setVideoBlank(true);
    v.update(60);                                // 60 ms: not yet
    CHECK(cellPixel(v, 0, 0, 40) != BLACK);
    v.update(60);                                // 120 ms total: blanked
    CHECK_EQ(cellPixel(v, 0, 0, 40), BLACK);

    v.setVideoBlank(false);                      // unblank redraws immediately
    CHECK(cellPixel(v, 0, 0, 40) != BLACK);
}

static void test_unblank_before_delay_cancels()
{
    Pet2001Video v = makeVideo(40);
    v.write(0, 0x01);
    v.setVideoBlank(true);
    v.update(60);
    v.setVideoBlank(false);                      // cancel pending blank
    v.update(1000);                              // timer must not fire late
    CHECK(cellPixel(v, 0, 0, 40) != BLACK);
}

// Writes while blanked must appear once unblanked (redraw pulls from vidram).
static void test_writes_during_blank_appear_on_unblank()
{
    Pet2001Video v = makeVideo(80);
    v.setVideoBlank(true);
    v.update(200);                               // now blanked
    v.write(1999, 0x01);                         // bottom half, while dark
    CHECK_EQ(cellPixel(v, 79, 24, 80), BLACK);
    v.setVideoBlank(false);
    CHECK(cellPixel(v, 79, 24, 80) != BLACK);    // needs the visible_ redraw fix
}

// save() must cover the whole 80-col vidram (2000 entries + 2 header fields).
static void test_save_covers_80col_vidram()
{
    Pet2001Video v = makeVideo(80);
    const std::string s = v.save();
    size_t commas = 0;
    for (char ch : s) if (ch == ',') ++commas;
    CHECK_EQ((long)commas, 2000 + 2);

    Pet2001Video v40 = makeVideo(40);
    const std::string s40 = v40.save();
    commas = 0;
    for (char ch : s40) if (ch == ',') ++commas;
    CHECK_EQ((long)commas, 1000 + 2);
}

static void test_save_load_roundtrip_80col()
{
    Pet2001Video a = makeVideo(80);
    a.write(1999, 0x41);
    a.write(0, 0x42);
    const std::string snap = a.save();

    Pet2001Video b = makeVideo(80);
    b.load(snap);
    CHECK(cellPixel(b, 79, 24, 80) != BLACK);    // addr 1999 restored + drawn
    CHECK(cellPixel(b, 0, 0, 80) != BLACK);
}

int main()
{
    std::memset(cs1, 0xFF, sizeof(cs1));
    std::memset(cs2, 0xFF, sizeof(cs2));

    test_80col_full_redraw_covers_bottom_half();
    test_40col_full_redraw_unchanged();
    test_blank_timer_fires_after_delay();
    test_unblank_before_delay_cancels();
    test_writes_during_blank_appear_on_unblank();
    test_save_covers_80col_vidram();
    test_save_load_roundtrip_80col();

    std::printf("\n%s  (%d checks, %d failures)\n",
                g_fail ? "TESTS FAILED" : "ALL TESTS PASSED", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
