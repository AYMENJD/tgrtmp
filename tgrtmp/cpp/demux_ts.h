// A minimal MPEG-TS (ISO/IEC 13818-1) demuxer for live publishing: it pulls
// the H.264 video access units and the ADTS AAC audio frames of a transport
// stream (the shape Streamlink feeds for HLS sources), assembling PES
// packets, extracting PTS/DTS and detecting discontinuities. It decodes
// nothing: every frame is handed over untouched (stream copy).
//
// Clean-room implementation written against the public spec.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace tgrtmp {

// One demuxed item of any demuxer.
struct DemuxFrame {
    enum class Kind : uint8_t {
        Video,        // data = one Annex-B access unit
        Audio,        // data = one raw AAC frame (no ADTS)
        Discontinuity // The source timeline jumped.
    };
    Kind kind = Kind::Video;
    std::string data;
    int64_t dts_ms = 0;
    int64_t pts_ms = 0;
    bool keyframe = false;
    // Video keyframes: the AVCDecoderConfigurationRecord (SPS/PPS), if the
    // access unit carries them. Audio: the AudioSpecificConfig.
    std::string config;
};

using DemuxSink = std::function<void(DemuxFrame &&)>;

class TsDemuxer {
  public:
    // Feed transport stream bytes (any chunking).
    void feed(std::string_view data, const DemuxSink &sink);
    // Flush the access unit in flight (end of stream).
    void finish(const DemuxSink &sink);

    // True as soon as the program map declares an H.264 track, before any
    // frame of it arrives (the publisher holds audio for the first keyframe).
    bool has_video_track() const { return video_pid_ >= 0; }

    // Warnings for streams Telegram's server will not accept (routed to the
    // sink through the Source).
    void set_warner(std::function<void(const char *)> warner) { warner_ = std::move(warner); }

  private:
    // Assembles PSI sections (PAT, PMT) across packet boundaries.
    struct PsiAssembler {
        bool feed(const unsigned char *payload, size_t length, std::string &section, bool pusi);
        bool collecting_ = false;
        std::string buffer_;
    };

    struct PesAssembly {
        bool started = false;
        std::string buffer;
        int64_t pts = 0, dts = 0; // 90 kHz ticks.
    };

    void process_packet(const unsigned char *p, const DemuxSink &sink);
    void process_psi(const unsigned char *payload, size_t length, bool pusi, int pid);
    void process_pes(const unsigned char *payload, size_t length, bool pusi, int pid,
                     const DemuxSink &sink);
    void emit_video(const DemuxSink &sink);
    void emit_audio(const DemuxSink &sink);
    void check_discontinuity(int64_t dts_ms, const DemuxSink &sink);

    std::string buffer_;
    int pmt_pid_ = -1;
    int video_pid_ = -1, audio_pid_ = -1;
    PsiAssembler pat_asm_, pmt_asm_;
    PesAssembly video_pes_, audio_pes_;
    bool video_first_ = true;
    int64_t last_video_dts_ = INT64_MIN;
    std::function<void(const char *)> warner_;
    uint64_t fed_bytes_ = 0;
    bool warned_no_pmt_ = false;
};

} // namespace tgrtmp
