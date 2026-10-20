// A minimal MKV/WebM (EBML) demuxer for publishing: it streams the
// H.264 and AAC frames of MKV/WebM files (the containers the reference
// implementation also plays) out of SimpleBlocks and BlockGroups, with
// lacing support. It decodes nothing: frames are handed over untouched
// (stream copy), H.264 converted to Annex-B when stored length-prefixed.
//
// Clean-room implementation written against the public Matroska spec.

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "demux_ts.h" // DemuxFrame, DemuxSink.

namespace tgrtmp {

class MkvDemuxer {
  public:
    void feed(std::string_view data, const DemuxSink &sink);
    void finish(const DemuxSink &sink);

    // True once the Tracks element declares a video track.
    bool has_video_track() const {
        for (const Track &track : tracks_) {
            if (track.video)
                return true;
        }
        return false;
    }

    // Warnings for streams the server will not accept.
    void set_warner(std::function<void(const std::string &)> warner) {
        warner_ = std::move(warner);
    }

  private:
    struct Track {
        uint64_t number = 0;
        bool video = false;
        std::string config; // AVCDecoderConfigurationRecord, or AudioSpecificConfig.
        size_t length_size = 4;
    };

    // One parse step over the buffer; false when more data is needed.
    bool step(const DemuxSink &sink);

    // EBML primitives; return 0 and set `ok` on truncation.
    uint64_t read_id(bool &ok);
    uint64_t read_size(bool &ok, bool &unknown);

    void parse_segment_element(uint64_t id, uint64_t size, bool unknown, const DemuxSink &sink);
    void parse_tracks(std::string_view data);
    void enter_cluster(uint64_t size, bool unknown);
    void parse_cluster_element(uint64_t id, uint64_t size, bool unknown, const DemuxSink &sink);
    void parse_block(std::string_view block, bool simple, const DemuxSink &sink,
                     bool group_keyframe = true);
    void emit_frame(const Track &track, std::string_view frame, int64_t timestamp_ns, bool keyframe,
                    size_t lace_index, const DemuxSink &sink);

    std::string buffer_;
    std::function<void(const std::string &)> warner_;
    size_t pos_ = 0;
    enum class State { EbmlHeader, Segment, Cluster } state_ = State::EbmlHeader;
    uint64_t timestamp_scale_ = 1000000; // Nanoseconds per tick.
    std::vector<Track> tracks_;
    int64_t cluster_ticks_ = 0;
    bool have_cluster_ticks_ = false;
    uint64_t cluster_remaining_ = 0; // Known-size clusters.
    bool cluster_unknown_size_ = false;
};

} // namespace tgrtmp
