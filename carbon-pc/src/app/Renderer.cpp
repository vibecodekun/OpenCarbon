#include "app/Renderer.h"
#include "app/EmuGlue.h"
#include "app/GameDef.h"
#include "app/Globals.h"
#include "gfx/ResourceManager.h"
#include "platform/Platform.h"
#include <algorithm>
#include <string>

namespace {

constexpr Vec4 kWhite{1.0f, 1.0f, 1.0f, 1.0f};  // DAT_7100194d50

// shared sprite sizes (exact float constants from the binary)
constexpr Vec2 kFullScreen{1280.0f, 720.0f};
constexpr Vec2 kLogoPos{30.0f, 30.0f}, kLogoSize{367.16f, 190.95f};
constexpr Vec2 kBButtonPos{30.0f, 660.0f}, kBButtonSize{102.4f, 72.0f};
constexpr Vec2 kDescBarPos{-20.0f, 620.0f}, kDescBarSize{732.31f, 53.6f};
constexpr Vec2 kMenuBarSize{298.15f, 27.47f};
constexpr Vec2 kSelectIconSize{59.63f, 62.98f};
constexpr uint32_t kUnselectedText = 0xF9F9BCFF;

void DrawSprite(SpriteRenderer* r, const char* texture, Vec2 pos, Vec2 size, Vec4 color) {
    r->DrawSprite(ResourceManager::GetTexture(texture), pos, size, 0.0f, color);
}

float MenuSpriteWidth() { return static_cast<float>(g::gameDef->menuSpriteWidth); }
float MenuSpriteHeight() { return static_cast<float>(g::gameDef->menuSpriteHeight); }

}  // namespace

bool Renderer::Init() {
    menuSelection = 0;
    Platform::InitGraphics();  // Graphics_InitEGL @ 0x710009b540 (+ wf_logo.mp4) and the GL loader init

    ResourceManager::LoadShader("rom:/assets/shaders/sprite.vs", "rom:/assets/shaders/sprite.fs", nullptr, "sprite");
    ResourceManager::LoadShader("rom:/assets/shaders/sprite.vs", "rom:/assets/shaders/particles.fs", nullptr, "emulation");
    ResourceManager::LoadShader("rom:/assets/shaders/sprite.vs", "rom:/assets/shaders/lcd.fs", nullptr, "lcd");
    ResourceManager::LoadShader("rom:/assets/shaders/sprite.vs", "rom:/assets/shaders/fade.fs", nullptr, "transition");

    // glm::ortho(0, 1280, 720, 0, -1, 1) built with zero-to-one depth (m10 = -0.5, m14 = 0.5)
    Mat4 projection = Mat4::Ortho(0.0f, 1280.0f, 720.0f, 0.0f, -1.0f, 1.0f);
    projection.m[10] = -0.5f;
    projection.m[14] = 0.5f;
    for (const char* name : {"sprite", "emulation", "lcd", "transition"}) {
        ResourceManager::GetShader(name).Use().SetInteger("image", 0);  // shaders use "scene"; harmless no-op
        ResourceManager::GetShader(name).SetMatrix4("projection", projection);
    }

    const GameDef& gd = *g::gameDef;
    ResourceManager::LoadTexture(gd.menuBackground.c_str(), true, "MenuBackground");
    ResourceManager::LoadTexture(gd.menuSprite.c_str(), true, "MenuSprite");
    ResourceManager::LoadTexture(gd.gameLogo.c_str(), true, "GameLogo");
    ResourceManager::LoadTexture(gd.mainMenuBar.c_str(), true, "MenuBar");
    ResourceManager::LoadTexture(gd.sidePauseMenu.c_str(), true, "PauseMenu");
    ResourceManager::LoadTexture(gd.topPauseMenu.c_str(), true, "PauseTopMenu");
    ResourceManager::LoadTexture(gd.inGameMenuBar.c_str(), true, "PauseMenuBar");
    ResourceManager::LoadTexture(gd.inGameBackground.c_str(), true, "GameBackground");
    ResourceManager::LoadTexture(gd.saveStateBox.c_str(), true, "SaveStateBox");
    ResourceManager::LoadTexture(gd.dialogBox.c_str(), true, "DialogBox");
    ResourceManager::LoadTexture(gd.pauseMenuImage.c_str(), true, "PauseMenuText");
    ResourceManager::LoadTexture(gd.controlImage.c_str(), true, "ControllerImage");
    ResourceManager::LoadTexture(gd.buttonA.c_str(), true, "A_Button");
    ResourceManager::LoadTexture(gd.buttonB.c_str(), true, "B_Button");
    ResourceManager::LoadTexture(gd.selectIcon.c_str(), true, "SelectIcon");
    ResourceManager::LoadTexture(gd.descriptionBar.c_str(), true, "DescriptionBar");
    ResourceManager::LoadTexture("rom:/assets/borderSmall.png", true, "GameBorder");
    for (const GameEntry& e : gd.titles)
        ResourceManager::LoadTexture(e.previewImage.c_str(), true, "MenuPreview" + std::to_string(static_cast<uint8_t>(e.id)));
    ResourceManager::LoadTexture("rom:/assets/transparent.png", true, "transparent");

    ResourceManager::CreateEmptyTexture(160, 144, 4, "GBFrameBuffer");
    ResourceManager::CreateEmptyTexture(256, 160, 4, "GBAFrameBuffer");
    ResourceManager::CreateEmptyTexture(256, 240, 4, "NESFrameBuffer");

    g::spriteRenderer = new SpriteRenderer(ResourceManager::GetShader("sprite"));
    g::emulationRenderer = new SpriteRenderer(ResourceManager::GetShader("emulation"));
    g::lcdRenderer = new SpriteRenderer(ResourceManager::GetShader("lcd"));
    g::transitionRenderer = new SpriteRenderer(ResourceManager::GetShader("transition"));
    ResourceManager::GetShader("gbc");  // queried but never loaded (cut GameBoy Color filter)

    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    bool ok = g::font.LoadFont(gd.font.c_str(), 1280, 720);
    if (ok) ok = g::fontSmall.LoadFont(gd.fontSmall.c_str(), 1280, 720);
    return ok;
}

void Renderer::BeginFrame() {
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
}

void Renderer::EndFrame() {
    glFinish();
    Platform::SwapBuffers();
}

void Renderer::DrawMainMenu() {
    g::menuFade = kWhite;
    Vec4 color = kWhite;
    if (g::menuAnimState == 2) {
        float s = g::menuScroll + 0.6f;
        g::menuScroll = (s <= 640.0f) ? s : 0.0f;
        g::hintFade = kWhite;
        g::hintColor = kWhite;
    } else if (g::menuAnimState == 1) {
        for (float& c : g::menuFadeInRGB) c += 0.015f;
        if (g::menuFadeInRGB[0] >= 1.0f) {
            g::menuFadeInRGB[0] = g::menuFadeInRGB[1] = g::menuFadeInRGB[2] = 1.0f;
            g::menuAnimState = 2;
        }
        color = {g::menuFadeInRGB[0], g::menuFadeInRGB[1], g::menuFadeInRGB[2], 1.0f};
    } else if (g::menuAnimState == 0) {
        g::menuSpriteX -= 36;
        if (g::menuSpriteX < 321) {
            g::menuSpriteX = 320;
            g::menuAnimState = 1;
        }
        for (float& c : g::menuFadeInRGB) c += 0.015f;
        color = {g::menuFadeInRGB[0], g::menuFadeInRGB[1], g::menuFadeInRGB[2], 1.0f};
    }

    g::shaderTime += 0.02f;
    ResourceManager::GetShader("emulation").SetFloat("time", g::shaderTime, true);

    DrawSprite(g::spriteRenderer, "MenuBackground", {0, 0}, kFullScreen, color);
    DrawSprite(g::emulationRenderer, "MenuBackground", {0, 0}, kFullScreen, color);
    DrawSprite(g::spriteRenderer, "MenuSprite", {static_cast<float>(g::menuSpriteX), 0.0f},
               {MenuSpriteWidth(), MenuSpriteHeight()}, kWhite);
    if (g::gameDef->gameLogoEnabled)
        DrawSprite(g::spriteRenderer, "GameLogo", kLogoPos, kLogoSize, color);
    if (g::gameDef->buttonBEnabled)
        DrawSprite(g::spriteRenderer, "B_Button", kBButtonPos, kBButtonSize, color);
    DrawSprite(g::spriteRenderer, "DescriptionBar", kDescBarPos, kDescBarSize, color);
}

void Renderer::DrawMenuList() {
    const int titleCount = g::gameDef->GetTitleCount();
    const int oldHue = g::hue;
    g::hue = (oldHue + 2 < 360) ? oldHue + 2 : 40;
    g::field_71001b2988 = 0;
    g::rainbowColor = g::kHueTable[(oldHue + 240) % 360] | 0xFFFFFF00u;

    const std::string previewName = "MenuPreview" + std::to_string(menuSelection);
    if (g::menuAnimState <= 0) return;

    std::vector<GameEntry> titles = g::gameDef->titles;  // @ 0x71000a2360 copies the vector every frame
    if (menuSelection < titleCount) {
        DrawSprite(g::spriteRenderer, previewName.c_str(), {62.0f, 404.0f}, {200.0f, 180.0f}, kWhite);
        DrawSprite(g::spriteRenderer, "GameBorder", {54.0f, 394.0f}, {218.0f, 200.0f}, kWhite);
    }

    auto drawSelection = [&](int index) {
        DrawSprite(g::spriteRenderer, "MenuBar", {80.0f, static_cast<float>(index * 40 + 240)}, kMenuBarSize, kWhite);
        DrawSprite(g::spriteRenderer, "SelectIcon", {26.0f, static_cast<float>(index * 40 + 220)}, kSelectIconSize, kWhite);
    };

    int y = 500;
    for (const GameEntry& e : titles) {
        g::font.ResetCursor();
        y -= 40;
        const bool selected = menuSelection == static_cast<uint8_t>(e.id);
        g::font.Print(selected ? 140 : 90, y, e.name.c_str(), 1, selected ? 0xFFFFFFFFu : kUnselectedText);
        g::font.Flush();
        if (selected) {
            std::string desc = e.description;
            g::font.ResetCursor();
            g::font.Print(90, 68, desc.c_str(), 1, 0xFFFFFFFFu);
            g::font.Flush();
            drawSelection(menuSelection);
        }
    }

    g::font.ResetCursor();
    bool sel = menuSelection == titleCount;
    g::font.Print(sel ? 140 : 90, y - 40, "EXTRAS", 1, sel ? 0xFFFFFFFFu : kUnselectedText);
    g::font.Flush();
    if (sel) {
        drawSelection(titleCount);
        g::font.ResetCursor();
        g::font.Print(90, 68, "Explore bonus content", 1, 0xFFFFFFFFu);
        g::font.Flush();
    }

    g::font.ResetCursor();
    sel = menuSelection == titleCount + 1;
    g::font.Print(sel ? 140 : 90, y - 80, "CREDITS", 1, sel ? 0xFFFFFFFFu : kUnselectedText);
    g::font.Flush();
    if (sel) {
        drawSelection(titleCount + 1);
        g::font.ResetCursor();
        g::font.Print(90, 68, "View the credits!", 1, 0xFFFFFFFFu);
        g::font.Flush();
    }
}

void Renderer::DrawLaunchTransition() {
    Vec4 color{0.0f, 0.0f, 0.0f, 1.0f};
    bool applyFadeStep = false;
    switch (g::menuAnimState) {
    case 0:
        g::menuSpriteX += 36;
        if (g::menuSpriteX > 1400) {
            g::menuSpriteX = 1400;
            g::menuAnimState = 1;
        }
        applyFadeStep = true;
        break;
    case 1:
        if (g::menuFade.r - 0.035f < 0.0f) {
            g::menuFade = {0.0f, 0.0f, 0.0f, 1.0f};
            g::menuAnimState = 2;
        } else {
            applyFadeStep = true;
        }
        break;
    case 2: {
        float s = g::menuScroll + 0.6f;
        g::menuAnimState = 3;
        g::menuScroll = (s <= 640.0f) ? s : 0.0f;
        break;
    }
    case 3:
        g::menuSpriteX = 120;
        g::menuFade = {0.0f, 0.0f, 0.0f, 0.0f};
        g::menuFadeInRGB[0] = g::menuFadeInRGB[1] = g::menuFadeInRGB[2] = 1.0f;
        g::transitionActive = false;
        return;
    default:
        break;  // original draws with an uninitialised colour here (unreachable)
    }
    if (applyFadeStep) {
        g::menuFade.r -= 0.035f;
        g::menuFade.g -= 0.035f;
        g::menuFade.b -= 0.035f;
        color = g::menuFade;
    }

    g::shaderTime += 0.02f;
    ResourceManager::GetShader("emulation").SetFloat("time", g::shaderTime, true);

    DrawSprite(g::spriteRenderer, "MenuBackground", {0, 0}, kFullScreen, color);
    DrawSprite(g::spriteRenderer, "MenuSprite", {static_cast<float>(g::menuSpriteX), 0.0f},
               {MenuSpriteWidth(), MenuSpriteHeight()}, kWhite);
    if (g::gameDef->gameLogoEnabled)
        DrawSprite(g::spriteRenderer, "GameLogo", kLogoPos, kLogoSize, color);
    if (g::gameDef->buttonBEnabled)
        DrawSprite(g::spriteRenderer, "B_Button", kBButtonPos, kBButtonSize, color);
}

void Renderer::DrawPauseOptions() {
    if (g::pauseSub[0] == 3 || g::pauseSub[0] == 4) return;

    const int h = g::hue;
    g::rainbowColor = (static_cast<uint32_t>(g::kHueTable[(h + 120) % 360]) << 16) |
                      (static_cast<uint32_t>(g::kHueTable[h]) << 8) |
                      g::kHueTable[(h + 240) % 360] | 0xFF000000u;
    g::hue = (h + 8 < 360) ? h + 8 : 40;

    const uint32_t alpha = static_cast<uint32_t>(static_cast<int>(g::pauseFade.b * 255.0f));
    const uint32_t selColor = alpha | 0xFFFFFF00u;
    const uint32_t unselColor = alpha | 0xF9F9BC00u;

    auto item = [&](int index, int y, const char* text) {
        g::font.ResetCursor();
        const bool sel = pauseSelection == index;
        g::font.Print(sel ? 220 : 180, y, text, 1, sel ? selColor : unselColor);
        g::font.Flush();
    };
    item(0, 400, "RESUME GAME");
    item(1, 340, "LOAD STATE");
    item(2, 280, "SAVE STATE");
    item(3, 220, "RESET GAME");

    g::font.ResetCursor();
    static const char* const kFilterNames[] = {"FILTER : Sharp", "FILTER : LCD", "FILTER : Native", "Filter : GameBoy Color"};
    if (filter >= 0 && filter <= 3) {
        const bool sel = pauseSelection == 4;
        // (for filter 3 the original leaves x uninitialised when the item is selected; unreachable)
        g::font.Print(sel ? 220 : 180, 160, kFilterNames[filter], 1, sel ? selColor : unselColor);
    }
    g::font.Flush();

    item(5, 100, "EXIT TO MAIN MENU");

    if (pauseSelection == 0) {
        g::font.ResetCursor();
        g::font.Print(860, 540, "GAME CONTROLS", 1, unselColor);
        g::font.Flush();
    }
}

void Renderer::DrawPauseMenu(bool closing) {
    g::hue = (g::hue < 359) ? g::hue + 1 : 40;

    if (!closing) {
        pauseSlideX = std::min(pauseSlideX + 24.0f, 0.0f);
        g::menuFade.r += 0.1f; g::menuFade.g += 0.1f; g::menuFade.b += 0.1f; g::menuFade.a += 0.1f;
        if (g::menuFade.r > 1.0f) g::menuFade = kWhite;
    } else {
        pauseSlideX = std::max(pauseSlideX - 24.0f, -720.0f);
        g::menuFade.r -= 0.1f; g::menuFade.g -= 0.1f; g::menuFade.b -= 0.1f; g::menuFade.a -= 0.1f;
        if (g::menuFade.r <= 0.0f) g::menuFade = {0.0f, 0.0f, 0.0f, 0.0f};
    }
    const Vec4 fade = g::menuFade;
    g::pauseFade = fade;
    const Vec4 c = g::pauseFade;

    const char* confirmText = nullptr;
    if (g::pauseSub[0] == 3) {
        const std::string preview = "PreviewImage" + std::to_string(g::pauseSub[1]);
        DrawSprite(g::spriteRenderer, "DialogBox", {380.0f, 270.0f}, {500.0f, 200.0f}, c);
        DrawSprite(g::spriteRenderer, preview.c_str(), {670.0f, 300.0f}, {160.0f, 144.0f}, c);
        g::font.ResetCursor();
        confirmText = "CONFIRM LOAD ?";
    } else if (g::pauseSub[0] != 4) {
        DrawSprite(g::spriteRenderer, "PauseMenu", {0, 0}, kFullScreen, fade);
        DrawSprite(g::spriteRenderer, "MenuBar", {180.0f, static_cast<float>(pauseSelection * 60 + 300)}, kMenuBarSize, c);
        DrawSprite(g::spriteRenderer, "SelectIcon", {120.0f, static_cast<float>(pauseSelection * 60 + 286)}, kSelectIconSize, c);

        if (static_cast<unsigned>(pauseSelection - 1) > 1) {
            if (pauseSelection == 0)
                DrawSprite(g::spriteRenderer, "ControllerImage", {750.0f, 220.0f}, {474.0f, 354.8f}, c);
            return;
        }

        // Selected slot box: hue-table bytes (0..255) multiplied by the fade WITHOUT /255, so the box is
        // effectively white and blinks out whenever a table byte is 0.
        auto boxColor = [&](int slot) -> Vec4 {
            if (closing || g::pauseSub[1] != slot) return c;
            const int h = g::hue;
            return {c.b * 255.0f,
                    static_cast<float>(g::kHueTable[(h + 20) % 360]) * c.b,
                    static_cast<float>(g::kHueTable[(h + 60) % 360]) * c.b,
                    static_cast<float>(g::kHueTable[(h + 40) % 360]) * c.b};
        };
        DrawSprite(g::spriteRenderer, "SaveStateBox", {700.0f, 150.0f}, {400.0f, 120.0f}, boxColor(0));
        DrawSprite(g::spriteRenderer, "SaveStateBox", {700.0f, 300.0f}, {400.0f, 120.0f}, boxColor(1));
        DrawSprite(g::spriteRenderer, "SaveStateBox", {700.0f, 450.0f}, {400.0f, 120.0f}, boxColor(2));

        g::font.ResetCursor();
        g::fontSmall.ResetCursor();
        // (int) of each [0,1] fade component: text is fully transparent until the fade is exactly 1.0
        const uint32_t textColor = static_cast<uint32_t>(
            static_cast<int>(fade.g) * 0xFF + static_cast<int>(fade.b) * 0xFF00 +
            static_cast<int>(fade.r) * 0xFF0000 + static_cast<int>(fade.a) * -0x1000000);

        struct SlotLayout { int slot, titleY, stampY; float previewY; };
        const SlotLayout layout[] = {{2, 210, 170, 456.0f}, {1, 360, 320, 306.0f}, {0, 510, 470, 156.0f}};
        for (const SlotLayout& L : layout) {
            const SaveSlotInfo& s = g::gbSystem->slots[L.slot];
            g::font.Print(860, L.titleY, s.title.c_str(), 1, textColor);
            if (s.empty == 0) {
                const std::string preview = "PreviewImage" + std::to_string(L.slot);
                g::fontSmall.Print(860, L.stampY, s.timestamp.c_str(), 1, textColor);
                DrawSprite(g::spriteRenderer, preview.c_str(), {710.0f, L.previewY}, {130.0f, 108.0f}, c);
            }
            if (L.slot == 0) break;  // slot 0 skips these flushes: its small text is flushed on the next frame
            g::fontSmall.Flush();
            g::font.Flush();
            g::font.ResetCursor();
        }
        g::font.Flush();
        return;
    } else {
        Texture2D& fb = ResourceManager::GetTexture("GBFrameBuffer");
        fb.Generate(160, 144, gb::Framebuffer(), false);
        DrawSprite(g::spriteRenderer, "DialogBox", {380.0f, 270.0f}, {500.0f, 200.0f}, c);
        g::spriteRenderer->DrawSprite(fb, {670.0f, 300.0f}, {160.0f, 144.0f}, 0.0f, c);
        g::font.ResetCursor();
        confirmText = "CONFIRM SAVE ?";
    }
    g::font.Print(440, 340, confirmText, 1, 0xB3060FFFu);
    g::font.Flush();
}

void Renderer::PresentGB(Vec4 tint) {
    const int frames = g::gbSystem->frameCount;
    if (!g::paused && frames < 600) {
        Vec4 color;
        if (frames < 540) {
            color = g::hintColor;
        } else {
            g::hintFade.r = std::max(g::hintFade.r - 0.04f, 0.0f);
            g::hintFade.g = std::max(g::hintFade.g - 0.04f, 0.0f);
            g::hintFade.b = std::max(g::hintFade.b - 0.04f, 0.0f);
            g::hintFade.a = std::max(g::hintFade.a - 0.04f, 0.0f);
            g::hintColor = g::hintFade;
            color = g::hintFade;
        }
        DrawSprite(g::spriteRenderer, "PauseMenuText", {1080.0f, 660.0f}, {147.0f, 52.0f}, color);
    }
    if (frames <= 60) return;  // nothing is shown for the first second of gameplay

    Texture2D& fb = ResourceManager::GetTexture("GBFrameBuffer");
    SpriteRenderer* r;
    Vec2 pos, size;
    switch (filter) {
    case 2: r = g::spriteRenderer; pos = {320.0f, 72.0f}; size = {640.0f, 576.0f}; break;  // Native, 4x centred
    case 1: r = g::lcdRenderer; pos = {225.0f, 0.0f}; size = {800.0f, 720.0f}; break;     // LCD
    case 0: r = g::spriteRenderer; pos = {225.0f, 0.0f}; size = {800.0f, 720.0f}; break;  // Sharp
    default: return;                                                                        // GameBoy Color: no path
    }
    fb.Generate(160, 144, gb::Framebuffer(), false);  // full glTexImage2D every frame, NEAREST
    r->DrawSprite(fb, pos, size, 0.0f, tint);
}

void Renderer::PresentGBA() {
    Texture2D& fb = ResourceManager::GetTexture("GBAFrameBuffer");
    fb.Generate(256, 160, gba::Framebuffer(), filter != 0);
    if (gbaBackground != 0)
        DrawSprite(g::spriteRenderer, "GameBackground", {0, 0}, kFullScreen, kWhite);
    // The shipped code never draws GBAFrameBuffer itself.
}
