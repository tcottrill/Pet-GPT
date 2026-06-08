// Copyright (c) 2012,2014 Thomas Skibo.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
// OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
// HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
// OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGE.


#include "pet2001video.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>

Pet2001Video::Pet2001Video()
: vidram(VIDRAM_SIZE, 0),
  fb(FB_W * FB_H, RGBA_BLACK),
  charset1(nullptr),
  charset2(nullptr),
  activeCharset(nullptr),
  blank(false),
  blankRequested(false),
  blankCountdownMs(-1)
{
}

void Pet2001Video::setCharsets(const uint8_t* charset1_128x8, const uint8_t* charset2_128x8)
{
    charset1 = charset1_128x8;
    charset2 = charset2_128x8;
    // Default to charset1 until caller switches.
    activeCharset = charset1;
}

void Pet2001Video::reset()
{
    std::fill(vidram.begin(), vidram.end(), 0x20); // space
    std::fill(fb.begin(), fb.end(), RGBA_BLACK);
    blank = false;
    blankRequested = false;
    blankCountdownMs = -1;
    // If charsets are set, show empty screen; otherwise still fine.
    redrawScreen();
}

void Pet2001Video::write(int addr, uint8_t value)
{
    if (addr < 0) return;

    if (addr >= static_cast<int>(vidram.size())) {
        // Allow growth beyond 1000 to mirror original JS "this.vidram = new Array(VIDRAM_SIZE);"
        // but the JS used a fixed size; we'll just ignore writes beyond range unless you prefer resize.
        return;
    }

    vidram[addr] = value;

    // Incremental draw only for visible screen area and when not blank.
    if (addr < VIDRAM_SIZE && !blank && activeCharset != nullptr) {
        drawCharCell(addr, value);
    }
}

void Pet2001Video::setVideoBlank(bool flag)
{
    // Mimic JS behavior:
    // - When enabling blank: start a 100ms timer; when it expires, blank the screen.
    // - When disabling blank: cancel any pending timeout and redraw immediately.

    if (flag) {
        // request to blank
        if (!blank) {
            if (blankCountdownMs < 0) {
                blankCountdownMs = BLANK_DELAY_MS;
            }
            blankRequested = true;
        }
    } else {
        // request to unblank
        if (blankCountdownMs >= 0) {
            // cancel pending blanking
            blankCountdownMs = -1;
        }
        if (blank) {
            // Leaving blank: redraw everything
            redrawScreen();
            blank = false;
        }
        blankRequested = false;
    }
}

void Pet2001Video::setCharset(bool useSecond)
{
    const uint8_t* newSet = useSecond ? charset2 : charset1;
    if (newSet == activeCharset) return;

    activeCharset = newSet;
    if (!blank && activeCharset != nullptr) {
        redrawScreen();
    }
}

void Pet2001Video::update(int elapsed_ms)
{
    // Service pending blanking timer
    if (blankCountdownMs >= 0) {
        blankCountdownMs -= std::max(0, elapsed_ms);
        if (blankCountdownMs <= 0) {
            // Timer fired -> actually blank the screen
            blankScreen();
            blank = true;
            blankCountdownMs = -1;
        }
    }
    redrawScreen();
}

std::string Pet2001Video::save() const
{
    // JS string format:
    //   blankFlag ',' charsetIndex ',' then vidram bytes as hex with commas
    // Here we serialize:
    //   blank: '1'/'0'
    //   charset: '1' (charset1) or '2' (charset2)
    //   then VIDRAM_SIZE hex values with commas

    char buf[32];
    std::string s;
    s.reserve(2 + 2 + (VIDRAM_SIZE * 3)); // rough

    s += (blank ? '1' : '0');
    s += ',';
    s += (activeCharset == charset1 ? '1' : '2');
    s += ',';

    for (int i = 0; i < VIDRAM_SIZE; ++i) {
        std::snprintf(buf, sizeof(buf), "%02x", static_cast<unsigned>(vidram[i]));
        s += buf;
        s += ',';
    }
    return s;
}

void Pet2001Video::load(const std::string& s)
{
    // Expect the format we produced in save()
    // Split by commas (simple parser)
    std::vector<std::string> parts;
    parts.reserve(VIDRAM_SIZE + 2);

    size_t start = 0;
    for (;;) {
        size_t pos = s.find(',', start);
        if (pos == std::string::npos) {
            // last fragment (shouldn't happen in our save format, but handle anyway)
            parts.emplace_back(s.substr(start));
            break;
        }
        parts.emplace_back(s.substr(start, pos - start));
        start = pos + 1;
        if (parts.size() > (size_t)(VIDRAM_SIZE + 2)) break;
    }

    if (parts.size() < 2) return;

    // blank flag
    blank = (!parts[0].empty() && parts[0][0] == '1');

    // charset index
    bool useSecond = (!parts[1].empty() && parts[1][0] == '2');
    setCharset(useSecond);

    // vidram
    for (int i = 0; i < VIDRAM_SIZE && (i + 2) < (int)parts.size(); ++i) {
        const std::string& hx = parts[i + 2];
        if (hx.empty()) continue;
        uint8_t v = static_cast<uint8_t>(std::strtoul(hx.c_str(), nullptr, 16));
        vidram[i] = v;
    }

    if (blank) {
        blankScreen();
    } else {
        redrawScreen();
    }
}

void Pet2001Video::redrawScreen()
{
    if (activeCharset == nullptr) {
        // No charset -> just clear to black
        std::fill(fb.begin(), fb.end(), RGBA_BLACK);
        return;
    }

    // Clear entire screen to black, then paint each visible char cell
    std::fill(fb.begin(), fb.end(), RGBA_BLACK);

    for (int addr = 0; addr < VIDRAM_SIZE; ++addr) {
        drawCharCell(addr, vidram[addr]);
    }
}

void Pet2001Video::blankScreen()
{
    // Solid black
    std::fill(fb.begin(), fb.end(), RGBA_BLACK);
}

void Pet2001Video::drawCharCell(int addr, uint8_t ch)
{
    if (activeCharset == nullptr) return;

    // Compute row/col from linear address
    const int col = addr % COLS;
    const int row = addr / COLS;

    // Black-out entire character cell first
    fillRect(col * CELL_W, row * CELL_H, CELL_W, CELL_H, RGBA_BLACK);

    // Foreground color (white-ish)
    const uint32_t fg = RGBA_WHITEISH;

    const int glyph = (ch & 0x7F); // lower 7 bits select glyph
    const bool inv = (ch & 0x80) != 0;

    // Each glyph has 8 rows, MSB is leftmost pixel
    const uint8_t* base = activeCharset + (glyph * CHAR_H);

    // Paint doubled pixels (2x2) for each set bit
    const int x0 = col * CELL_W;
    const int y0 = row * CELL_H;

    for (int y = 0; y < CHAR_H; ++y) {
        uint8_t bits = base[y];
        if (inv) bits ^= 0xFF;

        for (int x = 0; x < CHAR_W; ++x) {
            if (bits & 0x80) {
                // Draw a 2x2 block
                const int px = x0 + x * SCALE;
                const int py = y0 + y * SCALE;
                // Fill 2x2 (fast-path writes)
                putPixel(px,     py,     fg);
                putPixel(px + 1, py,     fg);
                putPixel(px,     py + 1, fg);
                putPixel(px + 1, py + 1, fg);
            }
            bits <<= 1;
        }
    }
}

inline void Pet2001Video::putPixel(int x, int y, uint32_t rgba)
{
    if ((unsigned)x < (unsigned)FB_W && (unsigned)y < (unsigned)FB_H) {
        fb[y * FB_W + x] = rgba;
    }
}

inline void Pet2001Video::fillRect(int x, int y, int w, int h, uint32_t rgba)
{
    // Clip
    int x1 = std::max(0, x);
    int y1 = std::max(0, y);
    int x2 = std::min(FB_W, x + w);
    int y2 = std::min(FB_H, y + h);
    if (x1 >= x2 || y1 >= y2) return;

    const int pitch = FB_W;
    for (int yy = y1; yy < y2; ++yy) {
        uint32_t* row = &fb[yy * pitch];
        for (int xx = x1; xx < x2; ++xx) {
            row[xx] = rgba;
        }
    }
}
