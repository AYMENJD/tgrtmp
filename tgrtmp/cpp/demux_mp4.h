// A minimal ISO BMFF (MP4) demuxer for publishing: it handles progressive
// (moov-first, faststart) files and fragmented MP4 (DASH/`styp` segments, the
// shape Streamlink feeds for DASH sources). It decodes nothing: H.264 access
// units are converted from their length-prefixed (AVCC) form to Annex-B and
// handed over untouched, as are AAC frames.
//
// A moov that follows its mdat cannot be read from a live stream: like the
// reference implementation, that is rejected with a clear error (remux with
// `-movflags +faststart`).
//
// Clean-room implementation written against ISO/IEC 14496-12 and -14.

#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "demux_ts.h" // DemuxFrame, DemuxSink.

namespace tgrtmp {

class Mp4Demuxer {
  public:
    void feed(std::string_view data, const DemuxSink &sink);
    void finish(const DemuxSink &sink);

    // Prime the sample table from a moov read elsewhere in the file (a
    // progressive MP4 whose moov sits at the tail): the caller then feeds
    // the media with feed_at(), absolute file offsets included.
    void prime(std::string_view moov_box, const DemuxSink &sink);
    void feed_at(std::string_view data, uint64_t offset, const DemuxSink &sink);

    // True once the moov declares a video track.
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
    struct SampleOut {
        uint64_t offset = 0; // Absolute file offset.
        uint32_t size = 0;
        int64_t dts = 0; // In the track's timescale.
        int64_t pts = 0;
        bool sync = false;
        size_t track = 0;
    };

    // One sample of a progressive track, in decoding order.
    struct Sample {
        uint64_t offset = 0; // Absolute file offset.
        uint32_t size = 0;
        int64_t dts = 0; // In the track's timescale.
        int64_t cts = 0; // Composition offset (pts = dts + cts).
        bool sync = false;
    };

    struct Track {
        int id = 0;
        bool video = false;
        uint32_t timescale = 1000;
        std::string config; // AVCDecoderConfigurationRecord, or AudioSpecificConfig.
        size_t length_size = 4;
        // Fragmented state.
        int64_t dts = 0;
        uint32_t default_duration = 0, default_size = 0, default_flags = 0;
        // Progressive state.
        std::vector<Sample> samples;
    };

    // Processes as many complete top-level boxes of `buffer_` as possible.
    void process(const DemuxSink &sink);
    void parse_moov(std::string_view box, const DemuxSink &sink);
    void parse_moof(std::string_view box, uint64_t moof_offset, const DemuxSink &sink);
    void parse_stsd(Track &track, std::string_view stsd);
    void parse_esds(Track &track, std::string_view esds);
    void parse_stbl(Track &track, std::string_view stbl);
    // Emits the samples fully covered by the window; returns the covered
    // byte count (the window never advances past a straddling sample).
    size_t emit_from_mdat(std::string_view content, uint64_t content_start, const DemuxSink &sink);
    void emit_sample(const SampleOut &sample, std::string_view bytes, const DemuxSink &sink);
    [[noreturn]] static void fail(const std::string &message);

    std::string buffer_;
    std::function<void(const std::string &)> warner_;
    uint64_t position_ = 0; // Absolute offset of buffer_[0].
    bool moov_parsed_ = false;
    bool primed_ = false; // The table came from a tail moov; data feeds carry absolute offsets.
    bool fragmented_ = false;
    uint64_t moov_end_ = 0;
    uint64_t moof_base_ = 0; // Base data offset of the current fragment.
    bool have_base_ = false;
    uint64_t prev_fragment_end_ = 0; // Where the default fragment base falls.
    uint64_t last_emitted_end_ = 0;
    bool mdat_open_ = false; // An mdat is streaming through the feeder.
    uint64_t mdat_remaining_ = 0;
    uint64_t mdat_position_ = 0;
    uint64_t at_offset_ = 0; // Absolute offset of buffer_[0] in primed mode.
    bool at_set_ = false;
    std::vector<Track> tracks_;
    std::deque<SampleOut> pending_; // Samples awaiting their mdat, by offset.
};

} // namespace tgrtmp
