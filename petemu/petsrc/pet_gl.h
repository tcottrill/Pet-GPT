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

    // CRT look (mono-monitor shader: horizontal softness + halation + tint).
    // Defaults from pet.ini at init; can be toggled live.
    void setCrtEnabled(bool on);
    bool getCrtEnabled() const;

    // Monitor color for the shader: true = green phosphor tint, false = B&W.
    void setTintEnabled(bool on);
    bool getTintEnabled() const;

    // ---- Menu-driven shader knobs (View > CRT Monitor) ----
    // Knob index order matches k_knobs in pet_gl.cpp:
    // 0 blur_h, 1 blur_v, 2 halation, 3 halation_radius, 4 scanline,
    // 5 contrast, 6 brightness.
    static constexpr int kKnobCount = 7;
    // Value access for the settings dialog: set clamps + saves to pet.ini.
    float getKnob(int idx) const;
    void  setKnob(int idx, float v);
    void  knobRange(int idx, float* lo, float* hi, float* step) const;
    // Reset all knobs to the built-in defaults and save them to pet.ini.
    void restoreKnobDefaults();


private:
    PetGL() = default;
    bool init(int winW, int winH, const char* title, std::function<void(int,int,int,int)> onKey);
    void updateTexture(const uint32_t* rgba, int w, int h);
    void draw();
    void clampKnobs();
    float* knobPtr(int idx);

  
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

    // ---- CRT look: mono-monitor shader (softness + halation + tint) ----
    unsigned crtProg = 0;           // single-pass mono monitor shader
    int uCrtTexLoc      = -1;
    int uCrtSrcSizeLoc  = -1;       // PET framebuffer size (px)
    int uCrtBlurHLoc    = -1;       // horizontal spot sigma (source px)
    int uCrtBlurVLoc    = -1;       // vertical spot sigma (source px)
    int uCrtHalationLoc = -1;       // glow strength (0..1)
    int uCrtHalRadLoc   = -1;       // glow radius (source px)
    int uCrtScanLoc     = -1;       // beam ripple strength (0..1, default 0)
    int uCrtContrastLoc = -1;       // video gain (fat text via saturation)
    int uCrtBrightLoc   = -1;       // black-level lift
    int uCrtTintOnLoc   = -1;
    int uCrtTintLoc     = -1;

    bool  m_crt         = false;            // master CRT toggle
    bool  m_tintOn      = true;             // apply phosphor tint when CRT on
    float m_tint[3]     = {0.30f, 1.0f, 0.40f}; // PET-ish phosphor green

    // Tunable knobs (pet.ini [video] mono_*; live-adjustable via tune API)
    float m_blurH     = 0.8f;   // 0..3   horizontal softness, PET px
    float m_blurV     = 0.35f;  // 0..2   vertical softness, PET px
    float m_halation  = 0.15f;  // 0..1   glow strength
    float m_halRadius = 4.0f;   // 1..16  glow radius, PET px
    float m_scanline  = 0.0f;   // 0..1   beam ripple (off by default)
    float m_contrast  = 1.0f;   // 1..3   video gain: overdrive = fatter strokes
    float m_bright    = 0.0f;   // 0..0.25 black-level lift (background glow)

    int  m_tuneSel = 0;         // selected knob for live tuning
    char m_tuneBuf[64] = {0};   // status string storage
    bool m_texMipFilter = false; // current texture filter state (avoid redundant sets)
};

#endif // PET_GL_H
