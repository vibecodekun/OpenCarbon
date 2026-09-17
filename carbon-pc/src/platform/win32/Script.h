// Frame-scripted input and canvas captures for unattended test runs (--script).
//
// One command per line, '#' starts a comment. A frame is one presented frame (Platform::SwapBuffers call,
// splash frames included). "+N" is relative to the previous command's frame. The startup movie comes before
// frame 0 and isn't counted; every 30th movie frame is captured as movie_NNN.png.
//   <frame> hold <frames> <BUTTON>[+<BUTTON>...]    A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK
//   <frame> stick <frames> <lx> <ly> <rx> <ry>     -32767..32767
//   <frame> shot <name>                            <shot dir>/<name>.png of the 1280x720 canvas
//   <frame> log <text...>                          prints a marker line to stdout
//   <frame> quit                                   window-close request
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct NpadSnapshot;

namespace script {

struct Event {
    enum class Kind { Hold, Stick, Shot, Log, Quit };
    Kind kind = Kind::Hold;
    int64_t frame = 0;
    int64_t frames = 0;
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    std::string text;
};

bool Load(const std::string& path, std::string& error);
bool Active();
// Scripted controller state for the given frame (replaces real input while a script is loaded).
void PadState(int64_t frame, NpadSnapshot& out);
// Events that fire when `frame` is presented, in file order.
std::vector<const Event*> EventsAt(int64_t frame);

}  // namespace script
