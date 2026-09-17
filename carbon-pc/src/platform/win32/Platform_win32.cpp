// Win32 implementation of platform/Platform.h.
//  - nn::vi + EGL (desktop GL 4.5 core)  -> Win32 window + WGL 4.5 core context. The app draws into a 1280x720
//                                          offscreen canvas that is letterboxed into the window on every swap,
//                                          so gl_FragCoord-based shaders (particles.fs) see the same resolution.
//  - eglSwapInterval(1) on a 60 Hz panel  -> 60 Hz pacing on any monitor (vsync when the refresh rate is a
//                                          multiple of 60, a timer otherwise). The GB core runs one frame per
//                                          swap, so this sets emulation speed as well.
//  - nn::hid Npad (No.1 + Handheld)       -> keyboard + first XInput controller (positional face buttons)
//  - nn::oe exit notification             -> window close
//  - nn::web offline applet               -> WebView2 inside the window (OfflineHtml_win32.cpp)
//  - wf_logo.mp4 movie during EGL init    -> Media Foundation into the canvas (MoviePlayer_win32.cpp)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <xinput.h>
#undef CreateDirectory  // keep Platform::CreateDirectory's name

#include "platform/Platform.h"
#include "app/Input.h"
#include "platform/Paths.h"
#include "platform/gl.h"
#include "platform/win32/MoviePlayer.h"
#include "platform/win32/OfflineHtml.h"
#include "platform/win32/Script.h"
#include "platform/win32/Win32Platform.h"
#include "stb_image_write.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace Platform {
namespace {

constexpr int kCanvasW = 1280;
constexpr int kCanvasH = 720;

// nn::hid::NpadButton bits (see NpadSnapshot)
constexpr uint64_t NPAD_A = 1u << 0, NPAD_B = 1u << 1, NPAD_X = 1u << 2, NPAD_Y = 1u << 3;
constexpr uint64_t NPAD_STICK_L = 1u << 4, NPAD_STICK_R = 1u << 5;
constexpr uint64_t NPAD_L = 1u << 6, NPAD_R = 1u << 7, NPAD_ZL = 1u << 8, NPAD_ZR = 1u << 9;
constexpr uint64_t NPAD_PLUS = 1u << 10, NPAD_MINUS = 1u << 11;
constexpr uint64_t NPAD_LEFT = 1u << 12, NPAD_UP = 1u << 13, NPAD_RIGHT = 1u << 14, NPAD_DOWN = 1u << 15;

struct KeyBinding { int vk; uint64_t npad; };
const KeyBinding kKeyBindings[] = {
    {VK_LEFT, NPAD_LEFT}, {VK_UP, NPAD_UP}, {VK_RIGHT, NPAD_RIGHT}, {VK_DOWN, NPAD_DOWN},
    {'X', NPAD_A}, {'Z', NPAD_B}, {'S', NPAD_X}, {'A', NPAD_Y},
    {'Q', NPAD_L}, {'W', NPAD_R}, {'E', NPAD_ZL}, {'R', NPAD_ZR},
    {VK_RETURN, NPAD_PLUS}, {VK_BACK, NPAD_MINUS},
    {VK_ESCAPE, NPAD_ZL | NPAD_ZR},  // the in-game pause combo on one key
};

struct PadBinding { WORD xinput; uint64_t npad; };
const PadBinding kPadBindings[] = {
    // positional: the Xbox bottom button is where the Switch B button is
    {XINPUT_GAMEPAD_B, NPAD_A}, {XINPUT_GAMEPAD_A, NPAD_B}, {XINPUT_GAMEPAD_Y, NPAD_X}, {XINPUT_GAMEPAD_X, NPAD_Y},
    {XINPUT_GAMEPAD_LEFT_THUMB, NPAD_STICK_L}, {XINPUT_GAMEPAD_RIGHT_THUMB, NPAD_STICK_R},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, NPAD_L}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, NPAD_R},
    {XINPUT_GAMEPAD_START, NPAD_PLUS}, {XINPUT_GAMEPAD_BACK, NPAD_MINUS},
    {XINPUT_GAMEPAD_DPAD_LEFT, NPAD_LEFT}, {XINPUT_GAMEPAD_DPAD_UP, NPAD_UP},
    {XINPUT_GAMEPAD_DPAD_RIGHT, NPAD_RIGHT}, {XINPUT_GAMEPAD_DPAD_DOWN, NPAD_DOWN},
};

Win32Options g_options;
HWND g_hwnd = nullptr;
HDC g_hdc = nullptr;
HGLRC g_glrc = nullptr;
int g_clientW = kCanvasW;
int g_clientH = kCanvasH;
bool g_exitRequested = false;
bool g_keys[256] = {};
bool g_fullscreen = false;
WINDOWPLACEMENT g_windowedPlacement = {sizeof(WINDOWPLACEMENT)};
GLuint g_canvasFbo = 0, g_canvasColor = 0, g_canvasDepth = 0;
int g_padIndex = -1;
int64_t g_nextPadScanNs = 0;
int64_t g_frame = 0;  // presented frames, for --script

struct Pacer {
    static constexpr int64_t kPeriodNs = 1'000'000'000 / 60;
    int64_t next = 0;  // timer mode: deadline of the next frame; vsync mode: earliest plausible next swap
    bool timer = false;
} g_pacer;

[[noreturn]] void Fatal(const char* message) {
    std::fprintf(stderr, "fatal: %s\n", message);
    MessageBoxA(g_hwnd, message, "Carbon", MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

void PaceFrame() {
    const int64_t now = MonotonicNs();
    if (g_pacer.timer) {
        if (g_pacer.next == 0 || now - g_pacer.next > 2 * Pacer::kPeriodNs) g_pacer.next = now;  // start / fell behind
        SleepUntilNs(g_pacer.next);
        g_pacer.next += Pacer::kPeriodNs;
    } else {
        // vsync already paced this swap unless it returned implausibly early (vsync forced off, minimized or
        // hidden window)
        if (g_pacer.next != 0 && now < g_pacer.next - Pacer::kPeriodNs / 8) SleepUntilNs(g_pacer.next);
        g_pacer.next = MonotonicNs() + Pacer::kPeriodNs;
    }
}

void RunScriptEvents() {
    for (const script::Event* e : script::EventsAt(g_frame)) {
        switch (e->kind) {
        case script::Event::Kind::Shot:
            WriteShot(e->text, ReadCanvas());
            break;
        case script::Event::Kind::Log:
            std::printf("[script] frame %lld t=%.3f: %s\n", static_cast<long long>(g_frame), MonotonicNs() / 1e9,
                        e->text.c_str());
            break;
        case script::Event::Kind::Quit:
            std::printf("[script] frame %lld: quit\n", static_cast<long long>(g_frame));
            g_exitRequested = true;
            break;
        default:
            break;
        }
    }
}

// The 1280x720 canvas scaled to fit the client area, centred (client coordinates, top-left origin).
struct CanvasRect {
    int x, y, w, h;
};

CanvasRect LetterboxedCanvas() {
    const int cw = std::max(g_clientW, 0), ch = std::max(g_clientH, 0);
    int w = cw;
    int h = static_cast<int>(static_cast<int64_t>(cw) * kCanvasH / kCanvasW);
    if (h > ch) {
        h = ch;
        w = static_cast<int>(static_cast<int64_t>(ch) * kCanvasW / kCanvasH);
    }
    return {(cw - w) / 2, (ch - h) / 2, w, h};
}

void PumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

void ToggleFullscreen() {
    const LONG style = GetWindowLongW(g_hwnd, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO mi = {sizeof(mi)};
        if (!GetWindowPlacement(g_hwnd, &g_windowedPlacement) ||
            !GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi))
            return;
        SetWindowLongW(g_hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(g_hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    } else {
        SetWindowLongW(g_hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_hwnd, &g_windowedPlacement);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CLOSE:
        g_exitRequested = true;  // App::Run closes the game and returns
        return 0;
    case WM_SIZE:
        g_clientW = LOWORD(lp);
        g_clientH = HIWORD(lp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_KILLFOCUS:
        std::memset(g_keys, 0, sizeof(g_keys));
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (wp == VK_F11 || (wp == VK_RETURN && (HIWORD(lp) & KF_ALTDOWN))) {
            if (!(HIWORD(lp) & KF_REPEAT)) ToggleFullscreen();
            return 0;
        }
        if (wp < 256) g_keys[wp] = true;
        if (msg == WM_SYSKEYDOWN && wp != VK_F4) return 0;  // no menu-bar activation; keep Alt+F4
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (wp < 256) g_keys[wp] = false;
        if (msg == WM_SYSKEYUP) return 0;
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void GLAD_API_PTR GlDebugCallback(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei, const GLchar* message,
                                  const void*) {
    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) return;
    if (type == GL_DEBUG_TYPE_PERFORMANCE && severity != GL_DEBUG_SEVERITY_HIGH) return;  // e.g. capture readback
    std::fprintf(stderr, "[gl] source=0x%x type=0x%x id=%u severity=0x%x: %s\n", source, type, id, severity, message);
}

void SetupPacing() {
    MONITORINFOEXW mi;
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    int hz = 0;
    if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm))
        hz = static_cast<int>(dm.dmDisplayFrequency);

    // 59/60, 119/120, 239/240 ... Hz: swap every (hz/60) refreshes. Anything else: no vsync, timer at 60 Hz.
    const int multiple = (hz + 1) / 60;
    int interval = 0;
    if (multiple >= 1 && std::abs(hz - multiple * 60) <= 1) interval = multiple;

    using SwapIntervalFn = BOOL(WINAPI*)(int);
    const auto swapInterval = reinterpret_cast<SwapIntervalFn>(wglGetProcAddress("wglSwapIntervalEXT"));
    if (interval > 0 && swapInterval && swapInterval(interval)) {
        g_pacer.timer = false;
        std::printf("display %d Hz: vsync, swap interval %d\n", hz, interval);
    } else {
        if (swapInterval) swapInterval(0);
        g_pacer.timer = true;
        std::printf("display %d Hz: vsync off, 60 Hz timer pacing\n", hz);
    }
}

void CreateCanvas() {
    glGenRenderbuffers(1, &g_canvasColor);
    glBindRenderbuffer(GL_RENDERBUFFER, g_canvasColor);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, kCanvasW, kCanvasH);
    glGenRenderbuffers(1, &g_canvasDepth);
    glBindRenderbuffer(GL_RENDERBUFFER, g_canvasDepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, kCanvasW, kCanvasH);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glGenFramebuffers(1, &g_canvasFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, g_canvasFbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, g_canvasColor);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, g_canvasDepth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) Fatal("canvas framebuffer is incomplete");
    glViewport(0, 0, kCanvasW, kCanvasH);
}

bool ReadPad(XINPUT_STATE& state) {
    if (g_padIndex >= 0) {
        if (XInputGetState(static_cast<DWORD>(g_padIndex), &state) == ERROR_SUCCESS) return true;
        g_padIndex = -1;
    }
    // XInputGetState on empty slots is slow, so only rescan once a second
    const int64_t now = MonotonicNs();
    if (now < g_nextPadScanNs) return false;
    g_nextPadScanNs = now + 1'000'000'000;
    for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
        if (XInputGetState(i, &state) == ERROR_SUCCESS) {
            g_padIndex = static_cast<int>(i);
            std::printf("XInput controller %lu connected\n", i);
            return true;
        }
    }
    return false;
}

int32_t ClampAxis(int32_t v) { return std::clamp(v, -32767, 32767); }

}  // namespace

void SetWin32Options(const Win32Options& options) {
    g_options = options;
    if (!g_options.scriptPath.empty()) {
        std::string error;
        if (!script::Load(g_options.scriptPath, error)) Fatal(("script: " + error).c_str());
    }
}

const Win32Options& GetWin32Options() { return g_options; }

void SleepUntilNs(int64_t deadlineNs) {
    thread_local const HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;) {
        const int64_t remainNs = deadlineNs - MonotonicNs();
        if (remainNs <= 0) return;
        if (remainNs > 2'000'000) {  // sleep, leaving ~1 ms to spin off
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -((remainNs - 1'000'000) / 100);
                SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
        } else {
            Sleep(0);
        }
    }
}

bool MountSaveAndRom() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const std::string gamedef = Paths::Resolve("rom:/gamedef.xml");
    if (!fs::is_regular_file(gamedef, ec)) {
        const std::string msg = "RomFS not found: " + gamedef + " is missing.\nPass the extracted romfs with --romfs <dir>.";
        std::fprintf(stderr, "%s\n", msg.c_str());
        MessageBoxA(nullptr, msg.c_str(), "Carbon", MB_ICONERROR | MB_OK);
        return false;
    }
    const std::string saveRoot = Paths::Resolve("save:/");
    fs::create_directories(saveRoot, ec);
    if (!fs::is_directory(saveRoot, ec)) {
        std::fprintf(stderr, "cannot create save directory %s\n", saveRoot.c_str());
        return false;
    }
    std::printf("rom:/  -> %s\nsave:/ -> %s\n", fs::absolute(Paths::Resolve("rom:/"), ec).string().c_str(),
                fs::absolute(saveRoot, ec).string().c_str());
    return true;
}

void InitGraphics() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"CarbonWindow";
    if (!RegisterClassExW(&wc)) Fatal("RegisterClassEx failed");

    RECT rect = {0, 0, kCanvasW, kCanvasH};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Carbon-Gb", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                             rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
    if (!g_hwnd) Fatal("CreateWindowEx failed");
    g_hdc = GetDC(g_hwnd);

    PIXELFORMATDESCRIPTOR pfd = {sizeof(pfd), 1};
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int format = ChoosePixelFormat(g_hdc, &pfd);
    if (format == 0 || !SetPixelFormat(g_hdc, format, &pfd)) Fatal("SetPixelFormat failed");

    // a legacy context is needed to reach wglCreateContextAttribsARB
    const HGLRC legacy = wglCreateContext(g_hdc);
    if (!legacy || !wglMakeCurrent(g_hdc, legacy)) Fatal("wglCreateContext failed");
    using CreateContextAttribsFn = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
    const auto createContextAttribs =
        reinterpret_cast<CreateContextAttribsFn>(wglGetProcAddress("wglCreateContextAttribsARB"));
    if (!createContextAttribs) Fatal("WGL_ARB_create_context is not supported");

    constexpr int WGL_CONTEXT_MAJOR_VERSION_ARB = 0x2091, WGL_CONTEXT_MINOR_VERSION_ARB = 0x2092,
                  WGL_CONTEXT_FLAGS_ARB = 0x2094, WGL_CONTEXT_PROFILE_MASK_ARB = 0x9126,
                  WGL_CONTEXT_CORE_PROFILE_BIT_ARB = 0x1, WGL_CONTEXT_DEBUG_BIT_ARB = 0x1;
    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 5,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        WGL_CONTEXT_FLAGS_ARB, g_options.glDebug ? WGL_CONTEXT_DEBUG_BIT_ARB : 0,
        0,
    };
    g_glrc = createContextAttribs(g_hdc, nullptr, attribs);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(legacy);
    if (!g_glrc || !wglMakeCurrent(g_hdc, g_glrc)) Fatal("could not create an OpenGL 4.5 core context");
    if (!gladLoaderLoadGL()) Fatal("gladLoaderLoadGL failed");
    std::printf("GL %s | %s\n", reinterpret_cast<const char*>(glGetString(GL_VERSION)),
                reinterpret_cast<const char*>(glGetString(GL_RENDERER)));

    if (g_options.glDebug) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(GlDebugCallback, nullptr);
    }

    CreateCanvas();
    SetupPacing();

    if (!g_options.hidden) {
        ShowWindow(g_hwnd, SW_SHOWNORMAL);
        if (g_options.fullscreen) ToggleFullscreen();
    }
    PumpMessages();

    // The original plays it before creating its own layer and context; here the window has to exist first.
    if (!g_options.skipMovie) movie_player::Play("rom:/assets/splash/wf_logo.mp4");
}

void SwapBuffers() {
    PumpMessages();
    if (script::Active()) RunScriptEvents();
    ++g_frame;
    PresentCanvas();
}

void PresentCanvas() {
    const int cw = g_clientW, ch = g_clientH;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_canvasFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    if (cw > 0 && ch > 0 && !IsIconic(g_hwnd)) {
        const CanvasRect r = LetterboxedCanvas();
        // window rows are bottom-up in GL
        const int y = ch - r.y - r.h;
        glViewport(0, 0, cw, ch);
        glClear(GL_COLOR_BUFFER_BIT);
        glBlitFramebuffer(0, 0, kCanvasW, kCanvasH, r.x, y, r.x + r.w, y + r.h, GL_COLOR_BUFFER_BIT,
                          (r.w == kCanvasW && r.h == kCanvasH) ? GL_NEAREST : GL_LINEAR);
    }
    ::SwapBuffers(g_hdc);
    glBindFramebuffer(GL_FRAMEBUFFER, g_canvasFbo);
    glViewport(0, 0, kCanvasW, kCanvasH);
    PaceFrame();
}

void PumpWindowMessages() { PumpMessages(); }

bool WindowCloseRequested() { return g_exitRequested; }

std::vector<unsigned char> ReadCanvas() {
    std::vector<unsigned char> pixels(static_cast<size_t>(kCanvasW) * kCanvasH * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_canvasFbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, kCanvasW, kCanvasH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;  // blended alpha isn't meaningful on screen
    // GL rows are bottom-up. Flip here rather than with stbi_flip_vertically_on_write, which is a global setting
    // and would also flip the save-state previews the GB core writes.
    std::vector<unsigned char> flipped(pixels.size());
    for (int y = 0; y < kCanvasH; ++y)
        std::memcpy(&flipped[static_cast<size_t>(y) * kCanvasW * 4], &pixels[static_cast<size_t>(kCanvasH - 1 - y) * kCanvasW * 4], kCanvasW * 4);
    return flipped;
}

void WriteShot(const std::string& name, const std::vector<unsigned char>& rgba) {
    std::error_code ec;
    std::filesystem::create_directories(g_options.shotDir, ec);
    const std::string path = g_options.shotDir + "/" + name + ".png";
    if (stbi_write_png(path.c_str(), kCanvasW, kCanvasH, 4, rgba.data(), kCanvasW * 4))
        std::printf("[script] frame %lld: shot %s\n", static_cast<long long>(g_frame), path.c_str());
    else
        std::fprintf(stderr, "[script] frame %lld: could not write %s\n", static_cast<long long>(g_frame), path.c_str());
}

void PollNpad(NpadSnapshot& out) {
    PumpMessages();
    if (script::Active()) {
        script::PadState(g_frame, out);
        return;
    }
    out = NpadSnapshot{};
    out.connected = true;  // keyboard is always there

    for (const KeyBinding& k : kKeyBindings)
        if (g_keys[k.vk]) out.buttons |= k.npad;
    // left stick on IJKL, right stick X on U/O (artwork zoom)
    if (g_keys['J']) out.lx = -32767;
    if (g_keys['L']) out.lx = 32767;
    if (g_keys['K']) out.ly = -32767;
    if (g_keys['I']) out.ly = 32767;
    if (g_keys['U']) out.rx = -32767;
    if (g_keys['O']) out.rx = 32767;

    XINPUT_STATE state;
    if (ReadPad(state)) {
        const XINPUT_GAMEPAD& gp = state.Gamepad;
        for (const PadBinding& b : kPadBindings)
            if (gp.wButtons & b.xinput) out.buttons |= b.npad;
        if (gp.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out.buttons |= NPAD_ZL;
        if (gp.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) out.buttons |= NPAD_ZR;
        if (out.lx == 0) out.lx = ClampAxis(gp.sThumbLX);
        if (out.ly == 0) out.ly = ClampAxis(gp.sThumbLY);
        if (out.rx == 0) out.rx = ClampAxis(gp.sThumbRX);
        if (out.ry == 0) out.ry = ClampAxis(gp.sThumbRY);
    }
}

bool PopExitRequest() {
    PumpMessages();
    const bool requested = g_exitRequested;
    g_exitRequested = false;
    return requested;
}

// nn::web::ShowOfflineHtmlPage: plays the page inside the window and blocks until it closes (see OfflineHtml.h).
void ShowOfflineHtml(const char* url) {
    namespace fs = std::filesystem;
    std::string relative = url;
    if (size_t q = relative.find_first_of("?#"); q != std::string::npos) relative.resize(q);
    std::error_code ec;
    const fs::path root = fs::absolute(fs::path(g_options.htmlRoot), ec);
    if (!fs::is_regular_file(root / fs::path(relative), ec)) {
        std::fprintf(stderr, "offline html: %s not found (pass the html-document dir with --html <dir>)\n",
                     (root / fs::path(relative)).string().c_str());
        return;
    }

    offline_html::Request request;
    request.window = g_hwnd;
    request.documentRoot = root.string();
    request.url = relative;
    request.viewRect = [](long& x, long& y, long& w, long& h) {
        const CanvasRect r = LetterboxedCanvas();
        x = r.x, y = r.y, w = r.w, h = r.h;
    };
    request.exitRequested = [] { return g_exitRequested; };  // left set: App::Run handles the close afterwards
    if (script::Active()) {
        request.captureDir = g_options.shotDir;
        request.renderWhileHidden = g_options.hidden;
    }
    std::printf("offline html: %s/%s\n", request.documentRoot.c_str(), relative.c_str());
    const int64_t startNs = MonotonicNs();
    const offline_html::ExitReason reason = offline_html::Show(request);
    std::printf("offline html: closed after %.1f s (%s)\n", (MonotonicNs() - startNs) / 1e9, offline_html::ToString(reason));
    if (reason == offline_html::ExitReason::Unavailable)
        std::fprintf(stderr, "offline html: the Microsoft Edge WebView2 runtime is required to show this page\n");
}

int64_t MonotonicNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    const int64_t t = QpcNow();
    return (t / freq) * 1'000'000'000 + (t % freq) * 1'000'000'000 / freq;
}

void CreateDirectory(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(Paths::Resolve(path), ec);
}

void Commit(const char*) {
    // host files are written directly; nothing to commit
}

}  // namespace Platform
