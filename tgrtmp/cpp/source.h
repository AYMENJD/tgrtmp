// The feeder-side glue: it recognizes the container of a byte stream (MPEG-TS,
// MP4, MKV) from its first bytes, demuxes it incrementally and routes the
// frames into a Publisher.
//
// Clean-room implementation.

#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "demux_mp4.h"
#include "demux_mkv.h"
#include "demux_ts.h"
#include "sink.h"

namespace tgrtmp {

class Source {
  public:
    explicit Source(FrameSink &sink) : sink_(sink) {}

    // Feed raw container bytes (any chunking; the first call may be smaller
    // than the probe needs).
    void feed(std::string_view data) {
        if (kind_ == Kind::Unknown) {
            initial_.append(data);
            probe();
            return;
        }
        demux_feed(data);
    }

    // End of the stream: flush what the demuxers hold.
    void finish() {
        if (kind_ == Kind::Unknown) {
            probe();
            if (kind_ == Kind::Unknown) {
                throw std::runtime_error(
                    "Unsupported stream: only MPEG-TS, MP4 (faststart/fragmented) and "
                    "MKV/WebM with H.264 video and AAC audio can be published");
            }
        }
        DemuxSink sink = [this](DemuxFrame &&frame) { route(std::move(frame)); };
        if (kind_ == Kind::Ts) {
            ts_.finish(sink);
        } else if (kind_ == Kind::Mp4) {
            mp4_.finish(sink);
        } else if (kind_ == Kind::Mkv) {
            mkv_.finish(sink);
        }
    }

    // Prime the MP4 sample table from a moov box read elsewhere in the file
    // (a progressive MP4 whose moov sits at the tail). MP4 sources only.
    void prime(std::string_view moov_box) {
        probe_if_needed();
        if (kind_ != Kind::Mp4) {
            throw std::runtime_error("Only MP4 sources can be primed with a moov");
        }
        ensure_warners();
        mp4_.prime(moov_box, [this](DemuxFrame &&frame) { route(std::move(frame)); });
    }

    // Feed media bytes located at an absolute file offset: the primed table
    // matches samples against it. MP4 sources only.
    void feed_at(std::string_view data, uint64_t offset) {
        if (kind_ != Kind::Mp4) {
            throw std::runtime_error("Only MP4 sources accept offset feeds");
        }
        DemuxSink sink = [this](DemuxFrame &&frame) { route(std::move(frame)); };
        mp4_.feed_at(data, offset, sink);
        announce_video();
    }

    // Whether a video track was seen (so the publisher knows to hold audio
    // until the first keyframe).
    bool saw_video() const { return saw_video_; }

  private:
    void probe_if_needed() {
        if (kind_ == Kind::Unknown) {
            probe();
        }
    }

    void ensure_warners() {
        if (!warners_set_) {
            warners_set_ = true;
            auto forward = [this](const std::string &message) { sink_.on_warn(message); };
            auto forward_c = [this](const char *message) { sink_.on_warn(message); };
            ts_.set_warner(std::move(forward_c));
            mp4_.set_warner(forward);
            mkv_.set_warner(forward);
        }
    }

    void announce_video() {
        // Announce the video track as soon as the container declares it.
        if (!told_has_video_) {
            bool has_video = (kind_ == Kind::Ts && ts_.has_video_track()) ||
                             (kind_ == Kind::Mp4 && mp4_.has_video_track()) ||
                             (kind_ == Kind::Mkv && mkv_.has_video_track());
            if (has_video) {
                told_has_video_ = true;
                sink_.on_has_video();
            }
        }
    }

    void demux_feed(std::string_view data) {
        ensure_warners();
        if (kind_ == Kind::Ts) {
            ts_.feed(data, [this](DemuxFrame &&frame) { route(std::move(frame)); });
        } else if (kind_ == Kind::Mp4) {
            mp4_.feed(data, [this](DemuxFrame &&frame) { route(std::move(frame)); });
        } else if (kind_ == Kind::Mkv) {
            mkv_.feed(data, [this](DemuxFrame &&frame) { route(std::move(frame)); });
        }

        announce_video();
    }

    void probe() {
        std::string_view data(initial_);
        if (data.size() >= 4 && data.substr(0, 4) == "\x1A\x45\xDF\xA3") {
            kind_ = Kind::Mkv;
        } else if (data.size() >= 8) {
            std::string_view brand = data.substr(4, 4);
            if (brand == "ftyp" || brand == "styp" || brand == "moov") {
                kind_ = Kind::Mp4;
            } else if (data.size() < 2 * 188) {
                // Too few bytes to confirm a sync at +188: wait for more.
                for (char byte : data) {
                    if (static_cast<unsigned char>(byte) == 0x47)
                        return;
                }
            } else {
                // MPEG-TS: a sync byte whose successors at +188 and +376 are
                // also sync bytes (three, so a spurious pair inside a payload
                // cannot lock the parse onto a shifted grid). The scan
                // tolerates an entry mid-packet (a seek, a spliced capture).
                size_t limit = std::min(data.size() - 188, size_t(4) * 188);
                for (size_t at = 0; at <= limit; ++at) {
                    if (static_cast<unsigned char>(data[at]) != 0x47)
                        continue;
                    if (static_cast<unsigned char>(data[at + 188]) != 0x47)
                        continue;
                    if (at + 2 * 188 < data.size() &&
                        static_cast<unsigned char>(data[at + 2 * 188]) != 0x47) {
                        continue;
                    }
                    kind_ = Kind::Ts;
                    break;
                }
            }
        }
        if (kind_ == Kind::Unknown && data.size() >= 8) {
            throw std::runtime_error(
                "Unsupported stream: only MPEG-TS, MP4 (faststart/fragmented) and "
                "MKV/WebM with H.264 video and AAC audio can be published");
        }
        if (kind_ != Kind::Unknown && !initial_.empty()) {
            demux_feed(initial_);
            initial_.clear();
        }
    }

    void route(DemuxFrame &&frame) {
        switch (frame.kind) {
        case DemuxFrame::Kind::Video:
            if (!saw_video_)
                saw_video_ = true;
            sink_.on_video(frame.dts_ms, frame.pts_ms, frame.data, frame.keyframe, frame.config);
            break;
        case DemuxFrame::Kind::Audio:
            sink_.on_audio(frame.pts_ms, frame.config, frame.data);
            break;
        case DemuxFrame::Kind::Discontinuity:
            sink_.on_jump();
            break;
        }
    }

    FrameSink &sink_;
    enum class Kind : uint8_t { Unknown, Ts, Mp4, Mkv } kind_ = Kind::Unknown;
    std::string initial_;
    TsDemuxer ts_;
    Mp4Demuxer mp4_;
    MkvDemuxer mkv_;
    bool saw_video_ = false;
    bool told_has_video_ = false;
    bool warners_set_ = false;
};

} // namespace tgrtmp
