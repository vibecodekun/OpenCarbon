// OpenGL entry point for the whole project. The Windows build uses a generated GL 4.5 core loader.
#pragma once
#include "glad/gl.h"

// Compatibility-profile enum still referenced by TextRenderer's 8-bit TGA path (not in the core loader).
#ifndef GL_LUMINANCE
#define GL_LUMINANCE 0x1909
#endif
