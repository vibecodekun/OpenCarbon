// ResourceManager — LearnOpenGL "Breakout" resource manager plus Carbon's CreateEmptyTexture.
// Original: statics init @ 0x71000e8d10, LoadShader @ 0x71000cf620 (+loadShaderFromFile @ 0x71000cf6a0),
// GetShader @ 0x71000d01e0, LoadTexture @ 0x71000d0220, GetTexture @ 0x71000d0330,
// CreateEmptyTexture @ 0x71000d0530. Paths use the "rom:/" mount, mapped by platform/Paths.
#pragma once
#include "gfx/Shader.h"
#include "gfx/Texture2D.h"
#include <map>
#include <string>

class ResourceManager {
public:
    static std::map<std::string, Shader> Shaders;
    static std::map<std::string, Texture2D> Textures;

    static Shader LoadShader(const char* vShaderFile, const char* fShaderFile, const char* gShaderFile, const std::string& name);
    static Shader& GetShader(const std::string& name);  // operator[] semantics: unknown names insert an empty Shader
    static Texture2D LoadTexture(const char* file, bool alpha, const std::string& name);
    static Texture2D& GetTexture(const std::string& name);

    // Carbon addition: RGBA texture of w*h uploaded from an *uninitialised* malloc buffer.
    static Texture2D CreateEmptyTexture(int width, int height, int channels, const std::string& name);

private:
    static Shader loadShaderFromFile(const char* vShaderFile, const char* fShaderFile, const char* gShaderFile);
    static Texture2D loadTextureFromFile(const char* file, bool alpha);
};
