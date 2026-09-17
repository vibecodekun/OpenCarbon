// UI/app globals. In the original these are loose globals in .data/.bss; the comment gives each address.
#pragma once
#include "gfx/SpriteRenderer.h"
#include "gfx/TextRenderer.h"
#include <cstdint>

class GameDef;
struct Input;
struct GbSystem;
class Renderer;
class App;

namespace g {

// --- object pointers ---------------------------------------------------------------------------
extern App* app;                         // 0x712aba3570
extern GameDef* gameDef;                 // 0x712abac000
extern Input* input;                     // 0x712aba9298
extern GbSystem* gbSystem;               // 0x712abac010
extern uint32_t* gbaKeys;                // 0x712abac018 (GBA system object: first field = key bits)

// --- pause sub-state (0x712abac008: new int[2]) -------------------------------------------------
// [0]: 0 = option list, 1 = load-slot select, 2 = save-slot select, 3 = CONFIRM LOAD, 4 = CONFIRM SAVE
// [1]: selected slot (0..2)
extern int pauseSub[2];

extern bool transitionActive;            // 0x712abac028
extern bool pauseClosing;                // 0x712abac029
extern bool paused;                      // 0x712abac02a (suppresses the "pause menu" hint)
extern Vec4 gameTint;                    // 0x712abac02c..0x712abac03b (dims the game behind the pause menu)

// --- renderers ---------------------------------------------------------------------------------
extern SpriteRenderer* spriteRenderer;      // 0x7100256ce8  shader "sprite"
extern SpriteRenderer* lcdRenderer;         // 0x7100256cf0  shader "lcd"
extern SpriteRenderer* emulationRenderer;   // 0x7100256d20  shader "emulation" (particles.fs)
extern SpriteRenderer* transitionRenderer;  // 0x7100256d28  shader "transition" (fade.fs)
extern TextRenderer font;                   // 0x7100256d30
extern TextRenderer fontSmall;              // 0x7100256da8

// --- menu animation ----------------------------------------------------------------------------
extern Vec4 menuFade;                    // 0x7100256cf8..d04
extern int menuAnimState;                // 0x7100256d08
extern float menuFadeInRGB[3];           // 0x7100256d0c..d14
extern float shaderTime;                 // 0x7100256d18 (+0.02 per menu frame, "time" uniform)
extern int menuSpriteX;                  // 0x71001b2980 (init 1800)
extern int hue;                          // 0x71001b2984 (init 40), index into kHueTable
extern int field_71001b2988;             // 0x71001b2988 (init 1, zeroed by the menu list)
extern uint32_t rainbowColor;            // 0x7100256e20
extern Vec4 pauseFade;                   // 0x7100256e30..3c
extern Vec4 hintFade;                    // 0x7100256e40..4c
extern float menuScroll;                 // 0x7100256e50 (+0.6 up to 640, never drawn)
extern Vec4 hintColor;                   // 0x7100256e60..6c (init 1,1,1,1)

// --- menu sound triggers (read by the menu audio thread) ---------------------------------------
extern int sfxBack;                      // 0x71223a2c00 -> sndchange.wav
extern int sfxSelect;                    // 0x71223a2c04 -> sndselect.wav
extern int sfxMove;                      // 0x71223a2c08 -> menu_scrub_through_items3.wav (vol 0.6)
extern int sfxToggle;                    // 0x71223a2c0c -> menu_toggle_yes_no3.wav
extern int musicRestart;                 // 0x71223a2bf8
extern float musicFadeInTarget;          // 0x712aba3000 (1.0, set to 0.7 when returning to the menu)
extern float musicFadeOutTarget;         // 0x712aba3004 (0.0)
extern int musicFadeOut;                 // 0x712aba3008
extern int musicFadeIn;                  // 0x712aba300c

// 360-entry 0..255 ramp table @ 0x7100194bb4 (129 zeros)
extern const uint8_t kHueTable[360];

}  // namespace g
