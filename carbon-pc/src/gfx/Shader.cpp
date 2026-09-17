#include "gfx/Shader.h"
#include <cstdio>

Mat4 Mat4::Ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
    Mat4 r;
    r.m[0] = 2.0f / (right - left);
    r.m[5] = 2.0f / (top - bottom);
    r.m[10] = -2.0f / (zFar - zNear);
    r.m[12] = -(right + left) / (right - left);
    r.m[13] = -(top + bottom) / (top - bottom);
    r.m[14] = -(zFar + zNear) / (zFar - zNear);
    return r;
}

Shader& Shader::Use() {
    glUseProgram(ID);
    return *this;
}

void Shader::Compile(const char* vertexSource, const char* fragmentSource, const char* geometrySource) {
    GLuint sVertex = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(sVertex, 1, &vertexSource, nullptr);
    glCompileShader(sVertex);
    checkCompileErrors(sVertex, "VERTEX");

    GLuint sFragment = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(sFragment, 1, &fragmentSource, nullptr);
    glCompileShader(sFragment);
    checkCompileErrors(sFragment, "FRAGMENT");

    GLuint gShader = 0;
    if (geometrySource != nullptr) {
        gShader = glCreateShader(GL_GEOMETRY_SHADER);
        glShaderSource(gShader, 1, &geometrySource, nullptr);
        glCompileShader(gShader);
        checkCompileErrors(gShader, "GEOMETRY");
    }

    ID = glCreateProgram();
    glAttachShader(ID, sVertex);
    glAttachShader(ID, sFragment);
    if (geometrySource != nullptr)
        glAttachShader(ID, gShader);
    glLinkProgram(ID);
    checkCompileErrors(ID, "PROGRAM");

    glDeleteShader(sVertex);
    glDeleteShader(sFragment);
    if (geometrySource != nullptr)
        glDeleteShader(gShader);
}

void Shader::SetFloat(const char* name, float value, bool useShader) {
    if (useShader) Use();
    glUniform1f(glGetUniformLocation(ID, name), value);
}

void Shader::SetInteger(const char* name, int value, bool useShader) {
    if (useShader) Use();
    glUniform1i(glGetUniformLocation(ID, name), value);
}

void Shader::SetVector4f(const char* name, float x, float y, float z, float w, bool useShader) {
    if (useShader) Use();
    glUniform4f(glGetUniformLocation(ID, name), x, y, z, w);
}

void Shader::SetMatrix4(const char* name, const Mat4& matrix, bool useShader) {
    if (useShader) Use();
    glUniformMatrix4fv(glGetUniformLocation(ID, name), 1, GL_FALSE, matrix.m);
}

void Shader::checkCompileErrors(GLuint object, const std::string& type) {
    GLint success;
    GLchar infoLog[1024];
    if (type != "PROGRAM") {
        glGetShaderiv(object, GL_COMPILE_STATUS, &success);
        if (!success) {
            glGetShaderInfoLog(object, 1024, nullptr, infoLog);
            std::printf("| ERROR::SHADER: Compile-time error: Type: %s\n%s\n -- --------------------------------------------------- -- \n",
                        type.c_str(), infoLog);
        }
    } else {
        glGetProgramiv(object, GL_LINK_STATUS, &success);
        if (!success) {
            glGetProgramInfoLog(object, 1024, nullptr, infoLog);
            std::printf("| ERROR::Shader: Link-time error: Type: %s\n%s\n -- --------------------------------------------------- -- \n",
                        type.c_str(), infoLog);
        }
    }
}
