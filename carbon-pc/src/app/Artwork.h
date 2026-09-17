// Artwork gallery (extras). Original: LoadTextures @ 0x71000a05d0, Select @ 0x71000a0ca0, Draw @ 0x71000a0e00.
#pragma once

namespace artwork {

extern int index;          // 0x712aba3578 (0..15)
extern int uiTimer;        // 0x712aba357c
extern int uiVisible;      // 0x71001b29fc (init 1)

void LoadTextures();       // artwork0..15 + artworkUI
void Select();             // fit current image to 1280 wide, centre it
void Draw();               // clears, draws, glFinish + swap (bypasses Renderer Begin/EndFrame)

}  // namespace artwork
