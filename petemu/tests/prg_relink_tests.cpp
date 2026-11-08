// Tests for basic_relink(): rebuilding CBM/PET BASIC line links after a direct
// PRG injection, the way the KERNAL does after a relocating LOAD.
//
// Reproduces the TELENGARD bug: a BASIC program whose stored forward links were
// built for a $0801 load but which loads at $0401 (links $0400 too high). After
// relink the links must point at the real next line so RUN can follow the chain.
#include <cstdint>
#include <cstdio>
#include <vector>

#include "basic_prg.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond) do { ++g_checks; if(!(cond)){ ++g_fail; \
    std::printf("FAIL %s:%d  CHECK(%s)\n", __FILE__, __LINE__, #cond);} } while(0)
#define CHECK_EQ(a,b) do { ++g_checks; long _va=(long)(a), _vb=(long)(b); \
    if(_va!=_vb){ ++g_fail; std::printf("FAIL %s:%d  CHECK_EQ(%s,%s)  got %ld != %ld\n", \
    __FILE__, __LINE__, #a, #b, _va, _vb);} } while(0)

static inline uint16_t link_at(const uint8_t* m, uint16_t a) {
    return (uint16_t)(m[a] | (m[a + 1] << 8));
}

// Build one BASIC line at `pos`: [link lo][link hi][lineno lo][lineno hi][tokens..][00]
// `badLink` is written as the (deliberately wrong) forward link. Returns the
// address of the next line.
static uint16_t put_line(uint8_t* m, uint16_t pos, uint16_t badLink,
                         uint16_t lineNo, const std::vector<uint8_t>& tokens) {
    m[pos]     = (uint8_t)(badLink & 0xFF);
    m[pos + 1] = (uint8_t)(badLink >> 8);
    m[pos + 2] = (uint8_t)(lineNo & 0xFF);
    m[pos + 3] = (uint8_t)(lineNo >> 8);
    uint16_t p = pos + 4;
    for (uint8_t t : tokens) m[p++] = t;
    m[p++] = 0x00;                         // end-of-line
    return p;
}

// A 3-line program at $0401 whose links are all $0400 too high (TELENGARD case).
static void test_relink_fixes_offset_links() {
    std::vector<uint8_t> mem(0x10000, 0);
    uint8_t* m = mem.data();
    const uint16_t start = 0x0401;

    // Lay out lines to learn the real next-line addresses, with wrong links.
    uint16_t a10 = start;
    uint16_t a20 = put_line(m, a10, (uint16_t)(0 /*patched below*/), 10, {0x8f, 0x41});        // REM A
    uint16_t a30 = put_line(m, a20, 0, 20, {0x8f, 0x42, 0x42});                                 // REM BB
    uint16_t aEnd = put_line(m, a30, 0, 30, {0x8f, 0x43});                                      // REM C
    m[aEnd] = 0x00; m[aEnd + 1] = 0x00;                                                         // null link
    const uint16_t expectVartab = (uint16_t)(aEnd + 2);

    // Now stamp the WRONG links ($0400 too high), as TELENGARD has.
    m[a10] = (uint8_t)((a20 + 0x0400) & 0xFF); m[a10 + 1] = (uint8_t)((a20 + 0x0400) >> 8);
    m[a20] = (uint8_t)((a30 + 0x0400) & 0xFF); m[a20 + 1] = (uint8_t)((a30 + 0x0400) >> 8);
    m[a30] = (uint8_t)((aEnd + 0x0400) & 0xFF); m[a30 + 1] = (uint8_t)((aEnd + 0x0400) >> 8);

    // Sanity: links are wrong before relink.
    CHECK(link_at(m, a10) != a20);

    const uint16_t vartab = basic_relink(m, start, (uint32_t)expectVartab);

    CHECK_EQ(link_at(m, a10), a20);        // each link now points at the real next line
    CHECK_EQ(link_at(m, a20), a30);
    CHECK_EQ(link_at(m, a30), aEnd);       // last real line links to the null-link slot
    CHECK_EQ(m[aEnd], 0); CHECK_EQ(m[aEnd + 1], 0);   // null link preserved
    CHECK_EQ(vartab, expectVartab);
}

// A correct program must be left unchanged (idempotent relink).
static void test_relink_idempotent_on_good_program() {
    std::vector<uint8_t> mem(0x10000, 0);
    uint8_t* m = mem.data();
    const uint16_t start = 0x0401;

    uint16_t a10 = start;
    uint16_t a20 = put_line(m, a10, 0, 10, {0x99, 0x22, 0x48, 0x49, 0x22});  // PRINT "HI"
    uint16_t aEnd = put_line(m, a20, 0, 20, {0x80});                          // END
    m[aEnd] = 0; m[aEnd + 1] = 0;
    // Correct links.
    m[a10] = (uint8_t)(a20 & 0xFF); m[a10 + 1] = (uint8_t)(a20 >> 8);
    m[a20] = (uint8_t)(aEnd & 0xFF); m[a20 + 1] = (uint8_t)(aEnd >> 8);

    const uint16_t vartab = basic_relink(m, start, (uint32_t)(aEnd + 2));
    CHECK_EQ(link_at(m, a10), a20);
    CHECK_EQ(link_at(m, a20), aEnd);
    CHECK_EQ(vartab, (uint16_t)(aEnd + 2));
}

// A single-line SYS stub followed by an ML blob (back2pet shape): relink must
// fix the stub's link, stop at the BASIC null link, and NOT touch the ML bytes.
static void test_relink_stops_at_null_link_keeps_ml() {
    std::vector<uint8_t> mem(0x10000, 0);
    uint8_t* m = mem.data();
    const uint16_t start = 0x0401;

    uint16_t aEnd = put_line(m, start, 0x080B /*wrong*/, 800, {0x9e, 0x31, 0x30, 0x33, 0x37}); // SYS 1037
    m[aEnd] = 0; m[aEnd + 1] = 0;                       // null link
    const uint16_t mlStart = (uint16_t)(aEnd + 2);
    m[mlStart] = 0xAA; m[mlStart + 1] = 0xBB;          // ML blob beyond the program

    // limit spans well past the ML (as the real loader passes end-of-file).
    const uint16_t vartab = basic_relink(m, start, (uint32_t)(mlStart + 64));
    CHECK_EQ(link_at(m, start), aEnd);                 // stub link corrected
    CHECK_EQ(vartab, mlStart);                          // VARTAB at the null-link's end
    CHECK_EQ(m[mlStart], 0xAA);                         // ML untouched
    CHECK_EQ(m[mlStart + 1], 0xBB);
}

int main() {
    test_relink_fixes_offset_links();
    test_relink_idempotent_on_good_program();
    test_relink_stops_at_null_link_keeps_ml();
    std::printf("\nPRG relink tests: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
