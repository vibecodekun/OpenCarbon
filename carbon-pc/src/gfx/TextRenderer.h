// TextRenderer — GPU text batcher used by Carbon's menus.
// Fonts: rom:/assets/fonts/<name>.tga (uncompressed 8/24/32-bit atlas) + <name>.bin (raw FontFile, 0x3820 bytes).
// Glyph quads are pushed to a texture buffer and expanded in the vertex shader from gl_VertexID.
// Original: ctor @ 0x71000ccad0, dtor @ 0x71000ccb30, LoadTga @ 0x71000cc830, Init @ 0x71000ccb80,
// LoadFont @ 0x71000cd180, pushQuad @ 0x71000cd3b0, ResetCursor @ 0x71000cd4c0, Flush @ 0x71000cd4d0,
// Print(rgba) @ 0x71000cd780, Print(float colour) @ 0x71000cd800.
#pragma once
#include "platform/gl.h"
#include <cstdint>
#include <vector>

struct FontFile {  // layout of the .bin file (little-endian)
    struct GlyphPix { int32_t u, v, width, height, advance, offX, offY; };
    struct GlyphNorm { float u, v, width, height, advance, offX, offY; };
    struct Glyph { GlyphPix pix; GlyphNorm norm; };
    int32_t texwidth, texheight;
    int32_t ascent, descent, linegap;
    float normAscent, normDescent, normLinegap;
    Glyph glyphs[256];
};

class TextRenderer {
public:
    TextRenderer() = default;
    ~TextRenderer();

    // @ 0x71000cd180. Returns false only if the .tga could not be loaded.
    bool LoadFont(const char* name, int canvasWidth, int canvasHeight);

    // @ 0x71000cd780: colour packed as 0xRRGGBBAA. `line` > 1 moves the start down by (line-1) lines.
    float Print(int x, int y, const char* text, int line, uint32_t rgba);
    // @ 0x71000cd800
    float Print(int x, int y, const char* text, int line, const float color[4]);

    void Flush();                         // @ 0x71000cd4d0
    void ResetCursor() { field_2c = 0; }  // @ 0x71000cd4c0

private:
    struct Quad { float x, y, w, h; float glyph, pad0, pad1, pad2; };  // two RGBA32F texels

    bool Init(int canvasWidth, int canvasHeight);  // @ 0x71000ccb80
    void pushQuad(const Quad& q) { quads.push_back(q); }

    GLuint program = 0, vertexShader = 0, fragmentShader = 0;
    GLint canvasLoc = -1, colorLoc = -1, glyphTexOffsetLoc = -1, quadsLoc = -1;
    GLuint fontTexture = 0;
    float field_28 = 1.0f;
    int field_2c = 0;
    GLuint quadsBuffer = 0, vao = 0;
    int quadCount = 0;
    GLuint glyphOffsetBuffer = 0, glyphOffsetTexture = 0, quadsTexture = 0;
    uint32_t canvasW = 0, canvasH = 0, canvasScale = 1;
    std::vector<Quad> quads;
    bool ownsFontData = false;

    // The original keeps ONE global font table (0x712aba91f8): loading a second font replaces the
    // metrics used by every TextRenderer. Kept as-is for fidelity.
    static FontFile* s_font;
};
