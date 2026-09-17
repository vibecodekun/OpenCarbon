#include "gfx/ResourceManager.h"
#include "platform/Paths.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include "stb_image.h"

std::map<std::string, Shader> ResourceManager::Shaders;
std::map<std::string, Texture2D> ResourceManager::Textures;

Shader ResourceManager::LoadShader(const char* vShaderFile, const char* fShaderFile, const char* gShaderFile, const std::string& name) {
    Shaders[name] = loadShaderFromFile(vShaderFile, fShaderFile, gShaderFile);
    return Shaders[name];
}

Shader& ResourceManager::GetShader(const std::string& name) {
    return Shaders[name];
}

Texture2D ResourceManager::LoadTexture(const char* file, bool alpha, const std::string& name) {
    Textures[name] = loadTextureFromFile(file, alpha);
    return Textures[name];
}

Texture2D& ResourceManager::GetTexture(const std::string& name) {
    return Textures[name];
}

Texture2D ResourceManager::CreateEmptyTexture(int width, int height, int channels, const std::string& name) {
    Texture2D texture;
    unsigned char* data = static_cast<unsigned char*>(std::malloc(static_cast<size_t>(width) * height * channels));
    texture.Internal_Format = GL_RGBA;  // original hard-codes RGBA regardless of `channels`
    texture.Image_Format = GL_RGBA;
    texture.Generate(width, height, data);
    std::free(data);
    Textures[name] = texture;
    return Textures[name];
}

Shader ResourceManager::loadShaderFromFile(const char* vShaderFile, const char* fShaderFile, const char* gShaderFile) {
    std::string vertexCode, fragmentCode, geometryCode;
    std::ifstream vertexShaderFile(Paths::Resolve(vShaderFile));
    std::ifstream fragmentShaderFile(Paths::Resolve(fShaderFile));
    std::stringstream vShaderStream, fShaderStream;
    vShaderStream << vertexShaderFile.rdbuf();
    fShaderStream << fragmentShaderFile.rdbuf();
    vertexCode = vShaderStream.str();
    fragmentCode = fShaderStream.str();
    if (gShaderFile != nullptr) {
        std::ifstream geometryShaderFile(Paths::Resolve(gShaderFile));
        std::stringstream gShaderStream;
        gShaderStream << geometryShaderFile.rdbuf();
        geometryCode = gShaderStream.str();
    }
    // (tutorial's "ERROR::SHADER: Failed to read shader files" path exists in the binary as well)
    Shader shader;
    shader.Compile(vertexCode.c_str(), fragmentCode.c_str(), gShaderFile != nullptr ? geometryCode.c_str() : nullptr);
    return shader;
}

Texture2D ResourceManager::loadTextureFromFile(const char* file, bool alpha) {
    Texture2D texture;
    if (alpha) {
        texture.Internal_Format = GL_RGBA;
        texture.Image_Format = GL_RGBA;
    }
    // Zero-initialised here: the original leaves these uninitialised, so a missing file (e.g. the shipped
    // gamedef's ingameTopMenu.png) uploads a texture of whatever size was on the stack.
    int width = 0, height = 0, nrChannels = 0;
    // original passes req_comp = 4 unconditionally
    unsigned char* data = stbi_load(Paths::Resolve(file).c_str(), &width, &height, &nrChannels, 4);
    texture.Generate(width, height, data);
    stbi_image_free(data);
    return texture;
}
