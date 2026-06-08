#include "pet_gl.h"
#include "framework.h"
#include "rawinput.h"   // RawInput callback registration
#include "sys_log.h"
#include "iniFile.h"    // get_config_* (pet.ini already opened by the host)
#include "stb_image.h"  // PNG loader for scanline texture

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

// ---- Scanline pass: PROCEDURAL horizontal CRT scanlines (one dark gap per PET
//      raster row, like a real raster CRT / the AAE "mappy" look), multiplied in
//      (glBlendFunc(GL_DST_COLOR, GL_ZERO)). Computed in framebuffer ROWS so it
//      locks to the raster lines and scales with the image; fwidth() anti-aliases
//      each line so it stays smooth (never chunky) at any zoom.
static const char* SCAN_VS_SRC = R"(
#version 330 core
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main(){ vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }
)";

static const char* SCAN_FS_SRC = R"(
#version 330 core
in vec2 vUV;
out vec4 fragColor;
uniform vec2  uSrcSize;    // PET framebuffer size in px (e.g. 640x400)
uniform float uPitch;      // scanline SPACING, framebuffer ROWS (2 = one per raster line; scales with image)
uniform float uThickness;  // dark-line THICKNESS in SCREEN pixels (fixed -> consistent at any zoom)
uniform float uDim;        // dark-line brightness (0..1)
void main(){
    float fbY    = vUV.y * uSrcSize.y;          // position down the framebuffer (raster rows)
    float ph     = mod(fbY, uPitch);
    float dRows  = min(ph, uPitch - ph);        // distance to nearest scanline (framebuffer rows)
    float rowsPP = max(fwidth(fbY), 1e-4);      // framebuffer rows per screen pixel
    float dPx    = dRows / rowsPP;              // distance to the scanline, in SCREEN pixels
    // Dark line of fixed SCREEN width uThickness (so spacing scales with the image
    // but the line stays the same crisp thickness whether at 2x or 4K), ~1px AA.
    float cov = 1.0 - smoothstep(uThickness * 0.5 - 0.75, uThickness * 0.5 + 0.75, dPx);
    float b   = mix(1.0, uDim, cov);            // dark line -> uDim, between -> 1.0
    fragColor = vec4(vec3(b), 1.0);
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
    m_crt          = get_config_bool ("video", "crt", false);
    m_tintOn       = get_config_bool ("video", "crt_tint", true);
    m_scanStrength = get_config_float("video", "crt_scanline_strength", 0.55f);
    m_grillePitch     = get_config_float("video", "crt_grille_pitch", 2.0f);
    m_grilleThickness = get_config_float("video", "crt_grille_thickness", 2.5f);
    if (m_grillePitch < 1.0f)     m_grillePitch = 1.0f;
    if (m_grilleThickness < 0.0f) m_grilleThickness = 0.0f;
    m_tint[0] = get_config_float("video", "crt_tint_r", 0.30f);
    m_tint[1] = get_config_float("video", "crt_tint_g", 1.0f);
    m_tint[2] = get_config_float("video", "crt_tint_b", 0.40f);

    // ---- Grille program (procedural; reuses the same quad VAO) ----
    {
        GLuint svs = compile(GL_VERTEX_SHADER, SCAN_VS_SRC);
        GLuint sfs = compile(GL_FRAGMENT_SHADER, SCAN_FS_SRC);
        scanProg = link(svs, sfs);
        glDeleteShader(svs);
        glDeleteShader(sfs);
        glUseProgram(scanProg);
        uScanSrcSizeLoc = glGetUniformLocation(scanProg, "uSrcSize");
        uScanPitchLoc   = glGetUniformLocation(scanProg, "uPitch");
        uScanThickLoc   = glGetUniformLocation(scanProg, "uThickness");
        uScanDimLoc     = glGetUniformLocation(scanProg, "uDim");
    }

    // ---- Procedural scanline texture: 1x2 RGBA, row0 = white (bright line),
    //      row1 = dim gray (= m_scanStrength) so every other emulated line is
    //      darkened when multiplied over the image. REPEAT + NEAREST. ----
    {
        float s = m_scanStrength;
        if (s < 0.0f) s = 0.0f; else if (s > 1.0f) s = 1.0f;
        unsigned char d = (unsigned char)(s * 255.0f + 0.5f);
        unsigned char pixels[2 * 4] = {
            255, 255, 255, 255,   // row 0: bright
            d,   d,   d,   255    // row 1: dim
        };
        glGenTextures(1, &scanTex);
        glBindTexture(GL_TEXTURE_2D, scanTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 2, 0,
            GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
        // Procedural cell is 1x2; a PNG (below) overrides these on success. Both
        // paths tile relative to the output viewport in draw().
        m_scanTexW = 1; m_scanTexH = 2;
    }

    // ---- Scanline texture: prefer a PNG file (configurable), else procedural. ----
    {
        std::string scanFile = get_config_string("video", "crt_scanline_texture", "scanlines.png");
        if (scanFile.empty()) scanFile = "scanlines.png";
        int sw = 0, sh = 0, sc = 0;
        stbi_set_flip_vertically_on_load(0);
        unsigned char* px = stbi_load(scanFile.c_str(), &sw, &sh, &sc, 4);
        if (px && sw > 0 && sh > 0) {
            glBindTexture(GL_TEXTURE_2D, scanTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            m_scanTexW = sw; m_scanTexH = sh; m_scanFromFile = true;
            stbi_image_free(px);
            LOG_INFO("[CRT] scanline texture '%s' loaded (%dx%d)", scanFile.c_str(), sw, sh);
        } else {
            LOG_INFO("[CRT] scanline texture '%s' not found; using procedural scanlines", scanFile.c_str());
        }
    }

    glUseProgram(0);
    glClearColor(0.f, 0.f, 0.f, 1.f);
    return true;
}

void PetGL::setCrtEnabled(bool on) { m_crt = on; }
bool PetGL::getCrtEnabled() const  { return m_crt; }

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
    if (scanTex) glDeleteTextures(1, &scanTex);
    if (vbo) glDeleteBuffers(1, &vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
    if (prog) glDeleteProgram(prog);
    if (scanProg) glDeleteProgram(scanProg);
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
}

void PetGL::draw()
{
    // The host has already set glViewport() to the centered, aspect-fit rect.
    // Clear the whole window (letterbox bars stay black), then draw the quad
    // into the active viewport.
    glClear(GL_COLOR_BUFFER_BIT);
    if (texW <= 0 || texH <= 0) return;

    // ---- Main pass: PET frame (optionally phosphor-tinted) ----
    glUseProgram(prog);
    glUniform1i(uTintOnLoc, (m_crt && m_tintOn) ? 1 : 0);
    glUniform3f(uTintLoc, m_tint[0], m_tint[1], m_tint[2]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);

    // ---- Scanline pass (DISABLED): the PET had a mono green-phosphor screen with
    //      no shadow mask, so CRT mode is just the green tint for now. This
    //      procedural horizontal-scanline pass is kept intact but gated off
    //      (m_scanlines = false) as a starting point for a proper VICE-style CRT
    //      shader later. Flip m_scanlines to re-enable.
    if (m_crt && m_scanlines) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_ZERO);   // multiply
        glUseProgram(scanProg);
        glUniform2f(uScanSrcSizeLoc, (float)texW, (float)texH);
        glUniform1f(uScanPitchLoc,   m_grillePitch);
        glUniform1f(uScanThickLoc,   m_grilleThickness);
        glUniform1f(uScanDimLoc,     m_scanStrength);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glDisable(GL_BLEND);
    }

    glBindVertexArray(0);
    glUseProgram(0);
}

void PetGL::present(const uint32_t* rgba, int srcW, int srcH)
{
    updateTexture(rgba, srcW, srcH);
    draw();
}
