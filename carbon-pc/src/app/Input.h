// Input — Carbon's controller state (object 0x6c bytes @ 0x712aba9298).
// Original: Init @ 0x710009fe10, Update @ 0x710009fe60, npad->Carbon mapping @ 0x710009fff0,
// npad polling @ 0x71000a0190 (merges No.1 + Handheld, JoyDual/FullKey/Handheld styles).
#pragma once
#include <cstdint>

// Carbon button bits (what the menus and the GB/GBA key mappers test)
enum CarbonButton : uint32_t {
    CB_LEFT = 0x1, CB_RIGHT = 0x2, CB_DOWN = 0x4, CB_UP = 0x8,
    CB_SHOULDER4 = 0x10,  // set by ZL and ZR
    CB_R = 0x20,          // R, also set by ZR
    CB_L = 0x40,          // L, also set by ZL
    CB_A = 0x100, CB_B = 0x200, CB_X = 0x400, CB_Y = 0x800,
    CB_PLUS = 0x1000,     // Minus is not mapped at all
    CB_RSTICK_LEFT = 0x10000, CB_RSTICK_RIGHT = 0x20000, CB_RSTICK_DOWN = 0x40000, CB_RSTICK_UP = 0x80000,
    CB_LSTICK_LEFT = 0x100000, CB_LSTICK_RIGHT = 0x200000, CB_LSTICK_DOWN = 0x400000, CB_LSTICK_UP = 0x800000,
};
constexpr uint32_t CB_ANY_UP = CB_UP | CB_LSTICK_UP;           // 0x800008
constexpr uint32_t CB_ANY_DOWN = CB_DOWN | CB_LSTICK_DOWN;     // 0x400004
constexpr uint32_t CB_ANY_RIGHT = CB_RIGHT | CB_LSTICK_RIGHT;  // 0x200002
constexpr uint32_t CB_ANY_LEFT = CB_LEFT | CB_LSTICK_LEFT;     // 0x100001

// Raw controller snapshot supplied by the platform layer, in nn::hid::NpadButton bit order
// (A=0 B=1 X=2 Y=3 StickL=4 StickR=5 L=6 R=7 ZL=8 ZR=9 Plus=10 Minus=11 Left=12 Up=13 Right=14 Down=15),
// sticks in the -32767..32767 range.
struct NpadSnapshot {
    bool connected = false;
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
};

struct Input {
    uint32_t npadIds[2] = {0, 0x20};  // [0],[1]: No.1 and Handheld
    uint8_t initialized = 0;          // +0x08
    uint32_t held = 0;                // [3]  +0x0c
    uint32_t prevHeld = 0;            // [4]  +0x10
    int8_t sticks[4] = {};            // [5]  +0x14 LX, +0x15 LY, +0x16 RX, +0x17 RY (high byte of each axis)
    int8_t prevSticks[4] = {};        // [6]  +0x18
    float stickAccum[4] = {};         // [7]..[10]
    uint32_t pressed = 0;             // [0xb] +0x2c
    uint32_t released = 0;            // [0xc] +0x30

    void Init();
    void Update(const NpadSnapshot& pad);
};
