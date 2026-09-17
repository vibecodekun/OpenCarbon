// Texture2D — LearnOpenGL "Breakout" texture wrapper as used by Carbon.
// Original: ctor @ 0x71000eb920, Generate @ 0x71000eb960, Generate(filter) @ 0x71000eba50, Bind @ 0x71000ebb70.
#pragma once
#include "platform/gl.h"

class Texture2D {
public:
    GLuint ID = 0;
    GLuint Width = 0, Height = 0;
    GLuint Internal_Format = GL_RGB;
    GLuint Image_Format = GL_RGB;
    GLuint Wrap_S = GL_REPEAT;
    GLuint Wrap_T = GL_REPEAT;
    GLuint Filter_Min = GL_LINEAR;
    GLuint Filter_Max = GL_LINEAR;

    Texture2D();  // calls glGenTextures, like the original

    // @ 0x71000eb960: upload with the stored wrap/filter settings.
    void Generate(GLuint width, GLuint height, const unsigned char* data);

    // @ 0x71000eba50: Carbon addition. Re-uploads (full glTexImage2D) and picks NEAREST (linear=false)
    // or LINEAR. The emulator presenters call this every frame with the framebuffer.
    void Generate(GLuint width, GLuint height, const unsigned char* data, bool linear);

    void Bind() const;
};
