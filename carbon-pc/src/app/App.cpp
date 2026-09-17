#include "app/App.h"
#include "app/Artwork.h"
#include "app/EmuGlue.h"
#include "app/GameDef.h"
#include "app/Globals.h"
#include "app/Input.h"
#include "app/MenuAudio.h"
#include "app/Renderer.h"
#include "gfx/ResourceManager.h"
#include "platform/Platform.h"
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace {
constexpr AppState kSubmenu6Targets[4] = {AppState::ExitToMenu, AppState::ResetGame, AppState::Unhandled12,
                                          AppState::Unhandled11};  // @ 0x7100193e00

void LoadSlotPreviewTextures() {
    for (int i = 0; i < 3; ++i)
        ResourceManager::LoadTexture(g::gbSystem->slots[i].previewPath.c_str(), true, "PreviewImage" + std::to_string(i));
}
}  // namespace

// @ 0x71000a0440
int CarbonMain() {
    // original: nn::account preselected user, EnsureSaveData/MountSaveData("save"), MountRom("rom"), srand(time)
    if (!Platform::MountSaveAndRom()) return 1;
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    g::app = new App;
    if (!g::app->Init()) return 1;  // (the original ignores the result and runs anyway)
    g::app->Run();
    return 0;
}

// @ 0x71000e99a0
bool App::Init() {
    g::gameDef = new GameDef;
    if (!g::gameDef->Load()) return false;

    renderer = new Renderer;
    if (!renderer->Init()) return false;

    menu_audio::Start();  // StartThread of the thread created at the top of the original Init
    artwork::LoadTextures();

    g::input = new Input;
    g::input->Init();

    renderer->menuSelection = 0;
    renderer->pauseSelection = 0;
    renderer->submenu6Selection = 0;
    renderer->filter = 0;
    renderer->option13Value0 = 0;
    renderer->options13Selection = 0;
    renderer->option13Changed = false;
    renderer->gbaBackground = 1;
    renderer->field_1c = 0;

    g::gbSystem = new GbSystem;
    g::gbaKeys = new uint32_t(0);
    carts::current = nullptr;
    titleCount = g::gameDef->GetTitleCount();
    return true;
}

// @ 0x71000ea0b0 — shows Splash1 for 6 s of wall-clock time. Splash2 is loaded but never drawn.
void App::ShowSplash() {
    ResourceManager::LoadTexture(g::gameDef->splash1.c_str(), true, "Splash1");
    ResourceManager::LoadTexture(g::gameDef->splash2.c_str(), true, "Splash2");
    SpriteRenderer splashRenderer(ResourceManager::GetShader("sprite"));
    const int64_t start = Platform::MonotonicNs();
    do {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        splashRenderer.DrawSprite(ResourceManager::GetTexture("Splash1"), {0, 0}, {1280.0f, 720.0f}, 0.0f, {1, 1, 1, 1});
        glFinish();
        Platform::SwapBuffers();
    } while ((Platform::MonotonicNs() - start) / 1000000000LL < 6);
}

// @ 0x71000eb800 — offline web applet with the gamedef credits document: background kind 1, footer, pointer,
// boot loading icon and touch off; JS extension and web audio on. Blocks until the applet closes.
void App::ShowCredits() {
    char url[0xC00];
    std::snprintf(url, sizeof(url), g::gameDef->creditsDocument.c_str(), 0);  // (format string use, as original)
    Platform::ShowOfflineHtml(url);
}

// exit-request branch of @ 0x71000e9bb0
void App::CloseGameOnExitRequest() {
    if (carts::current != nullptr && carts::SystemType(carts::current) < 2) {
        gb::SaveState(g::gbSystem, 4, 0);  // hidden slot 4, game 0; never loaded back
        gb::Shutdown();
        carts::FreeBuffers(carts::current);  // frees buffers only: SRAM is NOT flushed
        carts::Destroy(carts::current);
        carts::current = nullptr;
    }
}

// @ 0x71000e9bb0
void App::Run() {
    NpadSnapshot pad;
    Platform::PollNpad(pad);
    g::input->Update(pad);
    g::pauseSub[0] = 0;
    ShowSplash();
    tint = {1.0f, 1.0f, 1.0f, 1.0f};

    for (;;) {
        const bool exitRequested = Platform::PopExitRequest();
        if (exitRequested) {
            CloseGameOnExitRequest();
            // The original goes on to UpdateInput, which reads the now-NULL cartridge while in game. On the Switch
            // the OS ends the process as soon as the exit-request section is left, so return here instead.
            return;
        }

        UpdateInput();

        const uint32_t sysType = carts::current ? carts::SystemType(carts::current) : 0;
        switch (state) {
        case AppState::MainMenu:
            renderer->BeginFrame();
            renderer->DrawMainMenu();
            renderer->DrawMenuList();
            renderer->EndFrame();
            break;
        case AppState::Artwork:
            artwork::Draw();
            break;
        case AppState::InGame:
            renderer->BeginFrame();
            if (sysType < 2) {
                gb::RunFrame(g::gbSystem);
                renderer->PresentGB(tint);
            } else if (sysType == 2) {
                gba::RunFrame(g::gbaKeys);
                renderer->PresentGBA();
            }
            renderer->EndFrame();
            break;
        case AppState::Pause: {
            g::gameTint.r -= 0.035f; g::gameTint.g -= 0.035f; g::gameTint.b -= 0.035f; g::gameTint.a -= 0.035f;
            if (g::gameTint.r >= 0.2f) {
                tint = g::gameTint;
            } else {
                g::gameTint = {0.2f, 0.2f, 0.2f, 0.2f};
                tint = g::gameTint;
            }
            renderer->BeginFrame();
            if (sysType < 2) {
                gb::ClearAudioRing();  // silence while paused
                renderer->PresentGB(tint);
            } else if (sysType == 2) {
                gba::ClearAudioRing();
                renderer->PresentGBA();
            }
            renderer->DrawPauseMenu(g::pauseClosing);
            if (g::pauseClosing) {
                if (renderer->pauseSlideX <= -720.0f) {
                    renderer->pauseSlideX = -720.0f;
                    state = AppState::InGame;
                    g::pauseClosing = false;
                }
                g::gameTint.r += 0.1f; g::gameTint.g += 0.1f; g::gameTint.b += 0.1f; g::gameTint.a += 0.1f;
                if (g::gameTint.r > 1.0f) g::gameTint = {1.0f, 1.0f, 1.0f, 1.0f};
                tint = g::gameTint;
            }
            renderer->DrawPauseOptions();
            renderer->EndFrame();
            break;
        }
        case AppState::Submenu6:
            renderer->BeginFrame();
            if (sysType < 2) renderer->PresentGB(tint);
            else if (sysType == 2) renderer->PresentGBA();
            renderer->DrawPauseMenu(g::pauseClosing);
            renderer->EndFrame();
            break;
        case AppState::ExitToMenu:
            g::gameTint = {1.0f, 1.0f, 1.0f, 1.0f};
            tint = g::gameTint;
            break;
        case AppState::Options13:
            renderer->BeginFrame();
            if (launchedSystem == 2) renderer->PresentGBA();
            else if (launchedSystem == 1) renderer->PresentGB(tint);
            renderer->DrawPauseMenu(false);
            renderer->EndFrame();
            break;
        case AppState::LaunchGame:
            renderer->BeginFrame();
            if (g::transitionActive) renderer->DrawLaunchTransition();
            renderer->EndFrame();
            break;
        case AppState::ReturnToMenu:
            renderer->BeginFrame();
            if (g::transitionActive) renderer->DrawMainMenu();
            renderer->EndFrame();
            break;
        default:
            break;
        }
    }
}

// @ 0x71000ea3e0
void App::UpdateInput() {
    NpadSnapshot pad;
    Platform::PollNpad(pad);
    Input& in = *g::input;
    in.Update(pad);

    switch (state) {
    case AppState::MainMenu: {
        int& sel = renderer->menuSelection;
        if (in.pressed & CB_ANY_UP) {
            if (--sel < 0) sel = titleCount + 1;
            g::sfxMove = 1;
        } else if (in.pressed & CB_ANY_DOWN) {
            ++sel;
            if (static_cast<unsigned>(titleCount + 1) < static_cast<unsigned>(sel)) sel = 0;
            g::sfxMove = 1;
        } else if (in.pressed & CB_A) {
            if (g::menuAnimState != 2) return;
            g::sfxSelect = 1;
            switch (sel) {  // hard-coded for exactly two titles
            case 0:
            case 1:
                g::menuAnimState = 0;
                g::transitionActive = true;
                state = AppState::LaunchGame;
                g::musicFadeOut = 1;
                break;
            case 2:
                state = AppState::Artwork;
                artwork::Select();
                g::menuAnimState = 0;
                g::transitionActive = true;
                return;
            case 3:
                state = AppState::Credits;
                return;
            default:
                return;
            }
        }
        break;
    }

    case AppState::Credits:
        ShowCredits();
        state = AppState::MainMenu;
        break;

    case AppState::Artwork: {
        uint32_t p = in.pressed;
        if (p & CB_A) {
            artwork::uiVisible = 0;
            artwork::uiTimer = 0;
        } else if (in.released & CB_A) {
            artwork::uiVisible = 1;
            artwork::uiTimer = 0;
        }
        if (p & CB_B) {
            g::sfxBack = 1;
            state = AppState::MainMenu;
            renderer->DrawLaunchTransition();  // called from the input handler in the original
            p = in.pressed;
        }
        int idx;
        if (p & CB_R) idx = (artwork::index < 15) ? artwork::index + 1 : 0;
        else if (p & CB_L) idx = (artwork::index > 0) ? artwork::index - 1 : 15;
        else return;
        g::sfxToggle = 1;
        artwork::index = idx;
        artwork::Select();
        break;
    }

    case AppState::InGame: {
        const uint32_t sysType = carts::SystemType(carts::current);
        if (sysType < 2) {
            // D-pad: up/down/right follow the held state, left only the pressed edge (as shipped)
            if (in.held & CB_ANY_UP) gb::KeyDown(2); else if (in.released & CB_ANY_UP) gb::KeyUp(2);
            if (in.held & CB_ANY_DOWN) gb::KeyDown(3); else if (in.released & CB_ANY_DOWN) gb::KeyUp(3);
            if (in.held & CB_ANY_RIGHT) gb::KeyDown(0); else if (in.released & CB_ANY_RIGHT) gb::KeyUp(0);
            if (in.pressed & CB_ANY_LEFT) gb::KeyDown(1); else if (in.released & CB_ANY_LEFT) gb::KeyUp(1);
            // Switch B -> GB A, Switch A and Y -> GB B, + -> Start, X -> Select
            if (in.pressed & CB_B) gb::KeyDown(4); else if (in.released & CB_B) gb::KeyUp(4);
            if (in.pressed & CB_A) gb::KeyDown(5); else if (in.released & CB_A) gb::KeyUp(5);
            if (in.pressed & CB_Y) gb::KeyDown(5); else if (in.released & CB_Y) gb::KeyUp(5);
            if (in.pressed & CB_PLUS) gb::KeyDown(7); else if (in.released & CB_PLUS) gb::KeyUp(7);
            if (in.pressed & CB_X) gb::KeyDown(6); else if (in.released & CB_X) gb::KeyUp(6);

            uint32_t held = in.held;
            if ((~held & 0x70) == 0) {  // ZL+ZR (or L+ZR / ZL+R)
                state = AppState::Pause;
                renderer->pauseSelection = 0;
                g::pauseSub[0] = 0;
                g::gameTint = {1.0f, 1.0f, 1.0f, 1.0f};
                g::paused = true;
                held = in.held;
            }
            if (held & CB_L) g::gbSystem->lShoulderHeld = 1;
            else if (in.released & CB_L) g::gbSystem->lShoulderHeld = 0;
            return;
        }
        if (sysType != 2) return;
        uint32_t keys = 0;
        uint32_t held = in.held;
        if (held & CB_ANY_UP) keys |= 0x40;
        if (held & CB_ANY_DOWN) keys |= 0x80;
        if (held & CB_ANY_RIGHT) keys |= 0x10;
        if (held & CB_ANY_LEFT) keys |= 0x20;
        if (held & CB_B) keys |= 0x2;
        if (held & CB_A) keys |= 0x1;
        if (held & CB_PLUS) keys |= 0x8;
        if (held & CB_L) keys |= 0x200;
        if (held & CB_R) keys |= 0x100;
        *g::gbaKeys = keys;  // GBA Select is not mapped
        if (held & 0x50) state = AppState::Pause;  // L / ZL / ZR open the pause menu, so GBA L is unusable
        break;
    }

    case AppState::Pause: {
        int& sel = renderer->pauseSelection;
        int& sub = g::pauseSub[0];
        int& slot = g::pauseSub[1];
        uint32_t p = in.pressed;
        if (p & CB_ANY_UP) {
            if (static_cast<unsigned>(sub - 1) < 2) {
                if (--slot < 0) slot = 0;
                g::sfxToggle = 1;
            } else {
                if (--sel < 0) sel = 5;
                g::sfxMove = 1;
            }
        } else if (p & CB_ANY_DOWN) {
            if (1 < static_cast<unsigned>(sub - 1)) {
                if (++sel > 5) sel = 0;
                g::sfxMove = 1;
            } else {
                if (++slot >= 3) slot = 2;
                g::sfxToggle = 1;
            }
        }
        if (in.pressed & CB_ANY_RIGHT) {
            if (sel == 2) { slot = 0; sub = 2; g::sfxToggle = 1; }
            else if (sel == 1) { slot = 0; sub = 1; g::sfxToggle = 1; }
        }
        p = in.pressed;
        if (p & CB_ANY_LEFT) {
            if (sub != 2 && sub != 1) return;
            slot = -1;
            sub = 0;
            g::sfxMove = 1;
            return;
        }

        auto resume = [&] {
            g::pauseClosing = true;
            g::paused = false;
            g::musicFadeOut = 1;
        };
        if (!(p & CB_A)) {
            if (!(p & CB_B)) return;
            g::sfxBack = 1;
            if (sub != 3 && sub != 4) resume();
            else sub = 2;
        } else {
            switch (sub) {
            case 1:  // load immediately (no confirmation). Always the GB loader, even for GBA games.
                g::menuFade = {0, 0, 0, 0};
                gb::LoadState(g::gbSystem, slot, renderer->menuSelection);
                g::pauseClosing = true;
                g::paused = false;
                g::musicFadeOut = 1;
                g::sfxSelect = 1;
                break;
            case 2:
                g::menuFade = {0, 0, 0, 0};
                sub = 4;
                g::sfxToggle = 1;
                break;
            case 3:
                gb::LoadState(g::gbSystem, slot, renderer->menuSelection);
                resume();
                break;
            case 4:
                gb::SaveState(g::gbSystem, slot, renderer->menuSelection);
                gb::LoadSlotPreviews(g::gbSystem, carts::Title(carts::current), renderer->menuSelection);
                LoadSlotPreviewTextures();
                sub = 2;
                g::sfxSelect = 1;
                break;
            default:
                switch (sel) {
                case 1: slot = 0; sub = 1; g::sfxToggle = 1; break;
                case 2: slot = 0; sub = 2; g::sfxToggle = 1; break;
                case 3: gb::Reset(); resume(); break;  // always the GB reset (CPU + APU only)
                case 0: resume(); break;
                case 4:
                    g::sfxToggle = 1;
                    if (++renderer->filter > 2) renderer->filter = 0;
                    break;
                case 5:
                    state = AppState::ExitToMenu;
                    g::paused = false;
                    break;
                default:
                    break;
                }
            }
        }
        for (int k = 0; k < 8; ++k) gb::KeyUp(k);
        break;
    }

    case AppState::Submenu6: {
        int& s = renderer->submenu6Selection;
        uint32_t p = in.pressed;
        if (p & CB_ANY_UP) { if (--s < 0) s = 3; return; }
        if (p & CB_ANY_DOWN) { if (++s >= 4) s = 0; return; }
        if (!(p & CB_A)) {
            if (!(p & CB_B)) return;
            state = AppState::Pause;
            g::sfxBack = 1;
            return;
        }
        if (static_cast<unsigned>(s) < 4) state = kSubmenu6Targets[s];
        g::sfxSelect = 1;
        break;
    }

    case AppState::ExitToMenu: {
        const uint32_t sysType = carts::SystemType(carts::current);
        if (sysType < 2) {
            gb::Shutdown();
            if (carts::current) {
                carts::FreeBuffers(carts::current);
                carts::Destroy(carts::current);  // (original runs the base dtor without delete here)
            }
            carts::current = nullptr;
        } else if (sysType == 2) {
            gba::Exit();  // writes save:/<title>.stt (auto-resume); the cart object is left in place
        }
        g::menuSpriteX = 1800;
        g::menuAnimState = 0;
        g::menuFadeInRGB[0] = g::menuFadeInRGB[1] = g::menuFadeInRGB[2] = 0.0f;
        state = AppState::MainMenu;
        g::musicRestart = 1;
        g::musicFadeIn = 1;
        g::musicFadeInTarget = 0.7f;  // menu music comes back louder than its initial 0.5
        g::sfxBack = 1;
        break;
    }

    case AppState::ResetGame: {
        const uint32_t sysType = carts::SystemType(carts::current);
        if (sysType < 2) { gb::Reset(); state = AppState::InGame; }
        else if (sysType == 2) { gba::Reset(); state = AppState::InGame; }
        g::sfxBack = 1;
        break;
    }

    case AppState::Options13: {
        int& opt = renderer->options13Selection;
        uint32_t p = in.pressed;
        if (p & CB_ANY_UP) { if (--opt < 0) opt = 2; }
        else if (p & CB_ANY_DOWN) { if (++opt > 2) opt = 0; }
        if (in.pressed & CB_A) {
            if (opt == 2) { if (++renderer->gbaBackground >= 2) renderer->gbaBackground = 0; }
            else if (opt == 1) { if (++renderer->filter > 2) renderer->filter = 0; }
            else if (opt == 0) { renderer->option13Changed = true; if (++renderer->option13Value0 > 2) renderer->option13Value0 = 0; }
        }
        if (!(in.pressed & CB_B)) return;
        g::sfxBack = 1;
        state = AppState::Pause;
        break;
    }

    case AppState::LaunchGame: {
        if (g::transitionActive) break;
        std::vector<GameEntry> titles = g::gameDef->titles;
        for (const GameEntry& e : titles) {
            if (static_cast<uint32_t>(renderer->menuSelection) == static_cast<uint8_t>(e.id)) {
                state = AppState::InGame;
                launchedSystem = 1;
                carts::current = carts::Load(e.romName);
                break;
            }
        }
        // (the original dereferences gb_mapper here even if no title matched)
        const uint32_t sysType = carts::SystemType(carts::current);
        if (sysType < 2) {
            gb::Init(g::gbSystem);
            gb::LoadSlotPreviews(g::gbSystem, carts::Title(carts::current), renderer->menuSelection);
            LoadSlotPreviewTextures();
        } else if (sysType == 2) {
            gba::LoadRom(g::gbaKeys);
        }
        break;
    }

    case AppState::ReturnToMenu:
        if (!g::transitionActive) state = AppState::MainMenu;
        break;

    default:
        break;
    }
}
