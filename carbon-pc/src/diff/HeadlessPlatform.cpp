// Platform pieces the GB core needs when it runs inside gbdiff.dll: plain file-system calls and a silent
// audio stream (the audio thread is disabled there anyway).
#include "platform/AudioStream.h"
#include "platform/Paths.h"
#include "platform/Platform.h"
#include <chrono>
#include <filesystem>

namespace Platform {

void CreateDirectory(const char* path) {
    std::error_code ec;
    std::filesystem::create_directories(Paths::Resolve(path), ec);
}

void Commit(const char*) {}

int64_t MonotonicNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace Platform

struct AudioStream::Impl {};
AudioStream::AudioStream() = default;
AudioStream::~AudioStream() = default;
bool AudioStream::Open(int, int, int) { return true; }
void AudioStream::Write(const int16_t*, int) {}
