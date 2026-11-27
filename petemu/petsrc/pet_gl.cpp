#include "pet_gl.h"
#include "framework.h"

#include "sys_log.h"
#include "iniFile.h"    // get_config_* (pet.ini already opened by the host)
#include "phosphor_profile.h"

#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <functional>

static const char* VS_SRC = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    vUV = vec2(aUV.x, 1.0 - aUV.y); // single vertical flip
    gl_Position = vec4(aPos, 0.0, 1.0);
})";

static const char* FS_SRC = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform int  uTintOn;
uniform vec3 uTint;
void main(){
    vec4 c = texture(uTex, vUV);
    if (uTintOn != 0) c.rgb *= uTint;   // monochrome PET image -> phosphor tint
    fragColor = c;
}
)";

// ---- Mono monitor pass: single-pass B&W/green-screen CRT simulation (VICE-ish,
//      restrained). All work happens in SOURCE-pixel space (the 640x400 PET
//      framebuffer) so the look is identical at any window size:
//        1. Gaussian beam spot  -- 7x3 taps; uBlurH is the video-bandwidth
//           softness along the scanline, uBlurV a touch of vertical spot size.
//        2. Halation           -- 4 textureLod() taps from the source texture's
//           mip pyramid (glGenerateMipmap after each upload), screen-blended.
//           This is the no-FBO glow trick that keeps the AAE backport trivial.
//        3. Optional beam ripple keyed to the 200 doubled raster lines
//           (uScanline, default 0 = off).
//        4. Phosphor tint multiply.
static const char* CRT_FS_SRC = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform sampler2D uTex;
uniform vec2  uSrcSize;    // PET framebuffer size in px (e.g. 640x400)
uniform float uBlurH;      // horizontal spot sigma, source px
uniform float uBlurV;      // vertical spot sigma, source px
uniform float uHalation;   // glow strength 0..1
uniform float uHalRadius;  // glow radius, source px
uniform float uScanline;   // beam ripple strength 0..1 (0 = off)
uniform float uContrast;   // video gain (>=1); overdrive fattens strokes via saturation
uniform float uBright;     // black-level lift 0..0.25 (misadjusted-tube glow)
uniform int   uTintOn;
uniform vec3  uTint;
uniform sampler2D uPhos;   // phosphor state (R = fast, G = slow tail; same UVs as uTex), unit 1
uniform float uPersist;    // 0 = persistence off; uPhos is then never sampled
uniform float uTail;       // share of the glow that belongs to the slow component, 0..1

void main(){
    vec2 px = 1.0 / uSrcSize;

    // 1) Gaussian spot (7 horizontal x 3 vertical taps, source-pixel space).
    //    Near-zero sigma degenerates to the plain (bilinear) sample.
    float sh = max(uBlurH, 0.02);
    float sv = max(uBlurV, 0.02);
    vec3  acc  = vec3(0.0);
    vec2  accP = vec2(0.0);    // same taps through the phosphor state (linear, combined once below)
    float wsum = 0.0;
    for (int i = -3; i <= 3; ++i) {
        float wx = exp(-0.5 * float(i*i) / (sh*sh));
        for (int j = -1; j <= 1; ++j) {
            float w = wx * exp(-0.5 * float(j*j) / (sv*sv));
            vec2 tc = vUV + vec2(float(i), float(j)) * px;
            acc  += w * texture(uTex, tc).rgb;
            if (uPersist > 0.0) accP += w * texture(uPhos, tc).rg;
            wsum += w;
        }
    }
    vec3 col = acc / wsum;
    if (uPersist > 0.0) {
        // The blend is linear, so combining after the tap sum is exact.
        float g = mix(accP.r, accP.g, uTail) / wsum;
        col = mix(col, vec3(g), uPersist);
    }

    // 1b) Beam overdrive: video gain, clamped like a saturating phosphor.
    //     Gain pushes the spot's dim skirt past full-white, so strokes get
    //     FATTER while the clamp keeps their edges hard (fat, not blurry).
    //     Clamp before halation: the screen blend needs values <= 1.
    col = min(col * uContrast, vec3(1.0));

    // 2) Halation: cheap wide blur from the mip pyramid, screen blend so the
    //    glow brightens darks without clipping whites.
    if (uHalation > 0.0) {
        float lod = log2(max(uHalRadius, 1.0));
        vec2  o   = px * uHalRadius * 0.5;
        vec3 glow = textureLod(uTex, vUV + vec2(-o.x, -o.y), lod).rgb
                  + textureLod(uTex, vUV + vec2( o.x, -o.y), lod).rgb
                  + textureLod(uTex, vUV + vec2(-o.x,  o.y), lod).rgb
                  + textureLod(uTex, vUV + vec2( o.x,  o.y), lod).rgb;
        glow *= 0.25;
        if (uPersist > 0.0) {
            vec2 gp = textureLod(uPhos, vUV + vec2(-o.x, -o.y), lod).rg
                    + textureLod(uPhos, vUV + vec2( o.x, -o.y), lod).rg
                    + textureLod(uPhos, vUV + vec2(-o.x,  o.y), lod).rg
                    + textureLod(uPhos, vUV + vec2( o.x,  o.y), lod).rg;
            glow = mix(glow, vec3(mix(gp.r, gp.g, uTail) * 0.25), uPersist);
        }
        col = 1.0 - (1.0 - col) * (1.0 - glow * uHalation);
    }

    // 3) Optional soft beam ripple. The PET framebuffer doubles each raster
    //    line (400 rows = 200 lines), so beam centers sit at fbY = 2k+1.
    if (uScanline > 0.0) {
        float fbY = vUV.y * uSrcSize.y;
        float w   = 0.5 + 0.5 * cos(3.14159265 * (fbY - 1.0)); // 1 at centers, 0 in gaps
        col *= 1.0 - uScanline * (1.0 - w);
    }

    // 3b) Black-level lift, before tint so the raised background glows in the
    //     phosphor color rather than gray.
    col += uBright;

    // 4) Phosphor tint (monochrome image -> green screen / B&W).
    if (uTintOn != 0) col *= uTint;
    fragColor = vec4(col, 1.0);
}
)";

// ---- Phosphor update pass: F,S_new = max(lit, F,S_old * decay), one texel per
//      source pixel. Uses gl_FragCoord + texelFetch, so the state texel (x,y)
//      always corresponds to source texel (x,y): the quad's V flip in VS_SRC
//      only affects vUV, which this pass ignores.
static const char* PHOS_FS_SRC = R"(
#version 330 core
out vec4 fragColor;
uniform sampler2D uSrc;    // source framebuffer (unit 0; its filter may be LINEAR)
uniform sampler2D uPrev;   // previous state (unit 1)
uniform vec2  uDecay;      // exp(-dt / tau): x = fast, y = slow tail
void main(){
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 s = texelFetch(uSrc, p, 0).rgb;
    float lit = max(s.r, max(s.g, s.b));
    vec2 prev = texelFetch(uPrev, p, 0).rg;   // R = fast, G = slow tail
    fragColor = vec4(max(vec2(lit), prev * uDecay), 0.0, 1.0);
}
)";

static void GLAPIENTRY gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity,
    GLsizei length, const GLchar* message, const void* userParam)
{
    (void)source; (void)type; (void)id; (void)severity; (void)length; (void)userParam;
    LOG_ERROR("[GL] %s", message);
}

static GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetShaderInfoLog(s, sizeof(log), &n, log);
        LOG_ERROR("Shader compile error: %.*s", n, log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint link(GLuint vs, GLuint fs) {
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096]; GLsizei n = 0;
        glGetProgramInfoLog(p, sizeof(log), &n, log);
        LOG_ERROR("Program link error: %.*s", n, log);
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

// ---- RawInput bridge: forward to std::function passed into PetGL::create ----
static std::function<void(int, int, int, int)> g_onKey;


bool PetGL::init(int winW, int winH, const char* title,
    std::function<void(int, int, int, int)> onKey)
{
    // simple quad covering the entire NDC
    // pos(x,y), uv(u,v)
    float quad[] = {
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f, -1.f,  0.f, 0.f,
         1.f,  1.f,  1.f, 1.f,
        -1.f,  1.f,  0.f, 1.f
    };

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glBindVertexArray(0);

    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
    prog = link(vs, fs);
    glDeleteShader(vs);
    glDeleteShader(fs);

    if (!prog) return false;
    glUseProgram(prog);
    uTexLoc = glGetUniformLocation(prog, "uTex");
    uDstSizeLoc = glGetUniformLocation(prog, "uDstSize");
    uSrcSizeLoc = glGetUniformLocation(prog, "uSrcSize");
    uTintOnLoc = glGetUniformLocation(prog, "uTintOn");
    uTintLoc = glGetUniformLocation(prog, "uTint");
    glUniform1i(uTexLoc, 0);

    // Texture setup (no data yet)
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texW = texH = 0;

    // ---- CRT look: read defaults from pet.ini (host has already opened it) ----
    m_crt       = get_config_bool ("video", "crt", true);    // default: shader on, green phosphor
    m_tintOn    = get_config_bool ("video", "crt_tint", true);
    m_tint[0]   = get_config_float("video", "crt_tint_r", 0.30f);
    m_tint[1]   = get_config_float("video", "crt_tint_g", 1.0f);
    m_tint[2]   = get_config_float("video", "crt_tint_b", 0.40f);
    m_blurH     = get_config_float("video", "mono_blur_h", 0.8f);
    m_blurV     = get_config_float("video", "mono_blur_v", 0.35f);
    m_halation  = get_config_float("video", "mono_halation", 0.15f);
    m_halRadius = get_config_float("video", "mono_halation_radius", 4.0f);
    m_scanline  = get_config_float("video", "mono_scanline", 0.0f);
    m_contrast  = get_config_float("video", "mono_contrast", 1.0f);
    m_bright    = get_config_float("video", "mono_brightness", 0.0f);
    // Persistence is per machine: sane fallback row until setPhosphorModel() is called.
    {
        const PhosphorDefaults d = phosphor_defaults(m_phosModel);
        m_persist = d.persistence; m_persistMs = d.decayMs;
        m_persistTail = d.tail;    m_persistTailMs = d.tailMs;
    }
    clampKnobs();

    // ---- Mono monitor program (single pass; reuses the same quad VAO/VS) ----
    {
        GLuint cvs = compile(GL_VERTEX_SHADER, VS_SRC);
        GLuint cfs = compile(GL_FRAGMENT_SHADER, CRT_FS_SRC);
        // A failed compile yields 0; skip link so we never hand it a bad shader.
        crtProg = (cvs && cfs) ? link(cvs, cfs) : 0;
        glDeleteShader(cvs);
        glDeleteShader(cfs);
        if (crtProg != 0) {
            glUseProgram(crtProg);
            uCrtTexLoc      = glGetUniformLocation(crtProg, "uTex");
            uCrtSrcSizeLoc  = glGetUniformLocation(crtProg, "uSrcSize");
            uCrtBlurHLoc    = glGetUniformLocation(crtProg, "uBlurH");
            uCrtBlurVLoc    = glGetUniformLocation(crtProg, "uBlurV");
            uCrtHalationLoc = glGetUniformLocation(crtProg, "uHalation");
            uCrtHalRadLoc   = glGetUniformLocation(crtProg, "uHalRadius");
            uCrtScanLoc     = glGetUniformLocation(crtProg, "uScanline");
            uCrtContrastLoc = glGetUniformLocation(crtProg, "uContrast");
            uCrtBrightLoc   = glGetUniformLocation(crtProg, "uBright");
            uCrtTintOnLoc   = glGetUniformLocation(crtProg, "uTintOn");
            uCrtTintLoc     = glGetUniformLocation(crtProg, "uTint");
            uCrtPhosLoc     = glGetUniformLocation(crtProg, "uPhos");
            uCrtPersistLoc  = glGetUniformLocation(crtProg, "uPersist");
            uCrtTailLoc     = glGetUniformLocation(crtProg, "uTail");
            glUniform1i(uCrtTexLoc, 0);
            glUniform1i(uCrtPhosLoc, 1);
            glUniform1f(uCrtPersistLoc, 0.0f);

            // Persistence update program; failure only disables persistence.
            GLuint pvs = compile(GL_VERTEX_SHADER, VS_SRC);
            GLuint pfs = compile(GL_FRAGMENT_SHADER, PHOS_FS_SRC);
            phosProg = (pvs && pfs) ? link(pvs, pfs) : 0;
            glDeleteShader(pvs);
            glDeleteShader(pfs);
            if (phosProg != 0) {
                glUseProgram(phosProg);
                uPhosSrcLoc   = glGetUniformLocation(phosProg, "uSrc");
                uPhosPrevLoc  = glGetUniformLocation(phosProg, "uPrev");
                uPhosDecayLoc = glGetUniformLocation(phosProg, "uDecay");
                glUniform1i(uPhosSrcLoc, 0);
                glUniform1i(uPhosPrevLoc, 1);
            }
            else {
                LOG_ERROR("[GL] phosphor shader unavailable; persistence disabled");
                phosFailed = true;
            }
        }
        else {
            // Not fatal: draw() falls back to the plain pass instead of a black screen.
            LOG_ERROR("[GL] CRT shader unavailable; using the plain pass");
        }
    }

    glUseProgram(0);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    return true;
}

void PetGL::setCrtEnabled(bool on) { m_crt = on; }
bool PetGL::getCrtEnabled() const  { return m_crt; }

void PetGL::setTintEnabled(bool on) { m_tintOn = on; }
bool PetGL::getTintEnabled() const  { return m_tintOn; }

// ---- Live shader tuning -----------------------------------------------------
// One table drives cycle order, ini names, steps and clamp ranges; knobPtr()
// maps table index -> member.
static const struct { const char* name; float step, lo, hi, def; } k_knobs[] = {
    // name                   step   lo    hi     default (= the accepted look)
    { "mono_blur_h",          0.05f, 0.0f, 2.5f,  0.80f },
    { "mono_blur_v",          0.05f, 0.0f, 1.0f,  0.35f },
    { "mono_halation",        0.02f, 0.0f, 1.0f,  0.15f },
    { "mono_halation_radius", 0.5f,  1.0f, 16.0f, 4.00f },
    { "mono_scanline",        0.02f, 0.0f, 1.0f,  0.00f },
    { "mono_contrast",        0.05f, 1.0f, 3.0f,  1.00f },
    { "mono_brightness",      0.01f, 0.0f, 0.25f, 0.00f },
    { "mono_persistence",     0.02f, 0.0f, 1.0f,  0.12f },
    { "mono_persist_ms",      25.0f, 25.0f, 5000.0f, 1500.0f },  // the 8296 tube glows for seconds
    { "mono_persist_tail",    0.02f, 0.0f, 1.0f,  0.00f },       // 0 = single decay, as before
    { "mono_persist_tail_ms", 100.0f, 100.0f, 20000.0f, 4000.0f },
};
static const int k_knobCount = (int)(sizeof(k_knobs) / sizeof(k_knobs[0]));

float* PetGL::knobPtr(int idx)
{
    switch (idx) {
    case 0: return &m_blurH;
    case 1: return &m_blurV;
    case 2: return &m_halation;
    case 3: return &m_halRadius;
    case 4: return &m_scanline;
    case 5: return &m_contrast;
    case 6: return &m_bright;
    case 7: return &m_persist;
    case 8: return &m_persistMs;
    case 9: return &m_persistTail;
    case 10: return &m_persistTailMs;
    default: return nullptr;   // callers range-check; never alias a real knob
    }
}

void PetGL::clampKnobs()
{
    for (int i = 0; i < k_knobCount; ++i) {
        float* v = knobPtr(i);
        if (*v < k_knobs[i].lo) *v = k_knobs[i].lo;
        if (*v > k_knobs[i].hi) *v = k_knobs[i].hi;
    }
}

float PetGL::getKnob(int idx) const
{
    return (idx >= 0 && idx < k_knobCount) ? *const_cast<PetGL*>(this)->knobPtr(idx) : 0.0f;
}

void PetGL::knobRange(int idx, float* lo, float* hi, float* step) const
{
    if (idx < 0 || idx >= k_knobCount) { *lo = *hi = *step = 0; return; }
    *lo = k_knobs[idx].lo; *hi = k_knobs[idx].hi; *step = k_knobs[idx].step;
}

void PetGL::setKnob(int idx, float v)
{
    if (idx < 0 || idx >= k_knobCount) return;
    *knobPtr(idx) = v;
    clampKnobs();
    // Live apply happens on the next draw (uniforms set every frame);
    // persist immediately so the setting survives however the app exits.
    // Persistence knobs (7-10) are saved per machine; the rest are global.
    if (idx >= 7) set_config_float("video", phosphor_key(k_knobs[idx].name, m_phosModel).c_str(), *knobPtr(idx));
    else set_config_float("video", k_knobs[idx].name, *knobPtr(idx));
}

void PetGL::restoreKnobDefaults()
{
    for (int i = 0; i < 7; ++i) {
        *knobPtr(i) = k_knobs[i].def;
        set_config_float("video", k_knobs[i].name, k_knobs[i].def);
    }
    // Persistence goes back to the running machine's row only.
    const PhosphorDefaults d = phosphor_defaults(m_phosModel);
    const float v[4] = { d.persistence, d.decayMs, d.tail, d.tailMs };
    for (int i = 7; i < k_knobCount; ++i) {
        *knobPtr(i) = v[i - 7];
        clampKnobs();
        set_config_float("video", phosphor_key(k_knobs[i].name, m_phosModel).c_str(), *knobPtr(i));
    }
    LOG_INFO("[CRT] shader knobs restored to defaults (machine %d)", m_phosModel);
}

void PetGL::setPhosphorModel(int modelId)
{
    m_phosModel = modelId;
    const PhosphorDefaults d = phosphor_defaults(modelId);
    float def[4] = { d.persistence, d.decayMs, d.tail, d.tailMs };
    for (int i = 7; i < k_knobCount; ++i) {
        float fb = def[i - 7];
        // The 8296 was tuned before per-machine keys: the old global key is its fallback.
        // (The ini helper cannot report an absent key, so it is read with the 8296 default.)
        if (modelId == 96) fb = get_config_float("video", k_knobs[i].name, fb);
        *knobPtr(i) = get_config_float("video", phosphor_key(k_knobs[i].name, modelId).c_str(), fb);
    }
    clampKnobs();
}

PetGL* PetGL::create(int winW, int winH, const char* title,
    std::function<void(int, int, int, int)> onKey)
{
    PetGL* g = new PetGL();
    if (!g->init(winW, winH, title, onKey)) {
        delete g;
        return nullptr;
    }
    return g;
}

PetGL::~PetGL()
{
    if (tex) glDeleteTextures(1, &tex);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (prog) glDeleteProgram(prog);
    if (crtProg) glDeleteProgram(crtProg);
    if (phosProg) glDeleteProgram(phosProg);
    destroyPhosphor();
}

void PetGL::destroyPhosphor()
{
    if (phosTex[0] || phosTex[1]) glDeleteTextures(2, phosTex);
    phosTex[0] = phosTex[1] = 0;
    if (phosFbo) glDeleteFramebuffers(1, &phosFbo);
    phosFbo = 0;
    phosW = phosH = 0;
}

// (Re)create both state textures at w x h, cleared to 0. One-time FBO check; on
// any failure persistence is disabled for good and the old look is used.
bool PetGL::ensurePhosphor(int w, int h)
{
    if (phosFailed) return false;
    if (phosTex[0] && phosW == w && phosH == h) return true;

    GLint prevFbo = 0, prevTex = 0, vp[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &prevTex);
    glGetIntegerv(GL_VIEWPORT, vp);

    destroyPhosphor();
    glGenTextures(2, phosTex);
    glGenFramebuffers(1, &phosFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, phosFbo);
    glClearColor(0.f, 0.f, 0.f, 0.f);
    bool ok = phosTex[0] && phosTex[1] && phosFbo;
    for (int i = 0; ok && i < 2; ++i) {
        glBindTexture(GL_TEXTURE_2D, phosTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, w, h, 0, GL_RG, GL_HALF_FLOAT, nullptr);
        // The CRT pass blurs and takes mip halation from the state like from the source.
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, phosTex[i], 0);
        if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) ok = false;
        else { glClear(GL_COLOR_BUFFER_BIT); glGenerateMipmap(GL_TEXTURE_2D); }
    }

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevFbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)prevTex);
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glClearColor(0.f, 0.f, 0.f, 1.f);   // draw() relies on this

    if (!ok) {
        LOG_ERROR("[GL] phosphor state (RG16F %dx%d) unavailable; persistence disabled", w, h);
        destroyPhosphor();
        phosFailed = true;
        return false;
    }
    phosW = w; phosH = h; phosCur = 0;
    return true;
}

// One step: render max(lit, prev*decay) per component into the other texture, then swap.
void PetGL::updatePhosphor(double elapsedSeconds)
{
    if (!ensurePhosphor(texW, texH)) return;

    GLint prevFbo = 0, vp[4];
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevFbo);   // host may not use 0
    glGetIntegerv(GL_VIEWPORT, vp);                          // host set a letterboxed one

    const int dst = 1 - phosCur;
    // Guard against a zero time constant (knob ranges forbid it, ini is clamped anyway).
    const double tauF = (std::max)(m_persistMs * 0.001, 1e-3);   // parens: windows.h max macro
    const double tauS = (std::max)(m_persistTailMs * 0.001, 1e-3);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, phosFbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, phosTex[dst], 0);
    glViewport(0, 0, texW, texH);

    glUseProgram(phosProg);
    glUniform2f(uPhosDecayLoc, (float)std::exp(-elapsedSeconds / tauF),
                               (float)std::exp(-elapsedSeconds / tauS));
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, phosTex[phosCur]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
    glUseProgram(0);

    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)prevFbo);
    glViewport(vp[0], vp[1], vp[2], vp[3]);

    phosCur = dst;
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, phosTex[phosCur]);
    glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
}

void PetGL::updateTexture(const uint32_t* rgba, int w, int h)
{
    if (!rgba || w <= 0 || h <= 0) return;

    glBindTexture(GL_TEXTURE_2D, tex);

    // Ensure no row padding issues (our data is tightly packed RGBA8)
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    if (w != texW || h != texH) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        texW = w; texH = h;
    }
    else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
            GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }

    // CRT path samples smoothly and pulls halation from the mip pyramid;
    // plain path stays pixel-sharp NEAREST. (Source is 640x400 -> mip
    // regeneration is cheap.)
    if (crtActive()) {
        if (!m_texMipFilter) {
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            m_texMipFilter = true;
        }
        glGenerateMipmap(GL_TEXTURE_2D);
    }
    else if (m_texMipFilter) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        m_texMipFilter = false;
    }
}

void PetGL::draw()
{
    // The host has already set glViewport() to the centered, aspect-fit rect.
    // Clear the whole window (letterbox bars stay black), then draw the quad
    // into the active viewport.
    glClear(GL_COLOR_BUFFER_BIT);
    if (texW <= 0 || texH <= 0) return;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);

    bool persistOn = false;
    if (crtActive()) {
        // ---- Mono monitor pass: softness + halation + optional ripple + tint ----
        glUseProgram(crtProg);
        glUniform2f(uCrtSrcSizeLoc, (float)texW, (float)texH);
        glUniform1f(uCrtBlurHLoc,    m_blurH);
        glUniform1f(uCrtBlurVLoc,    m_blurV);
        glUniform1f(uCrtHalationLoc, m_halation);
        glUniform1f(uCrtHalRadLoc,   m_halRadius);
        glUniform1f(uCrtScanLoc,     m_scanline);
        glUniform1f(uCrtContrastLoc, m_contrast);
        glUniform1f(uCrtBrightLoc,   m_bright);
        glUniform1i(uCrtTintOnLoc,   m_tintOn ? 1 : 0);
        glUniform3f(uCrtTintLoc, m_tint[0], m_tint[1], m_tint[2]);
        if (m_persist > 0.0f && !phosFailed && phosTex[0]) {
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, phosTex[phosCur]);
            glActiveTexture(GL_TEXTURE0);
            glUniform1f(uCrtPersistLoc, m_persist);
            glUniform1f(uCrtTailLoc, m_persistTail);
            persistOn = true;
        }
        else glUniform1f(uCrtPersistLoc, 0.0f);
    }
    else {
        // ---- Plain pass: pixel-sharp passthrough, still tinted by the monitor
        // colour so Green phosphor holds with the CRT look off ----
        glUseProgram(prog);
        glUniform1i(uTintOnLoc, m_tintOn ? 1 : 0);
        glUniform3f(uTintLoc, m_tint[0], m_tint[1], m_tint[2]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);

    if (persistOn) {   // leave unit 1 clean for the plain pass
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
    }
    glBindVertexArray(0);
    glUseProgram(0);
}

void PetGL::present(const uint32_t* rgba, int srcW, int srcH, double elapsedSeconds)
{
    updateTexture(rgba, srcW, srcH);
    // Persistence is a tube property: CRT path only. elapsed == 0 is a redraw
    // (repaint, screenshot) and must not advance or fade the state.
    if (crtActive() && elapsedSeconds > 0.0 && phosProg != 0 && !phosFailed && texW > 0)
        updatePhosphor(elapsedSeconds);
    else if (crtActive() && !phosFailed && phosProg != 0 && texW > 0 && !phosTex[0])
        updatePhosphor(0.0);   // first redraw before any advance: create state so it is never sampled unallocated
    draw();
}
