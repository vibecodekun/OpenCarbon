// SpriteRenderer — adapted LearnOpenGL "Breakout" sprite renderer.
// Original: initRenderData @ 0x71000e9540, DrawSprite @ 0x71000e9680.
// Differences from the tutorial: vec4 colour, and the model matrix is T(pos)*Rz(rot)*S(size)
// (rotation about the top-left corner, not the centre).
#pragma once
#include "gfx/Shader.h"
#include "gfx/Texture2D.h"

struct Vec2 { float x, y; };
struct Vec4 { float r, g, b, a; };

class SpriteRenderer {
public:
    explicit SpriteRenderer(Shader& shader);
    void DrawSprite(Texture2D& texture, Vec2 position, Vec2 size, float rotate, Vec4 color);
    Shader& GetShader() { return shader; }

private:
    Shader shader;
    GLuint quadVAO = 0;
};
