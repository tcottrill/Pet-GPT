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
};

#endif // PET_GL_H
