#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#pragma warning(push)
#pragma warning(disable : 4100 4458)
#include <wrl.h>
#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#pragma warning(pop)

#include "platform/win32/OfflineHtml.h"
#include <cstdio>
#include <filesystem>
#include <memory>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace offline_html {
namespace {

constexpr wchar_t kHostName[] = L"offline.carbon";  // serves the html-document folder (web fonts need a real origin)
constexpr wchar_t kCallbackUrl[] = L"http://localhost/";
constexpr double kPageWidth = 1280.0;               // the Switch applet's CSS viewport width

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring UserDataFolder() {
    PWSTR base = nullptr;
    std::wstring folder;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        folder = std::wstring(base) + L"\\opencarbon\\WebView2";
        CoTaskMemFree(base);
    }
    return folder;
}

LRESULT CALLBACK ContainerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_ERASEBKGND) {  // black while the page loads, like background kind 1
        RECT r;
        GetClientRect(hwnd, &r);
        FillRect(reinterpret_cast<HDC>(wp), &r, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// Shared with the asynchronous WebView2 callbacks, which can outlive Show() if the window closes during startup.
struct Session {
    HWND container = nullptr;
    std::wstring root;
    std::wstring target;
    RECT bounds{};
    bool cancelled = false;
    bool failed = false;
    bool done = false;
    ExitReason reason = ExitReason::CallbackUrl;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;

    void ApplyBounds(const RECT& r) {
        bounds = r;
        const long w = r.right - r.left, h = r.bottom - r.top;
        SetWindowPos(container, nullptr, r.left, r.top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (!controller) return;
        controller->put_Bounds(RECT{0, 0, w, h});
        double scale = 1.0;  // physical pixels per DIP
        if (ComPtr<ICoreWebView2Controller3> c3; SUCCEEDED(controller.As(&c3))) c3->get_RasterizationScale(&scale);
        if (w > 0) controller->put_ZoomFactor(w / (kPageWidth * scale));
    }

    void OnController(ICoreWebView2Controller* created) {
        controller = created;
        if (cancelled) {
            controller->Close();
            controller.Reset();
            return;
        }
        controller->get_CoreWebView2(&webview);

        ComPtr<ICoreWebView2Settings> settings;
        webview->get_Settings(&settings);
        settings->put_IsScriptEnabled(TRUE);  // JS extension enabled
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_AreDevToolsEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        if (ComPtr<ICoreWebView2Settings3> s3; SUCCEEDED(settings.As(&s3))) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        if (ComPtr<ICoreWebView2Settings6> s6; SUCCEEDED(settings.As(&s6))) s6->put_IsSwipeNavigationEnabled(FALSE);
        if (ComPtr<ICoreWebView2Controller2> c2; SUCCEEDED(controller.As(&c2)))
            c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{255, 0, 0, 0});
        if (ComPtr<ICoreWebView2_3> w3; SUCCEEDED(webview.As(&w3)))
            w3->SetVirtualHostNameToFolderMapping(kHostName, root.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);

        EventRegistrationToken token;
        webview->add_NavigationStarting(
            Callback<ICoreWebView2NavigationStartingEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) -> HRESULT {
                    LPWSTR uri = nullptr;
                    args->get_Uri(&uri);
                    const std::wstring u = uri ? uri : L"";
                    CoTaskMemFree(uri);
                    if (u.rfind(kCallbackUrl, 0) == 0) {  // navigating to the callback URL closes the applet
                        args->put_Cancel(TRUE);
                        reason = ExitReason::CallbackUrl;
                        done = true;
                    } else if (u.rfind(std::wstring(L"https://") + kHostName + L"/", 0) != 0) {
                        args->put_Cancel(TRUE);  // an offline page stays inside its document folder
                    }
                    return S_OK;
                }).Get(),
            &token);
        webview->add_NavigationCompleted(
            Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* args) -> HRESULT {
                    BOOL ok = FALSE;
                    args->get_IsSuccess(&ok);
                    COREWEBVIEW2_WEB_ERROR_STATUS status{};
                    args->get_WebErrorStatus(&status);
                    if (!ok && !done && status != COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) {
                        std::fprintf(stderr, "offline html: navigation failed (web error status %d)\n", static_cast<int>(status));
                        reason = ExitReason::LoadFailed;
                        done = true;
                    }
                    return S_OK;
                }).Get(),
            &token);
        webview->add_NewWindowRequested(
            Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) -> HRESULT {
                    args->put_Handled(TRUE);
                    return S_OK;
                }).Get(),
            &token);

        ApplyBounds(bounds);
        controller->put_IsVisible(TRUE);
        webview->Navigate(target.c_str());
    }
};

void Capture(const ComPtr<ICoreWebView2>& webview, const std::string& path) {
    ComPtr<IStream> stream;
    if (FAILED(SHCreateStreamOnFileEx(Widen(path).c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE,
                                      nullptr, &stream)))
        return;
    webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
                            Callback<ICoreWebView2CapturePreviewCompletedHandler>([stream, path](HRESULT hr) -> HRESULT {
                                std::printf("offline html: capture %s %s\n", path.c_str(), SUCCEEDED(hr) ? "written" : "failed");
                                return S_OK;
                            }).Get());
}

}  // namespace

const char* ToString(ExitReason reason) {
    switch (reason) {
    case ExitReason::CallbackUrl: return "callback url";
    case ExitReason::WindowClosed: return "window closed";
    case ExitReason::LoadFailed: return "load failed";
    default: return "WebView2 unavailable";
    }
}

ExitReason Show(const Request& req) {
    // WebView2 needs a single-threaded apartment on the UI thread.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com) && com != RPC_E_CHANGED_MODE) return ExitReason::Unavailable;

    static constexpr wchar_t kClass[] = L"CarbonOfflineHtml";
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = ContainerProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);  // fails harmlessly once registered

    auto session = std::make_shared<Session>();
    long x = 0, y = 0, w = 1280, h = 720;
    req.viewRect(x, y, w, h);
    session->bounds = RECT{x, y, x + w, y + h};
    session->root = Widen(req.documentRoot);
    session->target = std::wstring(L"https://") + kHostName + L"/" + Widen(req.url);
    session->container = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, x, y, w, h,
                                         reinterpret_cast<HWND>(req.window), nullptr, wc.hInstance, nullptr);
    EnableWindow(session->container, FALSE);  // pointer and touch disabled: the page never receives input

    auto options = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (req.renderWhileHidden)
        options->put_AdditionalBrowserArguments(L"--disable-features=CalculateNativeWinOcclusion "
                                                L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding");

    const HRESULT started = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, UserDataFolder().c_str(), options.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [session](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(hr) || !env || session->cancelled) {
                    session->failed = FAILED(hr) || !env;
                    return S_OK;
                }
                const HRESULT chr = env->CreateCoreWebView2Controller(
                    session->container,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [session](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                            if (FAILED(result) || !controller) session->failed = true;
                            else session->OnController(controller);
                            return S_OK;
                        }).Get());
                if (FAILED(chr)) session->failed = true;
                return S_OK;
            }).Get());
    if (FAILED(started)) session->failed = true;

    // Modal loop: the game is blocked while the applet is up, as on the Switch. (The menu audio thread keeps
    // running; whether the Switch ducks it under the web applet is unknown.)
    const ULONGLONG start = GetTickCount64();
    static constexpr int kCaptureSeconds[] = {3, 26, 50};
    size_t nextCapture = 0;
    while (!session->done && !session->failed) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 30, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (req.exitRequested()) {
            session->reason = ExitReason::WindowClosed;
            break;
        }
        long nx = 0, ny = 0, nw = 0, nh = 0;
        req.viewRect(nx, ny, nw, nh);
        const RECT r{nx, ny, nx + nw, ny + nh};
        if (!EqualRect(&r, &session->bounds)) session->ApplyBounds(r);
        if (!req.captureDir.empty() && session->webview && nextCapture < std::size(kCaptureSeconds) &&
            GetTickCount64() - start >= static_cast<ULONGLONG>(kCaptureSeconds[nextCapture]) * 1000) {
            std::error_code ec;
            std::filesystem::create_directories(req.captureDir, ec);
            char name[32];
            std::snprintf(name, sizeof(name), "/credits_%02ds.png", kCaptureSeconds[nextCapture]);
            Capture(session->webview, req.captureDir + name);
            ++nextCapture;
        }
    }

    const ExitReason reason = session->failed ? ExitReason::Unavailable : session->reason;
    session->cancelled = true;
    if (session->controller) session->controller->Close();
    session->webview.Reset();
    session->controller.Reset();
    DestroyWindow(session->container);
    return reason;
}

}  // namespace offline_html
