#ifndef PET2001VIDEO_H
#define PET2001VIDEO_H

#include <cstdint>
#include <vector>
#include <string>

/*
    Pet2001Video
    ---------------------------------------------------------------------------
    CPU-side PET 2001 text-mode video renderer, ported from pet2001video.js.

    - 40x25 characters (1000 visible bytes)
    - 8x8 font, rendered at 2x scale -> 16x16 character cells
    - Framebuffer: 640x400 (RGBA8888), black bg, "white" foreground (#EFFEFF)
    - High bit of character byte = inverse video
    - Two character sets (charset1/charset2), each 128 glyphs x 8 rows
    - Blanking control with 100ms delayed blank to reduce flicker
    - Immediate incremental draw on single-byte writes when not blank
    - Full redraw when unblanking or switching charset
    - save()/load() text snapshot helpers (compatible with original JS format)

    ROM format expected for charsets:
      - charset1: const uint8_t[128 * 8]
      - charset2: const uint8_t[128 * 8]
      Each glyph occupies 8 bytes (rows), MSB = leftmost pixel.

    Usage:
      Pet2001Video video;
      video.setCharsets(petCharRom1, petCharRom2);
      video.reset();
      ... on VIA/memory write to video RAM:
          video.write(addr, value);
      ... once per frame:
          video.update(elapsed_ms);
          // present video.framebuffer() (640x400) using your renderer

    Notes:
      - The class owns the framebuffer. Access via framebuffer() and dims().
      - No background threads/timers: caller must call update(elapsed_ms).
*/

class Pet2001Video {
public:
    static constexpr int COLS = 40;
    static constexpr int ROWS = 25;
    static constexpr int CHAR_W = 8;
    static constexpr int CHAR_H = 8;
    static constexpr int SCALE = 2;
    static constexpr int CELL_W = CHAR_W * SCALE;  // 16
    static constexpr int CELL_H = CHAR_H * SCALE;  // 16
    static constexpr int FB_W = COLS * CELL_W;     // 640
    static constexpr int FB_H = ROWS * CELL_H;     // 400
    static constexpr int VIDRAM_SIZE = COLS * ROWS; // 1000 visible (40-col default)

    // 8032 mode: 80 columns render as 8px cells (1x horizontal, 2x vertical)
    // into the SAME 640x400 framebuffer. 40-col = 16px cells (2x2).
    void setColumns(int cols);
    int  columns() const { return cols_; }
    static constexpr int BLANK_DELAY_MS = 100;

    Pet2001Video();

    // Provide the character ROMs (required before drawing anything).
    void setCharsets(const uint8_t* charset1_128x8, const uint8_t* charset2_128x8);

    // Reset state and clear screen.
    void reset();

    // Write to "video RAM" (only 0..999 are used for on-screen; others stored).
    void write(int addr, uint8_t value);

    // Blanking: true = request blanking. This uses a 100ms delayed blank like the JS.
    void setVideoBlank(bool flag);

    // Switch character set: false -> charset1, true -> charset2; triggers full redraw.
    void setCharset(bool flag);

    // Call periodically to service the blanking delay (pass elapsed milliseconds).
    void update(int elapsed_ms);

    // Serialize/deserialize state (same spirit as JS version).
    std::string save() const;
    void load(const std::string& s);

    // Access framebuffer (RGBA8888).
    const uint32_t* framebuffer() const { return fb.data(); }
    uint32_t*       framebuffer()       { return fb.data(); }
    int fbWidth() const { return FB_W; }
    int fbHeight() const { return FB_H; }

private:
    // Rendering helpers
    void redrawScreen();
    void blankScreen();
    void drawCharCell(int addr, uint8_t ch);
    inline void putPixel(int x, int y, uint32_t rgba);
    inline void fillRect(int x, int y, int w, int h, uint32_t rgba);

    // Video state
    std::vector<uint8_t> vidram; // at least VIDRAM_SIZE; we allow larger to match JS pattern
    int cols_    = COLS;         // runtime columns (40 or 80)
    int scaleX_  = SCALE;        // horizontal pixel doubling (2 at 40-col, 1 at 80-col)
    int cellW_   = CELL_W;       // cell width in fb px (16 or 8)
    int visible_ = VIDRAM_SIZE;  // cols_ * ROWS
    std::vector<uint32_t> fb;    // RGBA8888 framebuffer (FB_W x FB_H)

    // Charsets
    const uint8_t* charset1; // 128 * 8 bytes
    const uint8_t* charset2; // 128 * 8 bytes
    const uint8_t* activeCharset;

    // Blanking
    bool blank;               // current "blanked" state
    bool blankRequested;      // requested blank flag
    int  blankCountdownMs;    // >=0 while counting down

    // Colors
    static constexpr uint32_t RGBA_BLACK   = 0xFF000000u;
    // Little-endian RGBA bytes FF,EF,EF,FF = R255 G239 B239 - a slightly WARM
    // white. (An older comment claimed #EFFEFF / cool white; the warm value is
    // what has always rendered and is the accepted look - keep it.)
    static constexpr uint32_t RGBA_WHITEISH= 0xFFEFEFFFu;
};

#endif // PET2001VIDEO_H
