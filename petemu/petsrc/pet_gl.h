#ifndef PET_GL_H
#define PET_GL_H

#include <cstdint>
#include <string>
#include <functional>
#include "sys_gl.h"
#include "framework.h"


// Forward-declare to avoid including your whole emulator here
class PetMachine;

class PetGL {
public:
    // Create a window and GL context. Returns nullptr on failure.
    // onKey is optional: you can wire this to PetKeys later.
    static PetGL* create(int winW = 960, int winH = 600,
                         const char* title = "PET 2001",
                         std::function<void(int key, int sc, int action, int mods)> onKey = nullptr);

    ~PetGL();

    // Push latest PET framebuffer (RGBA8888 640x400) to the GPU and draw it.
    // You should call this once per emu frame.
    void present(const uint32_t* rgba, int srcW, int srcH);

    // CRT look (phosphor-green tint + tiled scanline multiply). Defaults from
    // pet.ini at init; can be toggled live.
    void setCrtEnabled(bool on);
    bool getCrtEnabled() const;


private:
    PetGL() = default;
    bool init(int winW, int winH, const char* title, std::function<void(int,int,int,int)> onKey);
    void updateTexture(const uint32_t* rgba, int w, int h);
    void draw();

  
    // GL resource ids
    unsigned tex = 0;
    unsigned vao = 0;
    unsigned vbo = 0;
    unsigned prog = 0;

    int texW = 0, texH = 0;

    // cached uniform locations
    int uTexLoc = -1;
    int uDstSizeLoc = -1;
    int uSrcSizeLoc = -1;
    int uTintOnLoc = -1;
    int uTintLoc = -1;

    // ---- CRT look (scanlines + phosphor tint) ----
    unsigned scanProg = 0;          // grille pass program (procedural, dot-locked)
    unsigned scanTex  = 0;          // (legacy texture; unused by the procedural grille)
    int uScanSrcSizeLoc = -1;       // PET framebuffer size (px)
    int uScanPitchLoc   = -1;       // grille line spacing (framebuffer px)
    int uScanThickLoc   = -1;       // grille dark-line width (framebuffer px)
    int uScanDimLoc     = -1;       // grille dark-line brightness (0..1)

    bool  m_crt         = false;            // master CRT toggle (currently = green phosphor tint)
    bool  m_tintOn      = true;             // apply phosphor tint when CRT on
    // Scanline/grille overlay is DISABLED for now (PET was a mono green-phosphor
    // screen, no shadow mask). The procedural scanline shader below is kept,
    // unused, as a starting point for a proper VICE-style CRT shader later.
    bool  m_scanlines   = false;
    float m_tint[3]     = {0.30f, 1.0f, 0.40f}; // PET-ish phosphor green
    float m_scanStrength = 0.55f;           // scanline dark-gap brightness: lower = stronger
    float m_grillePitch     = 2.0f;         // scanline spacing, framebuffer rows (2 = 1/raster line)
    float m_grilleThickness = 2.5f;         // scanline dark-line thickness, SCREEN pixels (fixed)

    // PNG scanline texture (loaded via stb_image; 0 = use procedural)
    int  m_scanTexW    = 0;
    int  m_scanTexH    = 0;
    bool m_scanFromFile = false;
};

#endif // PET_GL_H
