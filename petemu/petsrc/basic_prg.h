#pragma once
#include <cstdint>

// -----------------------------------------------------------------------------
// basic_relink
//
// Rebuild the forward link pointers of a CBM/PET BASIC program already resident
// in a 64K memory image `mem`, starting at `start` (TXTTAB, e.g. $0401). This is
// what the KERNAL does after a *relocating* LOAD: it walks the program by line
// length and rewrites each line's 2-byte link to point at the next line.
//
// A BASIC line is:
//     [link lo][link hi][lineno lo][lineno hi][tokens...][$00]
// and the program ends with a null link ($00 $00). The stored links are NOT
// trusted while walking (a program saved for a different start address, e.g.
// $0801, carries links that are wrong for a $0401 load); only line lengths are.
//
// `limit` bounds the scan (exclusive, clamped to 64K). Returns VARTAB: the
// address just past the program's null link (start of variables / end of
// program). Stops early and returns the current position if the image is
// malformed (a line with no $00 terminator before `limit`).
// -----------------------------------------------------------------------------
inline uint16_t basic_relink(uint8_t* mem, uint16_t start, uint32_t limit)
{
    if (limit > 0x10000) limit = 0x10000;

    uint32_t ptr = start;
    while (ptr + 2 <= limit) {
        // A null link in the link slot marks the end of the program.
        if (mem[ptr] == 0 && mem[ptr + 1] == 0)
            return (uint16_t)(ptr + 2);

        // Skip link(2) + line number(2), then scan to the end-of-line $00.
        // (The line number's high byte can be $00, so we must skip it; tokens
        // and quoted text never contain $00 inside a line.)
        uint32_t i = ptr + 4;
        while (i < limit && mem[i] != 0) ++i;
        if (i >= limit) break;                       // no terminator -> malformed

        const uint32_t next = i + 1;                 // next line begins after $00
        mem[ptr]     = (uint8_t)(next & 0xFF);       // rewrite this line's link
        mem[ptr + 1] = (uint8_t)(next >> 8);
        ptr = next;
    }
    return (uint16_t)ptr;
}
