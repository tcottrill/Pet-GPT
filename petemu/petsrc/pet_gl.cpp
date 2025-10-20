#include "pet_gl.h"
#include "framework.h"
#include "rawinput.h"   // RawInput callback registration
#include "sys_log.h"

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
void main(){
    // Nearest-neighbor sampling (texture is configured with NEAREST anyway)
    fragColor = texture(uTex, vUV);
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
    glUniform1i(uTexLoc, 0);

    // Texture setup (no data yet)
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    texW = texH = 0;

    glClearColor(0.f, 0.f, 0.f, 1.f);
    return true;
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
    // Letterbox to maintain aspect (nearest scaling handled by texture filtering)
    int winW = SCREEN_W, winH = SCREEN_H;

    const float srcW = float(texW);
    const float srcH = float(texH);
    if (srcW <= 0.f || srcH <= 0.f)
    {
        glViewport(0, 0, winW, winH);
        glClear(GL_COLOR_BUFFER_BIT);
        return;
    }

    const float winAspect = float(winW) / float(winH);
    const float srcAspect = srcW / srcH;

    int vpW, vpH, vpX, vpY;
    if (winAspect >= srcAspect) {
        // window wider than src => pillarbox
        vpH = winH;
        vpW = int(std::round(srcAspect * vpH));
        vpX = (winW - vpW) / 2;
        vpY = 0;
    }
    else {
        // window taller than src => letterbox
        vpW = winW;
        vpH = int(std::round(vpW / srcAspect));
        vpX = 0;
        vpY = (winH - vpH) / 2;
    }

    glViewport(vpX, vpY, vpW, vpH);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(prog);
    if (uDstSizeLoc >= 0) glUniform2f(uDstSizeLoc, float(vpW), float(vpH));
    if (uSrcSizeLoc >= 0) glUniform2f(uSrcSizeLoc, float(texW), float(texH));

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);

    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    glBindVertexArray(0);
}

void PetGL::present(const uint32_t* rgba, int srcW, int srcH)
{
    updateTexture(rgba, srcW, srcH);
    draw();
}
