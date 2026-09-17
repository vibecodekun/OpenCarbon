// Platform services the app layer needs (replacing nn::vi/EGL, nn::hid, nn::oe, nn::web, nn::fs mounts).
// Implemented per target (src/platform/win32 for the Windows rebuild).
#pragma once
#include <cstdint>

struct NpadSnapshot;

namespace Platform {

bool MountSaveAndRom();                 // nn::fs::MountSaveData("save") + MountRom("rom")
void InitGraphics();                    // Graphics_InitEGL @ 0x710009b540: wf_logo.mp4, window, GL 4.5 core
                                        // context, vsync on, GL loader init
void SwapBuffers();                     // eglSwapBuffers
void PollNpad(NpadSnapshot& out);       // merged No.1 + Handheld controller state
bool PopExitRequest();                  // nn::oe notification 4 (exit requested) / window close
void ShowOfflineHtml(const char* url);  // nn::web::ShowOfflineHtmlPage (credits)
int64_t MonotonicNs();                  // nn::time::ClockSnapshot spans
void CreateDirectory(const char* path); // nn::fs::CreateDirectory on a mounted path
void Commit(const char* mount);         // nn::fs::Commit

}  // namespace Platform
