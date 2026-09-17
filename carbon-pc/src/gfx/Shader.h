// Shader — LearnOpenGL "Breakout" shader wrapper as used by Carbon.
// Original: Use @ 0x71000e8d70, Compile @ 0x71000e8da0, checkCompileErrors @ 0x71000e9090,
// SetFloat @ 0x71000e9320, SetInteger @ 0x71000e93a0, SetVector4f @ 0x71000e9420, SetMatrix4 @ 0x71000e94b0.
#pragma once
#include "platform/gl.h"
#include <string>

struct Mat4 {
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // column-major, like glm
    static Mat4 Ortho(float left, float right, float bottom, float top, float zNear, float zFar);
};

class Shader {
public:
    GLuint ID = 0;

    Shader& Use();
    void Compile(const char* vertexSource, const char* fragmentSource, const char* geometrySource = nullptr);

    void SetFloat(const char* name, float value, bool useShader = false);
    void SetInteger(const char* name, int value, bool useShader = false);
    void SetVector4f(const char* name, float x, float y, float z, float w, bool useShader = false);
    void SetMatrix4(const char* name, const Mat4& matrix, bool useShader = false);

private:
    // Prints "| ERROR::SHADER: Compile-time error: Type: ..." / "| ERROR::Shader: Link-time error: ..."
    void checkCompileErrors(GLuint object, const std::string& type);
};
