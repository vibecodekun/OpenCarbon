// Windows entry point: maps the Switch mounts to host directories, then runs nnMain's equivalent.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#undef CreateDirectory

#include "app/App.h"
#include "platform/Paths.h"
#include "platform/win32/Win32Platform.h"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

// Crash report: exception code and faulting module offset (enough to find the function in the .map/.pdb).
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    HMODULE module = nullptr;
    char name[MAX_PATH] = "?";
    const auto addr = static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress));
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &module))
        GetModuleFileNameA(module, name, sizeof(name));
    std::fprintf(stderr, "crash: exception 0x%08lx at %s+0x%llx (thread %lu)\n", rec->ExceptionCode, name,
                 static_cast<unsigned long long>(addr - reinterpret_cast<uintptr_t>(module)), GetCurrentThreadId());
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
        std::fprintf(stderr, "       %s address 0x%llx\n", rec->ExceptionInformation[0] ? "writing" : "reading",
                     static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    std::fflush(stderr);
    return EXCEPTION_EXECUTE_HANDLER;
}

void PrintUsage() {
    std::printf(
        "usage: carbon [options]\n"
        "  --romfs <dir>        extracted RomFS containing gamedef.xml (default: romfs)\n"
        "  --save <dir>         save data directory (default: save)\n"
        "  --html <dir>         html-document directory for the credits page (default: html-document)\n"
        "  --fullscreen         start in borderless fullscreen (F11 or Alt+Enter toggles)\n"
        "  --gl-debug           OpenGL debug context, log driver messages\n"
        "  --skip-movie         don't play the WayForward logo movie at startup (the Switch always plays it)\n"
        "test automation:\n"
        "  --script <file>      scripted input and captures (see src/platform/win32/Script.h)\n"
        "  --shots <dir>        where script captures go (default: shots)\n"
        "  --hidden             don't show the window\n"
        "  --mute               don't open audio devices\n"
        "  --audio-dump <dir>   also write every audio stream to a WAV file\n"
        "\n"
        "keyboard: arrows = D-pad, X = A, Z = B, S = X, A = Y, Q/W = L/R, E/R = ZL/ZR, Enter = +, Backspace = -,\n"
        "          Esc = ZL+ZR (pause menu), IJKL = left stick, U/O = right stick X (artwork zoom)\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // logs show up immediately when redirected to a file
    SetUnhandledExceptionFilter(CrashFilter);
    std::string romfs = "romfs";
    std::string save = "save";
    Platform::Win32Options options;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--romfs" && hasValue) {
            romfs = argv[++i];
        } else if (arg == "--save" && hasValue) {
            save = argv[++i];
        } else if (arg == "--html" && hasValue) {
            options.htmlRoot = argv[++i];
        } else if (arg == "--fullscreen") {
            options.fullscreen = true;
        } else if (arg == "--gl-debug") {
            options.glDebug = true;
        } else if (arg == "--skip-movie") {
            options.skipMovie = true;
        } else if (arg == "--script" && hasValue) {
            options.scriptPath = argv[++i];
        } else if (arg == "--shots" && hasValue) {
            options.shotDir = argv[++i];
        } else if (arg == "--hidden") {
            options.hidden = true;
        } else if (arg == "--mute") {
            options.mute = true;
        } else if (arg == "--audio-dump" && hasValue) {
            options.audioDumpDir = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", arg.c_str());
            PrintUsage();
            return 2;
        }
    }

    Paths::SetRomRoot(romfs);
    Paths::SetSaveRoot(save);
    Platform::SetWin32Options(options);
    const int result = CarbonMain();
    // The menu audio thread never stops (as on the Switch), so skip static destructors it may still be using.
    std::fflush(nullptr);
    std::quick_exit(result);
}
