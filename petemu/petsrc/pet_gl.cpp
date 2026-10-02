#include "pet_gl.h"
#include "framework.h"
#include "rawinput.h"   // RawInput callback registration
#include "sys_log.h"
#include "iniFile.h"    // get_config_* (pet.ini already opened by the host)

#include <cstdio>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>
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

void main(){
    vec2 px = 1.0 / uSrcSize;

    // 1) Gaussian spot (7 horizontal x 3 vertical taps, source-pixel space).
    //    Near-zero sigma degenerates to the plain (bilinear) sample.
    float sh = max(uBlurH, 0.02);
    float sv = max(uBlurV, 0.02);
    vec3  acc  = vec3(0.0);
    float wsum = 0.0;
    for (int i = -3; i <= 3; ++i) {
        float wx = exp(-0.5 * float(i*i) / (sh*sh));
        for (int j = -1; j <= 1; ++j) {
            float w = wx * exp(-0.5 * float(j*j) / (sv*sv));
            acc  += w * texture(uTex, vUV + vec2(float(i), float(j)) * px).rgb;
            wsum += w;
        }
    }
    vec3 col = acc / wsum;

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


static void APIENTRY gl_debug(GLenum source, GLenum type, GLuint id, GLenum severity,
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
    m_crt       = get_config_bool ("video", "crt", false);
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
    clampKnobs();

    // ---- Mono monitor program (single pass; reuses the same quad VAO/VS) ----
    {
        GLuint cvs = compile(GL_VERTEX_SHADER, VS_SRC);
        GLuint cfs = compile(GL_FRAGMENT_SHADER, CRT_FS_SRC);
        crtProg = link(cvs, cfs);
        glDeleteShader(cvs);
        glDeleteShader(cfs);
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
        glUniform1i(uCrtTexLoc, 0);
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
    default: return &m_bright;
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
    set_config_float("video", k_knobs[idx].name, *knobPtr(idx));
}

void PetGL::restoreKnobDefaults()
{
    for (int i = 0; i < k_knobCount; ++i) {
        *knobPtr(i) = k_knobs[i].def;
        set_config_float("video", k_knobs[i].name, k_knobs[i].def);
    }
    LOG_INFO("[CRT] shader knobs restored to defaults");
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
    if (m_crt) {
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

    if (m_crt) {
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
    }
    else {
        // ---- Plain pass: pixel-sharp passthrough, no tint ----
        glUseProgram(prog);
        glUniform1i(uTintOnLoc, 0);
        glUniform3f(uTintLoc, m_tint[0], m_tint[1], m_tint[2]);
    }
    glDrawArrays(GL_TRIANGLES, 0, 6);

    glBindVertexArray(0);
    glUseProgram(0);
}

void PetGL::present(const uint32_t* rgba, int srcW, int srcH)
{
    updateTexture(rgba, srcW, srcH);
    draw();
}
