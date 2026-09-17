#include "app/Artwork.h"
#include "app/Globals.h"
#include "app/Input.h"
#include "gfx/ResourceManager.h"
#include "platform/Platform.h"
#include <algorithm>
#include <cstdio>

namespace artwork {

int index = 0;
int uiTimer = 0;
int uiVisible = 1;

namespace {
int posX = 40;                 // 0x71001b29f8 (init 40)
int posY = 0;                  // 0x712aba3580
float width = 1200.0f;         // 0x71001b2a00 (init 1200)
float height = 1293.0f;        // 0x71001b2a04 (init 1293)
float scroll = 0.0f;           // 0x712aba3590 (+0.2 up to 640, unused)
Vec4 uiFade{0, 0, 0, 0};       // 0x712aba3594..35a0
Vec4 uiColor{0, 0, 0, 0};      // 0x712aba35b0..35bc
constexpr Vec4 kImageColor{1.0f, 1.0f, 1.0f, 1.0f};  // vec3 @ 0x7100194e80 + alpha 1

std::string CurrentName() {
    char name[32];
    std::snprintf(name, sizeof(name), "artwork%d", index);
    return name;
}
}  // namespace

void LoadTextures() {
    index = 0;
    for (int i = 0; i < 16; ++i) {
        char path[64];
        std::snprintf(path, sizeof(path), "rom:/assets/artwork/2_BonusArtwork_P%02d.png", i + 1);
        ResourceManager::LoadTexture(path, true, "artwork" + std::to_string(i));
    }
    ResourceManager::LoadTexture("rom:/assets/artwork/artworkUI-Small.png", true, "artworkUI");
    uiTimer = 0;
}

void Select() {
    Texture2D& tex = ResourceManager::GetTexture(CurrentName());
    const float w = static_cast<float>(tex.Width);
    const float h = static_cast<float>(tex.Height);
    width = (1280.0f / w) * w;
    height = (1280.0f / w) * h;
    posX = static_cast<int>(640.0f - width * 0.5f);
    posY = static_cast<int>(360.0f - height * 0.5f);
}

void Draw() {
    Texture2D& tex = ResourceManager::GetTexture(CurrentName());
    ResourceManager::GetTexture("artworkUI");
    ResourceManager::GetTexture("transparent");

    const Input& in = *g::input;
    float s = scroll + 0.2f;
    scroll = (s <= 640.0f) ? s : 0.0f;

    // zoom with the right stick X (stick byte / 8), clamp width to [600, 2200]
    int zoomStep = static_cast<int>(static_cast<int8_t>(in.sticks[2])) / 8;
    float w = width + static_cast<float>(zoomStep);
    w = std::max(w, 600.0f);
    width = std::min(w, 2200.0f);
    // pan with the left stick (byte / 12); Y is inverted
    posX += static_cast<int>(static_cast<int8_t>(in.sticks[0])) / 12;
    posY += -static_cast<int>(static_cast<int8_t>(in.sticks[1])) / 12;
    height = static_cast<float>((static_cast<double>(width) / static_cast<double>(tex.Width)) * static_cast<double>(tex.Height));

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    g::spriteRenderer->DrawSprite(tex, {static_cast<float>(posX), static_cast<float>(posY)}, {width, height}, 0.0f, kImageColor);

    Vec4 ui = uiColor;
    if (uiVisible == 0) {
        if (uiTimer > 4) {
            uiFade.r = std::max(uiFade.r - 0.2f, 0.0f);
            uiFade.g = std::max(uiFade.g - 0.2f, 0.0f);
            uiFade.b = std::max(uiFade.b - 0.2f, 0.0f);
            uiFade.a = std::max(uiFade.a - 0.2f, 0.0f);
            uiColor = uiFade;
            ui = uiFade;
        }
    } else if (uiTimer < 10) {
        uiFade.r = std::min(uiFade.r + 0.1f, 1.0f);
        uiFade.g = std::min(uiFade.g + 0.1f, 1.0f);
        uiFade.b = std::min(uiFade.b + 0.1f, 1.0f);
        uiFade.a = std::min(uiFade.a + 0.1f, 1.0f);
        uiColor = uiFade;
        ui = uiFade;
    }
    g::spriteRenderer->DrawSprite(ResourceManager::GetTexture("artworkUI"), {50.0f, 487.0f}, {245.0f, 233.0f}, 0.0f, ui);

    glFinish();
    Platform::SwapBuffers();
    ++uiTimer;
}

}  // namespace artwork
