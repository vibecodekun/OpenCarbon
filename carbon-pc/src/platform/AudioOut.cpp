#include "platform/AudioOut.h"
#include "platform/AudioStream.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct AudioOut::Impl {
    using Clock = std::chrono::steady_clock;

    int rate = 0, channels = 0, frames = 0;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::pair<int16_t*, std::vector<int16_t>>> queued;  // appended, not yet taken
    std::deque<int16_t*> released;
    std::deque<std::vector<int16_t>> mixed;                        // taken, waiting for the device thread
    bool event = false;
    float volume = 1.0f;
    bool started = false;
    bool streamOpen = false;
    bool stop = false;
    Clock::time_point firstTake;
    int64_t taken = 0;  // buffers taken since the stream opened (including silent periods)
    std::thread device;

    // Caller holds the mutex.
    void Take() {
        std::vector<int16_t> out(static_cast<size_t>(frames) * channels, 0);
        if (!queued.empty()) {
            const std::vector<int16_t>& in = queued.front().second;
            for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<int16_t>(in[i] * volume);
            released.push_back(queued.front().first);
            queued.pop_front();
            event = true;
        }
        mixed.push_back(std::move(out));
        ++taken;
        changed.notify_all();
    }

    Clock::time_point NextTake() const {
        return firstTake + std::chrono::nanoseconds(taken * frames * 1'000'000'000LL / rate);
    }

    void DeviceMain() {
        AudioStream stream;
        stream.Open(rate, channels, frames);
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            if (!mixed.empty()) {
                std::vector<int16_t> block = std::move(mixed.front());
                mixed.pop_front();
                lock.unlock();
                stream.Write(block.data(), frames);
                lock.lock();
                continue;
            }
            if (stop) return;
            if (streamOpen && Clock::now() >= NextTake()) {
                Take();
                continue;
            }
            if (streamOpen) changed.wait_until(lock, NextTake());
            else changed.wait(lock);
        }
    }
};

AudioOut::AudioOut() : impl(std::make_unique<Impl>()) {}

AudioOut::~AudioOut() { Stop(); }

bool AudioOut::Open(int sampleRate, int channels, int framesPerBuffer) {
    impl->rate = sampleRate;
    impl->channels = channels;
    impl->frames = framesPerBuffer;
    impl->device = std::thread([this] { impl->DeviceMain(); });
    return true;
}

void AudioOut::Start() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->started = true;
}

void AudioOut::Stop() {
    {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->stop = true;
        impl->changed.notify_all();
    }
    if (impl->device.joinable()) impl->device.join();
}

void AudioOut::Append(int16_t* buffer) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->queued.emplace_back(buffer, std::vector<int16_t>(buffer, buffer + static_cast<size_t>(impl->frames) * impl->channels));
    if (impl->started && !impl->streamOpen) {
        impl->streamOpen = true;
        impl->firstTake = Impl::Clock::now();
        impl->Take();
    }
    impl->changed.notify_all();
}

int16_t* AudioOut::GetReleased() {
    std::lock_guard<std::mutex> lock(impl->mutex);
    if (impl->released.empty()) return nullptr;
    int16_t* buffer = impl->released.front();
    impl->released.pop_front();
    return buffer;
}

void AudioOut::WaitEvent() {
    std::unique_lock<std::mutex> lock(impl->mutex);
    impl->changed.wait(lock, [this] { return impl->event || impl->stop; });
    impl->event = false;
}

void AudioOut::SetVolume(float volume) {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->volume = volume;
}
