#include "gfx/SpriteRenderer.h"
#include <cmath>

SpriteRenderer::SpriteRenderer(Shader& s) : shader(s) {
    // @ 0x71000e9540 — identical quad to the tutorial: pos.xy + texcoord.xy per vertex
    GLuint VBO;
    const GLfloat vertices[] = {
        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f,

        0.0f, 1.0f, 0.0f, 1.0f,
        1.0f, 1.0f, 1.0f, 1.0f,
        1.0f, 0.0f, 1.0f, 0.0f,
    };
    glGenVertexArrays(1, &quadVAO);
    glGenBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glBindVertexArray(quadVAO);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), nullptr);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
}

void SpriteRenderer::DrawSprite(Texture2D& texture, Vec2 position, Vec2 size, float rotate, Vec4 color) {
    shader.Use();

    // model = translate(position) * rotateZ(rotate) * scale(size)   (column-major)
    const float c = std::cos(rotate), s = std::sin(rotate);
    Mat4 model;
    model.m[0] = c * size.x;  model.m[1] = s * size.x;
    model.m[4] = -s * size.y; model.m[5] = c * size.y;
    model.m[10] = 1.0f;
    model.m[12] = position.x; model.m[13] = position.y; model.m[14] = 0.0f; model.m[15] = 1.0f;

    shader.SetMatrix4("model", model);
    shader.SetVector4f("spriteColor", color.r, color.g, color.b, color.a);

    glActiveTexture(GL_TEXTURE0);
    texture.Bind();

    glBindVertexArray(quadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    // original leaves the VAO bound
}
