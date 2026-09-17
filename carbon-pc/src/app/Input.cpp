#include "app/Input.h"

void Input::Init() {
    // nn::hid::InitializeNpad(); SetSupportedNpadStyleSet(FullKey|Handheld|JoyDual); ids {No1, Handheld}
    npadIds[0] = 0;
    npadIds[1] = 0x20;
    initialized = 1;
}

namespace {

// @ 0x710009fff0: nn::hid::NpadButton -> Carbon bits
uint32_t MapNpadButtons(uint64_t b) {
    uint32_t out = 0;
    out |= static_cast<uint32_t>(b & 0x40);                 // L  -> 0x40
    out |= (b & 0x1) ? CB_A : 0;                            // A  -> 0x100
    out |= (b & 0x2) ? CB_B : 0;                            // B  -> 0x200
    out |= (b & 0x4) ? CB_X : 0;                            // X  -> 0x400
    out |= (b & 0x8) ? CB_Y : 0;                            // Y  -> 0x800
    out |= (b & 0x400) ? CB_PLUS : 0;                       // +  -> 0x1000
    out |= (b >> 12) & 1;                                   // Left  -> 0x1
    out |= static_cast<uint32_t>((b >> 10) & 8);            // Up    -> 0x8
    out |= static_cast<uint32_t>((b >> 13) & 2);            // Right -> 0x2
    out |= static_cast<uint32_t>((b >> 13) & 4);            // Down  -> 0x4
    out |= static_cast<uint32_t>((b >> 2) & 0x20);          // R     -> 0x20
    if (b & 0x200) out |= 0x30;                             // ZR -> 0x30
    if (b & 0x100) out |= 0x50;                             // ZL -> 0x50
    return out;
}

}  // namespace

void Input::Update(const NpadSnapshot& pad) {
    // (the original first pumps the nns ControllerManager and shows the controller applet if nothing is connected)
    uint32_t buttons = 0;
    int8_t newSticks[4] = {};
    if (initialized && pad.connected) {
        buttons = MapNpadButtons(pad.buttons);
        newSticks[0] = static_cast<int8_t>(pad.lx >> 8);
        newSticks[1] = static_cast<int8_t>(pad.ly >> 8);
        newSticks[2] = static_cast<int8_t>(pad.rx >> 8);
        newSticks[3] = static_cast<int8_t>(pad.ry >> 8);
    }

    // Stick "d-pad" bits are computed from the PREVIOUS frame's stick bytes (one-frame lag, as in the original).
    uint32_t bits = buttons;
    bits |= (sticks[0] < -32) ? CB_LSTICK_LEFT : 0;
    bits |= (sticks[0] > 32) ? CB_LSTICK_RIGHT : 0;
    bits |= (sticks[1] < -32) ? CB_LSTICK_DOWN : 0;
    bits |= (sticks[1] > 32) ? CB_LSTICK_UP : 0;
    bits |= (sticks[2] < -32) ? CB_RSTICK_LEFT : 0;
    bits |= (sticks[2] > 32) ? CB_RSTICK_RIGHT : 0;
    bits |= (sticks[3] < -32) ? CB_RSTICK_DOWN : 0;
    bits |= (sticks[3] > 32) ? CB_RSTICK_UP : 0;

    uint32_t oldHeld = held;
    for (int i = 0; i < 4; ++i) prevSticks[i] = sticks[i];
    held = bits;
    prevHeld = oldHeld;
    pressed = (bits ^ oldHeld) & bits;
    released = (bits ^ oldHeld) & oldHeld;
    for (int i = 0; i < 4; ++i) {
        stickAccum[i] += static_cast<float>(static_cast<int>(newSticks[i]) - static_cast<int>(sticks[i])) * 0.017857144f;
        sticks[i] = newSticks[i];
    }
}
