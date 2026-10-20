// The interface between demuxers and the publishing side: a FrameSink
// receives the demuxed frames of a source. The Publisher implements it; a
// collector is used by tests and tools.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tgrtmp {

class FrameSink {
  public:
    virtual ~FrameSink() = default;
    virtual void on_video(int64_t dts_ms, int64_t pts_ms, std::string_view annexb, bool keyframe,
                          std::string_view avcc_config) = 0;
    virtual void on_audio(int64_t pts_ms, std::string_view asc, std::string_view raw) = 0;
    virtual void on_jump() = 0; // A discontinuity of the source timeline.
    // The container declared a video track (known before any of its frames:
    // a live source joined mid-GOP must hold audio for the first keyframe).
    virtual void on_has_video() {}
    // A condition the publisher (or the server) will not accept, surfacing
    // what would otherwise fail silently. Sinks that ignore it lose nothing.
    virtual void on_warn(std::string_view) {}
};

// A sink that records every frame (tests, tools, and controllers that poll
// it from Python after each feed).
class CollectorSink : public FrameSink {
  public:
    struct Frame {
        bool video = false;
        int64_t dts_ms = 0, pts_ms = 0;
        bool keyframe = false;
        std::string data;
        std::string config;
    };

    void on_video(int64_t dts_ms, int64_t pts_ms, std::string_view annexb, bool keyframe,
                  std::string_view avcc_config) override {
        frames.push_back(
            Frame{true, dts_ms, pts_ms, keyframe, std::string(annexb), std::string(avcc_config)});
    }
    void on_audio(int64_t pts_ms, std::string_view asc, std::string_view raw) override {
        frames.push_back(Frame{false, pts_ms, pts_ms, false, std::string(raw), std::string(asc)});
    }
    void on_jump() override { jumps++; }
    void on_has_video() override { has_video = true; }
    void on_warn(std::string_view message) override { warnings.emplace_back(message); }

    void clear() {
        frames.clear();
        warnings.clear();
        jumps = 0;
    }

    std::vector<Frame> frames;
    int jumps = 0;
    std::vector<std::string> warnings;
    bool has_video = false;
};

} // namespace tgrtmp
