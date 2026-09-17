#include "gfx/TextRenderer.h"
#include "platform/Paths.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

FontFile* TextRenderer::s_font = nullptr;

namespace {

const char* kTextVS =
#include "gfx/text_vs.glsl.inc"
    ;
const char* kTextFS =
#include "gfx/text_fs.glsl.inc"
    ;

struct TgaImage {
    GLenum format = static_cast<GLenum>(-1);
    int width = 0, height = 0, bpp = 0;
    unsigned char* data = nullptr;
};

// @ 0x71000cc830 — minimal uncompressed TGA reader. Ignores the ID-length field and image origin.
// Returns 1 on success, 2 if the file can't be opened (other failures also return 1 with data == nullptr).
int LoadTga(TgaImage& img, const char* path) {
    FILE* f = std::fopen(Paths::Resolve(path).c_str(), "rb");
    if (!f) return 2;
    unsigned char head3[3];
    std::fread(head3, 1, 3, f);  // id length, colour map type, image type
    std::fseek(f, 12, SEEK_SET);
    unsigned char dims[6];
    std::fread(dims, 1, 6, f);
    img.width = dims[0] | (dims[1] << 8);
    img.height = dims[2] | (dims[3] << 8);
    img.bpp = dims[4];
    // The original fclose()s here when the colour-map/type check fails and then keeps reading from the
    // closed FILE*. We just bail out instead.
    if (head3[1] != 0 || (head3[2] & 0xFE) != 2 || (img.bpp != 8 && img.bpp != 24 && img.bpp != 32)) {
        std::fclose(f);
        return 1;
    }
    size_t pixels = static_cast<size_t>(img.width) * img.height;
    size_t bytes = pixels * (img.bpp / 8);
    unsigned char* data = static_cast<unsigned char*>(std::malloc(bytes));
    if (data && std::fread(data, 1, bytes, f) == bytes) {
        if (img.bpp == 24) {
            for (size_t i = 0; i < bytes; i += 3) std::swap(data[i], data[i + 2]);
            img.format = GL_RGB;
        } else if (img.bpp == 32) {
            for (size_t i = 0; i < bytes; i += 4) std::swap(data[i], data[i + 2]);
            img.format = GL_RGBA;
        } else {
            img.format = GL_ALPHA;
        }
        img.data = data;
    } else {
        std::free(data);
        img.data = nullptr;
    }
    std::fclose(f);
    return 1;
}

GLuint CompileStage(GLenum type, const char* src, const char* failMsg) {
    GLuint sh = glCreateShader(type);
    if (sh == 0) {
        std::fputs(failMsg, stderr);
        return 0;
    }
    glShaderSource(sh, 1, &src, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[256] = {};
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        std::fprintf(stderr, "Compile failed:\n%s\n", log);
        glDeleteShader(sh);
        std::fputs(failMsg, stderr);
        return 0;
    }
    return sh;
}

}  // namespace

TextRenderer::~TextRenderer() {
    // @ 0x71000ccb30
    if (s_font && ownsFontData) {
        delete s_font;
        s_font = nullptr;
    }
}

bool TextRenderer::LoadFont(const char* name, int canvasWidth, int canvasHeight) {
    if (name != nullptr) {
        char path[200];
        std::snprintf(path, sizeof(path), "rom:/assets/fonts/%s.tga", name);
        TgaImage img;
        if (LoadTga(img, path) != 1) return false;

        std::snprintf(path, sizeof(path), "rom:/assets/fonts/%s.bin", name);
        if (FILE* f = std::fopen(Paths::Resolve(path).c_str(), "rb")) {
            if (s_font && ownsFontData) delete s_font;
            s_font = new FontFile;
            ownsFontData = true;
            std::fread(s_font, 1, sizeof(FontFile), f);
            std::fclose(f);

            if (fontTexture == 0) glGenTextures(1, &fontTexture);
            GLenum fmt = (img.format == GL_ALPHA) ? GL_LUMINANCE : (img.format == GL_RGBA ? GL_RGBA : GL_RGB);
            // original: glTextureImage2DEXT (EXT_direct_state_access)
            glBindTexture(GL_TEXTURE_2D, fontTexture);
            glTexImage2D(GL_TEXTURE_2D, 0, fmt, img.width, img.height, 0, fmt, GL_UNSIGNED_BYTE, img.data);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        std::free(img.data);
    }
    Init(canvasWidth, canvasHeight);
    return true;
}

bool TextRenderer::Init(int canvasWidth, int canvasHeight) {
    canvasW = canvasWidth;
    canvasH = canvasHeight;
    canvasScale = 1;
    if (program != 0) return true;

    vertexShader = CompileStage(GL_VERTEX_SHADER, kTextVS, "Vertex shader compile failed\n");
    fragmentShader = CompileStage(GL_FRAGMENT_SHADER, kTextFS, "Fragment shader compile failed\n");

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vertexShader);
    glAttachShader(prog, fragmentShader);
    glLinkProgram(prog);
    GLint logLen = 0;
    glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
    if (logLen > 0) {
        std::vector<char> log(logLen);
        glGetProgramInfoLog(prog, logLen, nullptr, log.data());
        if (log[0] != '\0') std::fprintf(stderr, "Link failed:\n%s\n", log.data());
    }
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        glDeleteProgram(prog);
        prog = 0;
    }
    program = prog;

    GLint fontTexLoc = glGetUniformLocation(program, "fontTex");
    canvasLoc = glGetUniformLocation(program, "canvas");
    colorLoc = glGetUniformLocation(program, "color");

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glUseProgram(program);
    glUniform1i(fontTexLoc, 3);

    // glyph atlas rectangles (normalised u, v, w, h) for all 256 glyphs
    glGenBuffers(1, &quadsBuffer);
    std::vector<float> offsets(256 * 4, 0.0f);
    if (s_font) {
        for (int i = 0; i < 256; ++i) {
            offsets[i * 4 + 0] = s_font->glyphs[i].norm.u;
            offsets[i * 4 + 1] = s_font->glyphs[i].norm.v;
            offsets[i * 4 + 2] = s_font->glyphs[i].norm.width;
            offsets[i * 4 + 3] = s_font->glyphs[i].norm.height;
        }
    }
    glyphTexOffsetLoc = glGetUniformLocation(program, "glyphTexOffset");
    glUniform1i(glyphTexOffsetLoc, 2);
    glGenBuffers(1, &glyphOffsetBuffer);
    glGenTextures(1, &glyphOffsetTexture);
    glBindBuffer(GL_TEXTURE_BUFFER, glyphOffsetBuffer);
    glBufferData(GL_TEXTURE_BUFFER, 0x1000, offsets.data(), GL_STATIC_DRAW);

    quadsLoc = glGetUniformLocation(program, "quads");
    glGenTextures(1, &quadsTexture);
    glUniform1i(quadsLoc, 1);
    glUseProgram(0);
    return true;
}

float TextRenderer::Print(int x, int y, const char* text, int line, uint32_t rgba) {
    const float color[4] = {
        static_cast<float>(((rgba >> 24) & 0xFF) * (1.0 / 255.0)),
        static_cast<float>(((rgba >> 16) & 0xFF) * (1.0 / 255.0)),
        static_cast<float>(((rgba >> 8) & 0xFF) * (1.0 / 255.0)),
        static_cast<float>((rgba & 0xFF) * (1.0 / 255.0)),
    };
    return Print(x, y, text, line, color);
}

float TextRenderer::Print(int x, int y, const char* text, int line, const float color[4]) {
    if (s_font == nullptr) return 0.0f;

    const int lineHeight = s_font->ascent + s_font->descent + s_font->linegap;
    float penY = static_cast<float>(y);
    if (line > 1) penY += static_cast<float>(lineHeight * (line - 1));

    glUseProgram(program);
    glUniform4fv(colorLoc, 1, color);  // original: glProgramUniform4fv
    glUseProgram(0);

    float totalHeight = 0.0f;
    field_28 = 1.0f;
    float penX = static_cast<float>(x + 1);
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '\n') {
            penY -= static_cast<float>(lineHeight);
            totalHeight += static_cast<float>(lineHeight);
            penX = static_cast<float>(x + 1);
            continue;
        }
        if (c < 0) continue;  // chars >= 0x80 are skipped
        const FontFile::GlyphPix& g = s_font->glyphs[static_cast<unsigned char>(c)].pix;
        Quad q{};
        q.x = static_cast<float>(static_cast<int>(penX + static_cast<float>(g.offX)));
        q.y = static_cast<float>(static_cast<int>((penY - static_cast<float>(g.height)) - static_cast<float>(g.offY)));
        q.w = static_cast<float>(g.width);
        q.h = static_cast<float>(g.height);
        q.glyph = static_cast<float>(static_cast<int>(c));
        pushQuad(q);
        penX += static_cast<float>(g.advance);
    }
    field_28 = 1.0f;
    return totalHeight;
}

void TextRenderer::Flush() {
    if (quads.empty()) return;

    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glStencilMask(0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glPrimitiveRestartIndex(0xFFFFFFFF);
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);

    glBindBuffer(GL_TEXTURE_BUFFER, quadsBuffer);
    glBufferData(GL_TEXTURE_BUFFER, quads.size() * sizeof(Quad), quads.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_TEXTURE_BUFFER, 0);
    quadCount = static_cast<int>(quads.size());

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_BUFFER, quadsTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, quadsBuffer);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_BUFFER, glyphOffsetTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA32F, glyphOffsetBuffer);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, fontTexture);

    glUseProgram(program);
    glUniform4f(canvasLoc, static_cast<float>(canvasW), static_cast<float>(canvasH), static_cast<float>(canvasScale), 0.0f);
    glGetError();
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, quadCount * 6);
    glUseProgram(0);
    glGetError();

    quads.clear();
    glActiveTexture(GL_TEXTURE0);  // (not in the original; keeps SpriteRenderer's unit-0 assumption safe)
}
