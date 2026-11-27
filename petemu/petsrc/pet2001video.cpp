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
: vidram(1024, 0),
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
    // A one-pair install leaves no stale second bank from an earlier ROM set.
    upperCharset1 = upperCharset2 = nullptr;
    charBankUpper_ = false;
    textSet_ = false;
    // Default to charset1 until caller switches.
    activeCharset = charset1;
}

void Pet2001Video::setCharsetsUpper(const uint8_t* charset1_128x8, const uint8_t* charset2_128x8)
{
    upperCharset1 = charset1_128x8;
    upperCharset2 = charset2_128x8;
    charBankUpper_ = false;
}

void Pet2001Video::setCharBank(bool upper)
{
    if (!upperCharset1 || !upperCharset2 || charBankUpper_ == upper) return;
    charBankUpper_ = upper;
    selectCharset();
}

void Pet2001Video::reset()
{
    std::fill(vidram.begin(), vidram.end(), uint8_t(0x20)); // space
    std::fill(fb.begin(), fb.end(), RGBA_BLACK);
    std::fill(raster_.begin(), raster_.end(), RGBA_BLACK);
    rasterFrameReady_ = false;
    blank = false;
    blankRequested = false;
    blankCountdownMs = -1;
    // Hardware reset drives CA2 back to the graphics charset (VIA PCR resets
    // to 0); without this a reset from text mode kept rendering charset2.
    if (charset1) activeCharset = charset1;
    textSet_ = false;
    charBankUpper_ = false;
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

    // Translate physical SRAM to a visible cell, wrapping at the SRAM boundary.
    const int mask = screenMask_;
    const int cell = (addr - screenStart_) & mask;
    if (!rasterFrameReady_ && !blank && activeCharset != nullptr) {
        // Larger programmed displays can show the same SRAM byte more than once.
        for (int visibleCell = cell; visibleCell < visible_; visibleCell += mask + 1)
            drawCharCell(visibleCell, value);
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
            blank = false;
            redrawScreen();
        }
        blankRequested = false;
    }
}

void Pet2001Video::setColumns(int cols)
{
    const int nc = (cols == 80) ? 80 : 40;
    // A model switch can keep its width while changing the video hardware.
    cols_    = nc;
    rasterFrameReady_ = false;
    displayCols_ = nc;
    scanlines_ = CHAR_H;
    fbHeight_ = FB_H;
    fb.resize(FB_W * fbHeight_, RGBA_BLACK);
    screenStart_ = 0;
    scaleX_  = (nc == 80) ? 1 : 2;
    cellW_   = CHAR_W * scaleX_;
    visible_ = nc * ROWS;
    // Keep a 4 KB screen selected earlier; the caller may also set it afterwards.
    screenMask_ = screen4K_ ? 0xfff : (nc == 80) ? 0x7ff : 0x3ff;
    const int sramSize = screenMask_ + 1;
    if ((int)vidram.size() < sramSize) vidram.resize(sramSize, 0x20);
    redrawScreen();
}

void Pet2001Video::setScreen4K(bool on)
{
    screen4K_ = on;
    screenMask_ = on ? 0xfff : (cols_ == 80) ? 0x7ff : 0x3ff;
    if ((int)vidram.size() <= screenMask_) vidram.resize(screenMask_ + 1, 0x20);
    redrawScreen();
}

void Pet2001Video::enableRaster()
{
    raster_.assign(FB_W * FB_H, RGBA_BLACK);
    rasterFrameReady_ = false;
    redrawScreen();
}

void Pet2001Video::fetchCharacter(int line, int column, int glyphLine, uint16_t byteOffset)
{
    if (column * cellW_ >= FB_W) return;
    const int lastPixel = (line + 1) * SCALE * FB_W;
    if (lastPixel > static_cast<int>(raster_.size()))
        raster_.resize(lastPixel, RGBA_BLACK);
    if (blank || !activeCharset || glyphLine >= CHAR_H) return;
    const uint8_t ch = vidram[byteOffset & screenMask_];
    auto* pixels = raster_.data() + line * SCALE * FB_W + column * cellW_;
    const uint8_t bits = glyphRow(ch, glyphLine);
    for (int x = 0; x < CHAR_W; ++x) {
        const uint32_t pixel = (bits & (0x80u >> x)) ? RGBA_WHITEISH : RGBA_BLACK;
        for (int dx = 0; dx < scaleX_; ++dx) {
            pixels[x * scaleX_ + dx] = pixel;
            pixels[FB_W + x * scaleX_ + dx] = pixel;
        }
    }
}

void Pet2001Video::finishRasterFrame()
{
    fb.swap(raster_);
    fbHeight_ = static_cast<int>(fb.size()) / FB_W;
    raster_.assign(FB_W * FB_H, RGBA_BLACK);
    rasterFrameReady_ = true;
}

void Pet2001Video::setCrtcGeometry(int columns, int rows, int scanlines, uint16_t byteOffset)
{
    columns = std::clamp(columns, 0, 510);
    rows = std::clamp(rows, 0, 127);
    scanlines = std::clamp(scanlines, 1, 32);
    const int visible = columns * rows;
    if (displayCols_ == columns && visible_ == visible &&
        scanlines_ == scanlines && screenStart_ == byteOffset) return;
    displayCols_ = columns;
    visible_ = visible;
    scanlines_ = scanlines;
    screenStart_ = byteOffset;
    if (!rasterFrameReady_) {
        fbHeight_ = std::max(FB_H, rows * scanlines * SCALE);
        fb.resize(FB_W * fbHeight_, RGBA_BLACK);
    }
    redrawScreen();
}

void Pet2001Video::setScreenStart(uint16_t byteOffset)
{
    if (screenStart_ == byteOffset) return;
    screenStart_ = byteOffset;
    redrawScreen();
}

void Pet2001Video::setCharset(bool useSecond)
{
    textSet_ = useSecond;
    selectCharset();
}

// Points activeCharset at the pair chosen by the bank and the graphics/text line.
void Pet2001Video::selectCharset()
{
    const bool upper = charBankUpper_ && upperCharset1 && upperCharset2;
    const uint8_t* newSet = textSet_ ? (upper ? upperCharset2 : charset2)
                                     : (upper ? upperCharset1 : charset1);
    if (newSet == activeCharset) return;

    activeCharset = newSet;
    if (!blank && activeCharset != nullptr) {
        redrawScreen();
    }
}

void Pet2001Video::update(int elapsed_ms)
{
    // Service the pending blanking timer only. (This used to also do an
    // unconditional full redrawScreen() per call -- unnecessary, since every
    // vidram write already draws its cell incrementally.)
    if (blankCountdownMs >= 0) {
        blankCountdownMs -= std::max(0, elapsed_ms);
        if (blankCountdownMs <= 0) {
            // Timer fired -> actually blank the screen
            blankScreen();
            blank = true;
            blankCountdownMs = -1;
        }
    }
}

std::string Pet2001Video::save() const
{
    // JS string format:
    //   blankFlag ',' charsetIndex ',' then vidram bytes as hex with commas
    // Here we serialize:
    //   blank: '1'/'0'
    //   charset: '1' (charset1) or '2' (charset2)
    //   then VIDRAM_SIZE hex values with commas

    // Include physical SRAM tails now exposed by CRTC scrolling. Legacy
    // snapshots with only 1000/2000 cells remain readable by load().
    char buf[32];
    std::string s;
    s.reserve(2 + 2 + (vidram.size() * 3)); // rough

    s += (blank ? '1' : '0');
    s += ',';
    s += (textSet_ ? '2' : '1');
    s += ',';

    for (int i = 0; i < (int)vidram.size(); ++i) {
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
    parts.reserve(vidram.size() + 2);

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
        if (parts.size() > vidram.size() + 2) break;
    }

    if (parts.size() < 2) return;

    // blank flag
    blank = (!parts[0].empty() && parts[0][0] == '1');
    rasterFrameReady_ = false;
    // The snapshot format stores the effective blank state, not a pending
    // transition. Never retain a timer from the state being replaced.
    blankRequested = blank;
    blankCountdownMs = -1;

    // charset index
    bool useSecond = (!parts[1].empty() && parts[1][0] == '2');
    setCharset(useSecond);

    // vidram
    for (int i = 0; i < (int)vidram.size() && (i + 2) < (int)parts.size(); ++i) {
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

void Pet2001Video::saveState(StateWriter& w) const
{
    w.u32(static_cast<uint32_t>(vidram.size()));
    w.bytes(vidram.data(), vidram.size());
    w.i32(cols_); w.i32(screenMask_); w.flag(screen4K_);
    w.i32(displayCols_); w.i32(scanlines_); w.i32(fbHeight_); w.i32(scaleX_); w.i32(cellW_); w.i32(visible_);
    w.u16(screenStart_);
    w.flag(blank); w.flag(blankRequested); w.i32(blankCountdownMs);
    w.flag(textSet_); w.flag(charBankUpper_);
}

bool Pet2001Video::loadState(StateReader& r)
{
    const uint32_t size = r.u32();
    if (r.failed() || size < 1024 || size > 65536 || r.remaining() < size) { r.fail(); return false; }
    std::vector<uint8_t> ram(size);
    r.bytes(ram.data(), size);
    const int cols = r.i32(), mask = r.i32();
    const bool is4K = r.flag();
    const int displayCols = r.i32(), scanlines = r.i32(), height = r.i32(), scaleX = r.i32(), cellW = r.i32(), visible = r.i32();
    const uint16_t start = r.u16();
    const bool isBlank = r.flag(), requested = r.flag();
    const int countdown = r.i32();
    const bool text = r.flag(), upper = r.flag();
    // The sizes below decide how big the framebuffer gets, so check them all.
    const bool ok = !r.failed() && (cols == 40 || cols == 80) && (mask == 0x3ff || mask == 0x7ff || mask == 0xfff) &&
        size > static_cast<uint32_t>(mask) && displayCols >= 0 && displayCols <= 510 && scanlines >= 1 && scanlines <= 32 &&
        height >= 1 && height <= 16384 && (scaleX == 1 || scaleX == 2) && cellW == CHAR_W * scaleX &&
        visible >= 0 && visible <= 510 * 127 && countdown >= -1 && countdown <= 10000;
    if (!ok) { r.fail(); return false; }

    vidram.swap(ram);
    cols_ = cols; screenMask_ = mask; screen4K_ = is4K;
    displayCols_ = displayCols; scanlines_ = scanlines; fbHeight_ = height; scaleX_ = scaleX; cellW_ = cellW; visible_ = visible;
    screenStart_ = start;
    blank = isBlank; blankRequested = requested; blankCountdownMs = countdown;
    textSet_ = text; charBankUpper_ = upper;
    // Same pick as selectCharset(), but the redraw below is unconditional.
    const bool useUpper = charBankUpper_ && upperCharset1 && upperCharset2;
    activeCharset = textSet_ ? (useUpper ? upperCharset2 : charset2) : (useUpper ? upperCharset1 : charset1);
    fb.assign(static_cast<size_t>(FB_W) * height, RGBA_BLACK);
    if (!raster_.empty()) raster_.assign(static_cast<size_t>(FB_W) * FB_H, RGBA_BLACK);
    // Incremental drawing resumes at once; the next finished raster frame takes over again.
    rasterFrameReady_ = false;
    if (blank) blankScreen(); else redrawScreen();
    return true;
}

void Pet2001Video::redrawScreen()
{
    if (rasterFrameReady_) return;

    if (blank || activeCharset == nullptr) {
        // No charset -> just clear to black
        std::fill(fb.begin(), fb.end(), RGBA_BLACK);
        return;
    }

    // Clear entire screen to black, then paint each visible char cell.
    // Geometry can suppress the display or extend beyond the fixed viewport.
    std::fill(fb.begin(), fb.end(), RGBA_BLACK);

    for (int addr = 0; addr < visible_; ++addr) {
        drawCharCell(addr, vidram[(screenStart_ + addr) & screenMask_]);
    }
}

void Pet2001Video::blankScreen()
{
    // Solid black
    std::fill(fb.begin(), fb.end(), RGBA_BLACK);
    std::fill(raster_.begin(), raster_.end(), RGBA_BLACK);
}

uint8_t Pet2001Video::glyphRow(uint8_t ch, int row) const
{
    const uint8_t bits = activeCharset[(ch & 0x7f) * CHAR_H + row];
    return (ch & 0x80) ? bits ^ 0xff : bits;
}

void Pet2001Video::drawCharCell(int addr, uint8_t ch)
{
    if (activeCharset == nullptr || displayCols_ == 0) return;

    // Compute row/col from linear address
    const int col = addr % displayCols_;
    const int row = addr / displayCols_;
    const int cellH = scanlines_ * SCALE;
    if (col * cellW_ >= FB_W || row * cellH >= fbHeight_) return;

    // Black-out entire character cell first
    fillRect(col * cellW_, row * cellH, cellW_, cellH, RGBA_BLACK);

    // Foreground color (white-ish)
    const uint32_t fg = RGBA_WHITEISH;

    // Paint doubled pixels (2x2) for each set bit
    const int x0 = col * cellW_;
    const int y0 = row * cellH;

    for (int y = 0; y < std::min(scanlines_, CHAR_H); ++y) {
        const uint8_t bits = glyphRow(ch, y);

        for (int x = 0; x < CHAR_W; ++x) {
            // Test the original row byte directly. Repeated uint8_t shifts
            // misrender edge pixels with the Windows Release optimizer.
            if (bits & (0x80u >> x)) {
                // Draw a 2x2 block
                const int px = x0 + x * scaleX_;
                const int py = y0 + y * SCALE;
                // Fill 2x2 (fast-path writes)
                for (int dx = 0; dx < scaleX_; ++dx) {
                    putPixel(px + dx, py,     fg);
                    putPixel(px + dx, py + 1, fg);
                }
            }
        }
    }
}

inline void Pet2001Video::putPixel(int x, int y, uint32_t rgba)
{
    if ((unsigned)x < (unsigned)FB_W && (unsigned)y < (unsigned)fbHeight_) {
        fb[y * FB_W + x] = rgba;
    }
}

inline void Pet2001Video::fillRect(int x, int y, int w, int h, uint32_t rgba)
{
    // Clip
    int x1 = std::max(0, x);
    int y1 = std::max(0, y);
    int x2 = std::min(FB_W, x + w);
    int y2 = std::min(fbHeight_, y + h);
    if (x1 >= x2 || y1 >= y2) return;

    const int pitch = FB_W;
    for (int yy = y1; yy < y2; ++yy) {
        uint32_t* row = &fb[yy * pitch];
        for (int xx = x1; xx < x2; ++xx) {
            row[xx] = rgba;
        }
    }
}
