#pragma once
// Stub header for NXVideo C++ bindings
// Full implementation pending NXVideo library integration

#include <string>
#include <vector>
#include <stdexcept>

namespace nxvideo {

enum class Codec { H264, H265, VP9, AV1 };
enum class State { Stopped, Playing, Paused, Buffering, Seeking, Ended, Error };

class VideoException : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct VideoStreamInfo {
    uint32_t width = 0, height = 0;
    double fps = 0;
    std::string codecName;
};

struct AudioStreamInfo {
    uint32_t sampleRate = 0, channels = 0;
    std::string codecName;
};

struct MediaInfo {
    bool hasVideo = false;
    bool hasAudio = false;
    double duration = 0;
    std::string container;
    std::string title;
    VideoStreamInfo video;
    AudioStreamInfo audio;
};

class Player {
public:
    struct Config {
        bool hwDecode = true;
        bool loop = false;
        float volume = 1.0f;
        float playbackRate = 1.0f;
        bool muted = false;
    };

    explicit Player(const Config& = {}) {}
    void open(const std::string&) {}
    void play() {}
    void pause() {}
    void stop() {}
    void seek(double) {}
    double position() const { return 0; }
    double duration() const { return 0; }
    State state() const { return State::Stopped; }
    void setVolume(float) {}
    void setMuted(bool) {}
    void setPlaybackRate(float) {}
    MediaInfo getInfo() const { return {}; }
};

struct System {
    static bool isHardwareAvailable(Codec) { return false; }
};

inline System& getVideoSystem() {
    static System sys;
    return sys;
}

} // namespace nxvideo
