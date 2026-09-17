// Renderer — menus, pause menu and emulator presentation (object 0x78 bytes @ App+0x1d0).
// The object doubles as the menu-selection state that App::UpdateInput mutates.
#pragma once
#include "gfx/SpriteRenderer.h"
#include <cstdint>

class Renderer {
public:
    int32_t menuSelection = 0;       // +0x00 main menu (titles..., EXTRAS, CREDITS)
    int32_t pauseSelection = 0;      // +0x04 0 Resume,1 Load,2 Save,3 Reset,4 Filter,5 Exit
    int32_t submenu6Selection = 0;   // +0x08 unreachable 4-item submenu (state 6)
    int32_t options13Selection = 0;  // +0x0c unreachable 3-item options submenu (state 13)
    int32_t option13Value0 = 0;      // +0x10
    int32_t filter = 0;              // +0x14 0 Sharp, 1 LCD, 2 Native (3 "GameBoy Color" is unreachable)
    int32_t gbaBackground = 1;       // +0x18 (option 2 of the unreachable state-13 submenu toggles it)
    int32_t field_1c = 0;            // +0x1c
    bool option13Changed = false;    // +0x20
    float pauseSlideX = -500.0f;     // +0x48 (ctor value; the pause menu treats it as an open/close timer)

    bool Init();                                // @ 0x7100097f70
    void BeginFrame();                          // @ 0x710009a1d0: glClear(COLOR|DEPTH)
    void EndFrame();                            // @ 0x710009a1f0: glFinish + swap
    void DrawMainMenu();                        // @ 0x7100097930
    void DrawMenuList();                        // @ 0x7100099450 (via 0x7100099430)
    void DrawLaunchTransition();                // @ 0x7100099c40
    void DrawPauseMenu(bool closing);           // @ 0x710009a560
    void DrawPauseOptions();                    // @ 0x710009a210
    void PresentGB(Vec4 tint);                  // gb_present_frame @ 0x7100097520
    void PresentGBA();                          // gba_present_frame @ 0x71000977f0
};
