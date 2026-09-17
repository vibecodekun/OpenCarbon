// Blocking PCM output stream (replaces nn::audio::AudioOut / AudioRenderer device sinks).
#pragma once
#include <cstdint>
#include <memory>

class AudioStream {
public:
    AudioStream();
    ~AudioStream();
    bool Open(int sampleRate, int channels, int framesPerBuffer);
    // Blocks until the device has room, like waiting on the audio system event in the original loops.
    void Write(const int16_t* interleaved, int frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
