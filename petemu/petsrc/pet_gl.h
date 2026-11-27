#ifndef PET_GL_H
#define PET_GL_H

#include <cstdint>
#include <string>
#include <functional>
#include "glew.h"
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
    // elapsedSeconds: emulated time since the previous advancing present (drives
    // the phosphor decay); 0 = redraw of the same picture, state is not touched.
    void present(const uint32_t* rgba, int srcW, int srcH, double elapsedSeconds);

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
    // 5 contrast, 6 brightness, 7 persistence (0..1), 8 persist_ms (decay time),
    // 9 persist_tail (slow share, 0..1), 10 persist_tail_ms (slow decay time).
    static constexpr int kKnobCount = 11;
    // Value access for the settings dialog: set clamps + saves to pet.ini.
    float getKnob(int idx) const;
    void  setKnob(int idx, float v);
    void  knobRange(int idx, float* lo, float* hi, float* step) const;
    // Reset all knobs to the built-in defaults and save them to pet.ini.
    // Persistence (7-10) resets to the current machine's defaults only.
    void restoreKnobDefaults();
    // Persistence knobs 7-10 are stored per machine (<name>_m<id>); load that machine's
    // values. Cheap; call after every ROM-set change.
    void setPhosphorModel(int modelId);


private:
    PetGL() = default;
    bool init(int winW, int winH, const char* title, std::function<void(int,int,int,int)> onKey);
    void updateTexture(const uint32_t* rgba, int w, int h);
    void draw();
    bool ensurePhosphor(int w, int h);          // (re)create + clear the state textures
    void updatePhosphor(double elapsedSeconds); // one decay/excite step into the other state texture
    void destroyPhosphor();
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
    int uCrtPhosLoc     = -1;       // slow-phosphor state sampler (unit 1)
    int uCrtPersistLoc  = -1;       // persistence amount, 0 = shader never samples uPhos
    int uCrtTailLoc     = -1;       // share of the glow from the slow tail component

    // ---- Phosphor persistence: per-source-pixel state (R fast, G slow), ping-pong RG16F ----
    unsigned phosProg = 0;          // update pass: max(lit, prev * decay) per component
    int uPhosSrcLoc = -1, uPhosPrevLoc = -1, uPhosDecayLoc = -1;
    unsigned phosTex[2] = {0, 0};
    unsigned phosFbo = 0;
    int phosCur = 0;                // index of the current (latest) state texture
    int phosW = 0, phosH = 0;
    bool phosFailed = false;        // creation/FBO failed: permanent fallback, no retries

    bool  m_crt        = false;            // master CRT toggle
    bool  m_tintOn      = true;             // apply phosphor tint (CRT and plain pass)

    // True when the CRT shader is wanted and actually linked; else plain pass.
    bool crtActive() const { return m_crt && crtProg != 0; }
    float m_tint[3]     = {0.30f, 1.0f, 0.40f}; // PET-ish phosphor green

    // Tunable knobs (pet.ini [video] mono_*; live-adjustable via tune API)
    float m_blurH     = 0.8f;   // 0..3   horizontal softness, PET px
    float m_blurV     = 0.35f;  // 0..2   vertical softness, PET px
    float m_halation  = 0.15f;  // 0..1   glow strength
    float m_halRadius = 4.0f;   // 1..16  glow radius, PET px
    float m_scanline  = 0.0f;   // 0..1   beam ripple (off by default)
    float m_contrast  = 1.0f;   // 1..3   video gain: overdrive = fatter strokes
    float m_bright    = 0.0f;   // 0..0.25 black-level lift (background glow)
    float m_persist   = 0.12f;  // 0..1   phosphor persistence amount
    float m_persistMs = 1500.0f; // 25..5000 phosphor decay time constant, ms
    float m_persistTail   = 0.0f;    // 0..1 share of the glow from the slow tail (0 = off)
    float m_persistTailMs = 4000.0f; // 100..20000 tail decay time constant, ms

    int  m_phosModel = 8;       // machine whose persistence settings are active
    int  m_tuneSel = 0;        // selected knob for live tuning
    char m_tuneBuf[64] = {0};   // status string storage
    bool m_texMipFilter = false; // current texture filter state (avoid redundant sets)
};

#endif // PET_GL_H
