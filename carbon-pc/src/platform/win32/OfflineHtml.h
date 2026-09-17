// nn::web::ShowOfflineHtmlPage on Windows: the page plays inside Carbon's window in WebView2 (Chromium), with the
// options ShowCredits @ 0x71000eb800 passes to the Switch applet:
//   background kind 1, footer off, pointer off, boot loading icon off, JS extension on, web audio on,
//   touch on contents off.
// The call blocks like the applet does and returns when the page navigates to the callback URL
// http://localhost/ (the credits do this with a 52 s meta refresh), when the window is closed, or on failure.
#pragma once
#include <functional>
#include <string>

struct HWND__;

namespace offline_html {

struct Request {
    HWND__* window = nullptr;
    std::string documentRoot;                       // the NCA's html-document directory
    std::string url;                                // document path inside it, e.g. credits.htdocs/sw.html
    std::function<void(long& x, long& y, long& w, long& h)> viewRect;  // 1280x720 canvas area in client pixels
    std::function<bool()> exitRequested;            // window closed while the page is up
    std::string captureDir;                         // test runs: capture the page at a few points in time
    bool renderWhileHidden = false;                 // test runs: keep Chromium painting in a hidden window
};

enum class ExitReason { CallbackUrl, WindowClosed, LoadFailed, Unavailable };

ExitReason Show(const Request& request);
const char* ToString(ExitReason reason);

}  // namespace offline_html
