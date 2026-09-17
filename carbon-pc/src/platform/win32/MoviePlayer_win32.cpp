// The startup movie (wf_logo.mp4) on Windows.
//
// The original is the NintendoSDK movie sample player on top of NintendoSDK_movie 10.4.1 (subsdk2):
//  - MoviePlayer_Play @ 0x710009bf50 never reads input, so the movie can't be skipped. Once playback has started
//    it checks every 100 ms whether both renderers have reached the end of their streams, then destroys the
//    player and its vi layer: the screen stays black until the app presents its first frame.
//  - Before starting, it waits up to 5 s for the decoders, then up to 1 s (200 x 5 ms) until 6 audio and 4 video
//    frames are decoded. An earlier 5 s wait for buffered data ends at once for a file, because
//    NuMediaExtractor::getBufferedRange fails unless the source is a caching (network) one.
//  - Video renderer (FUN_71001056b0, FUN_7100105a10): a frame is presented when the media clock reaches its
//    timestamp, and dropped if it is 40 ms late or more.
//  - The Android MPEG4Extractor inside reads an edit list before the track's duration and sample rate are known,
//    so it ignores both of wf_logo.mp4's: the video is stamped from 66.7 ms and the audio keeps its 1600 AAC
//    priming frames. Media Foundation doesn't apply them either.
//  - NVN renderer (init @ 0x7100106b50, draw commands @ 0x7100108270, per frame @ 0x7100107e10): a 1280x720
//    window cleared to 50% grey, and a quad scaled horizontally by (video aspect / 16:9). The NV12 planes are
//    copied into R8 and RG8 textures and converted by the sample's shader with the matrix picked by the decoder's
//    "nv12-colorspace" (tables @ 0x710019589c).
//  - Sampler: nvnSamplerBuilderSetDefaults, SetWrapMode(7, 7, 7), nothing else. Executing the NVN driver's own
//    code for that (sdk: SetDefaults @ 0x4679a0, descriptor @ 0x485ab0, tools/movie/nvn_sampler.py) gives the GPU
//    descriptor mag LINEAR, min LINEAR, mip NONE, clamp to edge.
//  - Colour space: nv12-colorspace is 0, so BT.601 limited range. In subsdk2 it comes from ACodec's
//    "hw-buf-ColorSpace" = SfNvnUtil::getNativeBufColorSpace, whose colour bits only
//    SfNvnUtil::setColorAspect sets. That runs only when the configure format has color-range/standard/transfer,
//    which the track configuration gets only from MetaData colour keys, and only MatroskaExtractor sets those.
//  - Audio renderer (@ 0x71001042a0): nn::audio::AudioOut, full volume.
//
// Here Media Foundation decodes (H.264 -> NV12, AAC -> 16-bit PCM), the same shaders draw into the canvas with the
// same sampling, and AudioStream plays the sound.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#undef CreateDirectory

#include "platform/win32/MoviePlayer.h"
#include "platform/AudioStream.h"
#include "platform/Paths.h"
#include "platform/Platform.h"
#include "platform/gl.h"
#include "platform/win32/Script.h"
#include "platform/win32/Win32Platform.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace movie_player {
namespace {

using Microsoft::WRL::ComPtr;

constexpr int kCanvasW = 1280;
constexpr int kCanvasH = 720;
constexpr int64_t kDropLateUs = 0x9c41;       // FUN_7100105a10: frames at least this late aren't presented
constexpr int64_t kEndPollNs = 100'000'000;   // MoviePlayer_Play's end-of-stream check
constexpr int64_t kDecoderWaitNs = 5'000'000'000;
constexpr int64_t kPrefillWaitNs = 1'000'000'000;
constexpr int64_t kPrefillVideoFrames = 4;
constexpr size_t kQueuedVideoFrames = 8;
constexpr int kAudioChunkFrames = 1024;
constexpr int kShotEvery = 30;                // --script: capture every 30th presented frame

// From the binary (@ 0x7100195569, @ 0x710019563f). glslc lays the uniform block out as std140; desktop GL needs
// that spelled out.
const char* const kVertexShader =
    "#version 450\nlayout(location = 0) in vec4 a_position;layout(location = 1) in vec2 a_texCoord;out vec2 "
    "v_texCoord;void main() {   gl_Position = a_position;   v_texCoord = a_texCoord;   v_texCoord.y = v_texCoord.y;}";
const char* const kFragmentShader =
    "#version 450\nin vec2 v_texCoord;layout(binding = 0) uniform sampler2D u_textureY;layout(binding = 1) uniform "
    "sampler2D u_textureUV;layout(std140, binding = 2) uniform UBO {    vec3 src_bias;    mat3 src_xform;};out vec4 "
    "colorOut;void main() {    float y = texture(u_textureY, v_texCoord).r;    vec2 cbcr = texture(u_textureUV, "
    "v_texCoord).rg;    vec3 yuv = vec3(y, cbcr);    yuv -= src_bias;    yuv *= src_xform;    colorOut = vec4(yuv, "
    "1.0);}";

// @ 0x710019589c (nv12-colorspace 0 and 4): src_bias, then the three mat3 columns, each padded to 16 bytes.
const float kSrcTransform[16] = {
    std::bit_cast<float>(0x3d808081u), std::bit_cast<float>(0x3f008081u), std::bit_cast<float>(0x3f008081u), 0.0f,
    std::bit_cast<float>(0x3f950a81u), 0.0f, std::bit_cast<float>(0x3fcc4a9du), 0.0f,
    std::bit_cast<float>(0x3f950a81u), std::bit_cast<float>(0xbec89514u), std::bit_cast<float>(0xbf501ea8u), 0.0f,
    std::bit_cast<float>(0x3f950a81u), std::bit_cast<float>(0x40011a56u), 0.0f, 0.0f,
};

struct VideoFrame {
    int64_t ptsUs = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> y;   // width x height
    std::vector<uint8_t> uv;  // (width/2 x height/2) CbCr pairs
};

struct Shared {
    std::wstring path;
    std::atomic<bool> stop{false};
    std::mutex mutex;
    std::condition_variable changed;
    bool start = false;

    bool videoOpened = false, videoFailed = false, videoEnded = false;
    int64_t videoDecoded = 0;
    std::deque<std::unique_ptr<VideoFrame>> frames;

    bool audioOpened = false, audioFailed = false, audioReady = false, audioEnded = false;

    void Update(const std::function<void()>& change) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            change();
        }
        changed.notify_all();
    }
};

struct MediaFoundationScope {
    bool com = false, mf = false;
    MediaFoundationScope() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MediaFoundationScope() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

ComPtr<IMFSourceReader> OpenReader(const std::wstring& path, DWORD stream, const GUID& major, const GUID& subtype) {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> type;
    if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader)) ||
        FAILED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) ||
        FAILED(reader->SetStreamSelection(stream, TRUE)) || FAILED(MFCreateMediaType(&type)) ||
        FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, major)) || FAILED(type->SetGUID(MF_MT_SUBTYPE, subtype)))
        return nullptr;
    if (major == MFMediaType_Audio) type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    if (FAILED(reader->SetCurrentMediaType(stream, nullptr, type.Get()))) return nullptr;
    return reader;
}

// ---- video decoder thread

struct VideoLayout {
    UINT32 codedW = 0, codedH = 0;  // buffer size; the chroma plane starts after codedH rows
    LONG offsetX = 0, offsetY = 0;  // displayed area (the decoder's "width"/"height")
    UINT32 width = 0, height = 0;
    LONG stride = 0;
};

bool ReadVideoLayout(IMFSourceReader* reader, VideoLayout& v) {
    ComPtr<IMFMediaType> type;
    if (FAILED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type)) ||
        FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &v.codedW, &v.codedH)))
        return false;
    v.offsetX = v.offsetY = 0;
    v.width = v.codedW;
    v.height = v.codedH;
    MFVideoArea area{};
    if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
        v.offsetX = std::clamp<LONG>(area.OffsetX.value, 0, static_cast<LONG>(v.codedW));
        v.offsetY = std::clamp<LONG>(area.OffsetY.value, 0, static_cast<LONG>(v.codedH));
        v.width = std::min<UINT32>(static_cast<UINT32>(area.Area.cx), v.codedW - static_cast<UINT32>(v.offsetX));
        v.height = std::min<UINT32>(static_cast<UINT32>(area.Area.cy), v.codedH - static_cast<UINT32>(v.offsetY));
    }
    v.stride = static_cast<LONG>(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, v.codedW));
    return v.width >= 2 && v.height >= 2;
}

std::unique_ptr<VideoFrame> CopyFrame(IMFSample* sample, const VideoLayout& v, LONGLONG time) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return nullptr;
    BYTE* base = nullptr;
    LONG stride = v.stride;
    DWORD length = 0;
    ComPtr<IMF2DBuffer> buffer2d;
    const bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&base, &stride));
    if (locked2d) {
        buffer2d->GetContiguousLength(&length);
    } else if (FAILED(buffer->Lock(&base, nullptr, &length))) {
        return nullptr;
    }

    std::unique_ptr<VideoFrame> frame;
    const uint64_t needed = static_cast<uint64_t>(stride) * v.codedH * 3 / 2;
    if (stride >= static_cast<LONG>(v.codedW) && length >= needed) {
        frame = std::make_unique<VideoFrame>();
        frame->ptsUs = time / 10;
        frame->width = static_cast<int>(v.width);
        frame->height = static_cast<int>(v.height);
        const size_t w = v.width, h = v.height, cw = w / 2, ch = h / 2;
        frame->y.resize(w * h);
        frame->uv.resize(cw * ch * 2);
        for (size_t row = 0; row < h; ++row)
            std::memcpy(&frame->y[row * w], base + (v.offsetY + row) * stride + v.offsetX, w);
        const BYTE* chroma = base + static_cast<size_t>(stride) * v.codedH;
        for (size_t row = 0; row < ch; ++row)
            std::memcpy(&frame->uv[row * cw * 2], chroma + (v.offsetY / 2 + row) * stride + (v.offsetX / 2) * 2, cw * 2);
    }
    if (locked2d) buffer2d->Unlock2D();
    else buffer->Unlock();
    return frame;
}

void VideoDecoderMain(Shared* s) {
    MediaFoundationScope scope;
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    ComPtr<IMFSourceReader> reader = OpenReader(s->path, stream, MFMediaType_Video, MFVideoFormat_NV12);
    VideoLayout layout;
    if (!scope.mf || !reader || !ReadVideoLayout(reader.Get(), layout)) {
        s->Update([&] { s->videoFailed = true; });
        return;
    }
    s->Update([&] { s->videoOpened = true; });

    bool logged = false;
    while (!s->stop) {
        DWORD flags = 0;
        LONGLONG time = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, &time, &sample)) || (flags & MF_SOURCE_READERF_ERROR))
            break;
        // the decoder usually settles its output layout (e.g. 1088 coded rows) on the first frame
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) && !ReadVideoLayout(reader.Get(), layout)) break;
        if (sample) {
            std::unique_ptr<VideoFrame> frame = CopyFrame(sample.Get(), layout, time);
            if (!logged) {
                std::printf("movie: video %ux%u (buffer %ux%u, stride %ld), first frame at %.1f ms%s\n", layout.width,
                            layout.height, layout.codedW, layout.codedH, static_cast<long>(layout.stride), time / 1e4,
                            frame ? "" : ": unexpected buffer layout");
                logged = true;
            }
            if (!frame) break;
            std::unique_lock<std::mutex> lock(s->mutex);
            s->changed.wait(lock, [&] { return s->stop || s->frames.size() < kQueuedVideoFrames; });
            if (s->stop) return;
            s->frames.push_back(std::move(frame));
            ++s->videoDecoded;
            lock.unlock();
            s->changed.notify_all();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    s->Update([&] { s->videoEnded = true; });
}

// ---- audio thread: decodes the whole track (4 s), then plays it once started

void AudioMain(Shared* s) {
    MediaFoundationScope scope;
    const DWORD stream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
    ComPtr<IMFSourceReader> reader = OpenReader(s->path, stream, MFMediaType_Audio, MFAudioFormat_PCM);
    ComPtr<IMFMediaType> type;
    UINT32 rate = 0, channels = 0, bits = 0;
    if (scope.mf && reader && SUCCEEDED(reader->GetCurrentMediaType(stream, &type))) {
        rate = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        channels = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
        bits = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
    }
    if (rate == 0 || channels == 0 || bits != 16) {
        s->Update([&] { s->audioFailed = true; });
        return;
    }
    s->Update([&] { s->audioOpened = true; });

    AudioStream out;
    out.Open(static_cast<int>(rate), static_cast<int>(channels), kAudioChunkFrames);
    std::vector<int16_t> pcm;
    while (!s->stop) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(stream, 0, nullptr, &flags, nullptr, &sample)) || (flags & MF_SOURCE_READERF_ERROR))
            break;
        ComPtr<IMFMediaBuffer> buffer;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (sample && SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)) &&
            SUCCEEDED(buffer->Lock(&data, nullptr, &length))) {
            const size_t old = pcm.size();
            pcm.resize(old + length / sizeof(int16_t));
            std::memcpy(&pcm[old], data, (length / sizeof(int16_t)) * sizeof(int16_t));
            buffer->Unlock();
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    }
    s->Update([&] { s->audioReady = true; });  // all 192 frames, so past the 6 the original waits for

    {
        std::unique_lock<std::mutex> lock(s->mutex);
        s->changed.wait(lock, [&] { return s->stop || s->start; });
        if (s->stop) return;
    }
    const size_t frames = pcm.size() / channels;
    for (size_t pos = 0; pos < frames && !s->stop; pos += kAudioChunkFrames) {
        const size_t n = std::min<size_t>(kAudioChunkFrames, frames - pos);
        out.Write(&pcm[pos * channels], static_cast<int>(n));
    }
    s->Update([&] { s->audioEnded = true; });
}

// ---- NVN renderer equivalent

GLuint CompileProgram() {
    const auto compile = [](GLenum kind, const char* source) {
        const GLuint shader = glCreateShader(kind);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[1024] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::fprintf(stderr, "movie: shader compile failed: %s\n", log);
        }
        return shader;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader);
    const GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader);
    const GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        std::fprintf(stderr, "movie: program link failed: %s\n", log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

struct VideoRenderer {
    GLuint program = 0, vao = 0, vbo = 0, ubo = 0, texY = 0, texUV = 0;
    int width = 0, height = 0;

    bool Create() {
        program = CompileProgram();
        if (!program) return false;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, 4 * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 6 * sizeof(float), nullptr);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(4 * sizeof(float)));
        glGenBuffers(1, &ubo);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferData(GL_UNIFORM_BUFFER, sizeof(kSrcTransform), kSrcTransform, GL_STATIC_DRAW);
        return true;
    }

    // @ 0x7100108270: textures and quad for a new video size
    void Resize(int w, int h) {
        glDeleteTextures(1, &texY);
        glDeleteTextures(1, &texUV);
        const auto makeTexture = [](GLenum format, int tw, int th) {
            GLuint tex = 0;
            glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            glTexStorage2D(GL_TEXTURE_2D, 1, format, tw, th);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);  // descriptor min LINEAR, mip NONE
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            return tex;
        };
        texY = makeTexture(GL_R8, w, h);
        texUV = makeTexture(GL_RG8, w / 2, h / 2);
        width = w;
        height = h;

        // triangle strip: position xyzw, texcoord uv
        const float a = (static_cast<float>(w) / static_cast<float>(h)) /
                        (static_cast<float>(kCanvasW) / static_cast<float>(kCanvasH));
        const float quad[24] = {
            -a, 1.0f,  0.0f, 1.0f, 0.0f, 0.0f,
            -a, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f,
            a,  1.0f,  0.0f, 1.0f, 1.0f, 0.0f,
            a,  -1.0f, 0.0f, 1.0f, 1.0f, 1.0f,
        };
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    }

    // @ 0x7100107e10 + the recorded draw
    void Draw(const VideoFrame& f) {
        if (f.width != width || f.height != height) Resize(f.width, f.height);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glBindTexture(GL_TEXTURE_2D, texY);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RED, GL_UNSIGNED_BYTE, f.y.data());
        glBindTexture(GL_TEXTURE_2D, texUV);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width / 2, height / 2, GL_RG, GL_UNSIGNED_BYTE, f.uv.data());
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);

        glViewport(0, 0, kCanvasW, kCanvasH);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, ubo);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texY);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, texUV);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // leaves the default state the app starts from
    void Destroy() {
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, 2, 0);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindVertexArray(0);
        glUseProgram(0);
        glDeleteTextures(1, &texY);
        glDeleteTextures(1, &texUV);
        glDeleteBuffers(1, &ubo);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);
        glDeleteProgram(program);
        glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    }
};

// Waits with the window responsive. Returns false on a window close.
bool WaitFor(Shared& s, int64_t timeoutNs, const std::function<bool()>& ready) {
    const int64_t deadline = Platform::MonotonicNs() + timeoutNs;
    for (;;) {
        Platform::PumpWindowMessages();
        if (Platform::WindowCloseRequested()) return false;
        std::unique_lock<std::mutex> lock(s.mutex);
        if (ready() || Platform::MonotonicNs() >= deadline) return true;
        s.changed.wait_for(lock, std::chrono::milliseconds(5));
    }
}

}  // namespace

void Play(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path file = fs::absolute(Paths::Resolve(path), ec);
    if (!fs::is_regular_file(file, ec)) return;  // nn::fs::OpenFile failed: nothing is played
    std::printf("movie: %s\n", file.string().c_str());

    Shared s;
    s.path = file.wstring();
    const int64_t openNs = Platform::MonotonicNs();
    std::thread audio(AudioMain, &s);
    std::thread video(VideoDecoderMain, &s);
    const auto finish = [&] {
        s.stop = true;
        s.changed.notify_all();
        audio.join();
        video.join();
    };

    VideoRenderer renderer;
    bool play = WaitFor(s, kDecoderWaitNs, [&] {
        return (s.videoOpened || s.videoFailed) && (s.audioOpened || s.audioFailed);
    });
    if (play && (s.videoFailed || s.audioFailed)) {
        std::fprintf(stderr, "movie: Media Foundation can't decode the %s track; not playing it\n",
                     s.videoFailed ? "video" : "audio");
        play = false;
    }
    if (play && !renderer.Create()) play = false;
    if (play) play = WaitFor(s, kPrefillWaitNs, [&] {
        return s.audioReady && (s.videoDecoded >= kPrefillVideoFrames || s.videoEnded);
    });
    if (!play) {
        finish();
        if (renderer.program) renderer.Destroy();
        return;
    }

    int64_t startNs = 0;
    s.Update([&] {
        s.start = true;
        startNs = Platform::MonotonicNs();
    });

    int64_t presented = 0, dropped = 0;
    int64_t nextEndPollNs = startNs;
    std::vector<std::pair<std::string, std::vector<unsigned char>>> shots;
    for (;;) {
        Platform::PumpWindowMessages();
        if (Platform::WindowCloseRequested()) break;
        const int64_t now = Platform::MonotonicNs();
        if (now >= nextEndPollNs) {
            bool ended = false;
            s.Update([&] { ended = s.audioEnded && s.videoEnded && s.frames.empty(); });
            if (ended) break;
            while (nextEndPollNs <= now) nextEndPollNs += kEndPollNs;
        }

        std::unique_ptr<VideoFrame> frame;
        int64_t nextDueNs = now + 4'000'000;
        s.Update([&] {
            // The first frame anchors the media clock to itself (FUN_7100105a10), so it shows at once even though
            // it is stamped 66.7 ms: neither decoder applies the file's edit lists. The audio renderer re-anchors
            // the clock to its own position with every buffer, which times the frames after it.
            if (presented == 0 && dropped == 0 && !s.frames.empty()) {
                frame = std::move(s.frames.front());
                s.frames.pop_front();
            }
            const int64_t clockUs = (now - startNs) / 1000;
            while (!frame && !s.frames.empty() && s.frames.front()->ptsUs <= clockUs) {
                std::unique_ptr<VideoFrame> due = std::move(s.frames.front());
                s.frames.pop_front();
                if (clockUs - due->ptsUs >= kDropLateUs) {
                    ++dropped;
                    continue;
                }
                frame = std::move(due);
                break;
            }
            if (!s.frames.empty()) nextDueNs = std::min(nextDueNs, startNs + s.frames.front()->ptsUs * 1000);
        });

        if (frame) {
            renderer.Draw(*frame);
            if (script::Active() && presented % kShotEvery == 0) {
                char name[32];
                std::snprintf(name, sizeof(name), "movie_%03lld", static_cast<long long>(presented));
                shots.emplace_back(name, Platform::ReadCanvas());
            }
            ++presented;
            Platform::PresentCanvas();
        } else {
            Platform::SleepUntilNs(std::min(nextDueNs, nextEndPollNs));
        }
    }
    const int64_t endNs = Platform::MonotonicNs();
    finish();

    // FUN_710009d640 destroys the renderer with its layer: black until the app's first frame
    renderer.Destroy();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    Platform::PresentCanvas();

    std::printf("movie: started %.0f ms after opening, played %.3f s, %lld frames presented, %lld dropped\n",
                (startNs - openNs) / 1e6, (endNs - startNs) / 1e9, static_cast<long long>(presented),
                static_cast<long long>(dropped));
    for (const auto& [name, pixels] : shots) Platform::WriteShot(name, pixels);
}

}  // namespace movie_player
