// Windows-only settings for the platform layer (set from the command line before CarbonMain).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace Platform {

struct Win32Options {
    std::string htmlRoot = "html-document";  // offline web applet documents (credits.htdocs/...)
    bool fullscreen = false;                 // start in borderless fullscreen (F11 / Alt+Enter toggle)
    bool glDebug = false;                    // debug context + KHR_debug message log
    bool skipMovie = false;                  // don't play wf_logo.mp4 at startup (the Switch always does)

    // test automation
    std::string scriptPath;                  // scripted input / captures, see Script.h
    std::string shotDir = "shots";           // where script "shot" PNGs go
    bool hidden = false;                     // never show the window
    bool mute = false;                       // don't open audio devices (streams still run in real time)
    std::string audioDumpDir;                // write every AudioStream to <dir>/audio<N>_<rate>hz.wav
};

void SetWin32Options(const Win32Options& options);
const Win32Options& GetWin32Options();

// Sleeps the calling thread until MonotonicNs() >= deadlineNs (high-resolution waitable timer + short spin).
void SleepUntilNs(int64_t deadlineNs);

// For drawing outside the app's frame loop (the startup movie). PresentCanvas letterboxes, swaps and paces like
// SwapBuffers, but isn't a --script frame.
void PresentCanvas();
void PumpWindowMessages();
bool WindowCloseRequested();                  // a pending close, left for App::Run to handle
std::vector<unsigned char> ReadCanvas();      // 1280x720 RGBA, top row first
void WriteShot(const std::string& name, const std::vector<unsigned char>& rgba);  // <shot dir>/<name>.png

}  // namespace Platform
