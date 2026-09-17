// App — top-level state machine. Original: ctor @ 0x71000e9980, Init @ 0x71000e99a0, Run @ 0x71000e9bb0,
// UpdateInput @ 0x71000ea3e0, ShowSplash @ 0x71000ea0b0, ShowCredits @ 0x71000eb800. nnMain @ 0x71000a0440.
#pragma once
#include "gfx/SpriteRenderer.h"
#include <cstdint>

class Renderer;

enum class AppState : int32_t {
    MainMenu = 0,
    Credits = 2,
    Artwork = 3,
    InGame = 4,
    Pause = 5,
    Submenu6 = 6,          // unreachable 4-item submenu -> {9, 10, 12, 11}
    ExitToMenu = 9,
    ResetGame = 10,
    Unhandled11 = 11,
    Unhandled12 = 12,
    Options13 = 13,        // unreachable options submenu
    LaunchGame = 14,       // plays the launch transition, then loads the ROM
    ReturnToMenu = 15,     // never entered by the shipped code
};

class App {
public:
    AppState state = AppState::MainMenu;  // +0x00
    int32_t launchedSystem = 0;           // +0x04 set to 1 on launch; used by state 13 to pick a presenter
    Renderer* renderer = nullptr;         // +0x1d0
    int32_t titleCount = 0;               // +0x1d8

    bool Init();
    void Run();

private:
    void UpdateInput();
    void ShowSplash();
    void ShowCredits();
    void CloseGameOnExitRequest();

    Vec4 tint{1.0f, 1.0f, 1.0f, 1.0f};    // Run()'s local colour for the game frame
};

int CarbonMain();  // nnMain equivalent
