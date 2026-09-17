// nn::audio::AudioOut as the reference player (Ryujinx 1.1.1403, SDL2 backend) runs it, on top of AudioStream.
// Measured with the opencarbon audio trace (tools/ryujinx_trace; testresults/ryujinx_audio/20260917_122613):
//  - The output stream opens during the first AppendAudioOutBuffer after StartAudioOut, and that buffer is mixed at
//    once, at the volume current then. Before any SetAudioOutVolume that is 1.0.
//  - Every later buffer is taken one buffer period after the previous one and mixed at the volume current when it
//    is taken. It is released, with the buffer event signalled, at that moment, which is one period before it is
//    heard. With nothing queued, that period is silent.
//  - Buffer contents are read when appended. Waiting on the event clears it.
// The volume is applied as the float the game passes; SDL's 1/128 steps are not reproduced.
#pragma once
#include <cstdint>
#include <memory>

class AudioOut {
public:
    AudioOut();
    ~AudioOut();
    bool Open(int sampleRate, int channels, int framesPerBuffer);  // OpenDefaultAudioOut
    void Start();                                                 // StartAudioOut
    void Stop();                                                  // StopAudioOut + CloseAudioOut
    void Append(int16_t* buffer);                                 // AppendAudioOutBuffer (framesPerBuffer frames)
    int16_t* GetReleased();                                       // GetReleasedAudioOutBuffer, nullptr if none
    void WaitEvent();                                             // WaitSystemEvent on the buffer event
    void SetVolume(float volume);                                 // SetAudioOutVolume

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
