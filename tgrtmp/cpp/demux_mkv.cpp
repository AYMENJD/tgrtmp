#include "demux_mkv.h"

#include <cstring>
#include <stdexcept>

#include "flv.h"
#include "h264.h"

namespace tgrtmp {

namespace {

[[noreturn]] void fail(const char *what) { throw std::runtime_error(what); }

constexpr uint64_t kSegment = 0x18538067;
constexpr uint64_t kInfo = 0x1549A966;
constexpr uint64_t kTimestampScale = 0x2AD7B1;
constexpr uint64_t kTracks = 0x1654AE6B;
constexpr uint64_t kTrackEntry = 0xAE;
constexpr uint64_t kTrackNumber = 0xD7;
constexpr uint64_t kTrackType = 0x83;
constexpr uint64_t kCodecId = 0x86;
constexpr uint64_t kCodecPrivate = 0x63A2;
constexpr uint64_t kCluster = 0x1F43B675;
constexpr uint64_t kClusterTimestamp = 0xE7;
constexpr uint64_t kSimpleBlock = 0xA3;
constexpr uint64_t kBlockGroup = 0xA0;
constexpr uint64_t kBlock = 0xA1;
constexpr uint64_t kReferenceBlock = 0xFB;

constexpr int TRACK_TYPE_VIDEO = 1;

std::string_view as_view(const std::string &data, size_t offset, uint64_t length) {
    return std::string_view(data).substr(offset, static_cast<size_t>(length));
}

} // namespace

void MkvDemuxer::feed(std::string_view data, const DemuxSink &sink) {
    buffer_.append(data);
    while (step(sink)) {
    }
    // Keep the unparsed tail (including a partial element header).
    buffer_.erase(0, pos_);
    pos_ = 0;
}

void MkvDemuxer::finish(const DemuxSink &) {
    // Blocks are complete within clusters; nothing to flush.
}

uint64_t MkvDemuxer::read_id(bool &ok) {
    ok = true;
    if (pos_ >= buffer_.size()) {
        ok = false;
        return 0;
    }
    unsigned char first = static_cast<unsigned char>(buffer_[pos_]);
    int length = 1;
    uint64_t value = first;
    for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
        ++length;
    if (length > 8 || pos_ + length > buffer_.size()) {
        ok = false;
        return 0;
    }
    for (int i = 1; i < length; ++i) {
        value = (value << 8) | static_cast<unsigned char>(buffer_[pos_ + i]);
    }
    pos_ += static_cast<size_t>(length);
    return value;
}

uint64_t MkvDemuxer::read_size(bool &ok, bool &unknown) {
    ok = true;
    unknown = false;
    if (pos_ >= buffer_.size()) {
        ok = false;
        return 0;
    }
    unsigned char first = static_cast<unsigned char>(buffer_[pos_]);
    int length = 1;
    for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
        ++length;
    if (length > 8 || pos_ + length > buffer_.size()) {
        ok = false;
        return 0;
    }
    uint64_t value = first & (0xFF >> length);
    for (int i = 1; i < length; ++i) {
        value = (value << 8) | static_cast<unsigned char>(buffer_[pos_ + i]);
    }
    pos_ += static_cast<size_t>(length);
    if (value == (static_cast<uint64_t>(1) << (7 * length)) - 1)
        unknown = true;
    return value;
}

bool MkvDemuxer::step(const DemuxSink &sink) {
    if (state_ == State::EbmlHeader) {
        // The file begins with the EBML header, then the Segment.
        if (buffer_.size() - pos_ < 4)
            return false;
        if (std::string_view(buffer_).substr(pos_, 4) != "\x1A\x45\xDF\xA3") {
            throw std::runtime_error("Not an MKV/WebM file");
        }
        state_ = State::Segment;
        // Fall through to Segment parsing; the header is skipped as a segment
        // sibling element (its id differs from every element we care about).
    }
    if (state_ == State::Segment) {
        bool ok = false, unknown = false;
        size_t before = pos_;
        uint64_t id = read_id(ok);
        if (!ok) {
            pos_ = before;
            return false;
        }
        uint64_t size = read_size(ok, unknown);
        if (!ok) {
            pos_ = before;
            return false;
        }
        if (id == kCluster) {
            enter_cluster(size, unknown);
            return true;
        }
        if (id == kSegment) {
            // Descend into the segment (its children are parsed from here);
            // with a known size, the trailing elements after it are harmless
            // no-ops at this level.
            return true;
        }
        if (unknown) {
            throw std::runtime_error("Unsupported MKV element of unknown size");
        }
        if (buffer_.size() - pos_ < size) {
            pos_ = before;
            return false;
        }
        parse_segment_element(id, size, unknown, sink);
        pos_ += static_cast<size_t>(size);
        return true;
    }

    // Inside a cluster.
    if (buffer_.size() - pos_ < 1)
        return false;
    if (!cluster_unknown_size_ && cluster_remaining_ == 0) {
        state_ = State::Segment;
        return true;
    }
    bool ok = false, unknown = false;
    size_t before = pos_;
    uint64_t id = read_id(ok);
    if (!ok) {
        pos_ = before;
        return false;
    }
    if (id == kCluster) {
        // A new cluster ends the previous one (streamed clusters have no end).
        state_ = State::Segment;
        pos_ = before;
        return true;
    }
    size_t after_id = pos_;
    if (!cluster_unknown_size_) {
        // A child that does not fit the remaining size also ends the cluster.
        uint64_t size_probe = read_size(ok, unknown);
        if (!ok) {
            pos_ = before;
            return false;
        }
        if (unknown || size_probe > cluster_remaining_) {
            state_ = State::Segment;
            pos_ = before;
            return true;
        }
        pos_ = after_id; // Rewind past the id only: the size is re-read.
    }
    uint64_t size = read_size(ok, unknown);
    if (!ok) {
        pos_ = before;
        return false;
    }
    if (!unknown) {
        if (buffer_.size() - pos_ < size) {
            pos_ = before;
            return false;
        }
        if (!cluster_unknown_size_)
            cluster_remaining_ -= size + (pos_ - before);
    }
    parse_cluster_element(id, size, unknown, sink);
    if (!unknown)
        pos_ += static_cast<size_t>(size);
    return true;
}

void MkvDemuxer::parse_segment_element(uint64_t id, uint64_t size, bool, const DemuxSink &) {
    if (id == kInfo) {
        // Scan the Info element for the timestamp scale.
        std::string_view data = as_view(buffer_, pos_, size);
        size_t offset = 0;
        while (offset + 2 <= data.size()) {
            bool ok = true, unknown = false;
            // Local reads over the element bytes.
            auto read_local_id = [&](size_t &at) -> uint64_t {
                unsigned char first = static_cast<unsigned char>(data[at]);
                int length = 1;
                for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
                    ++length;
                uint64_t value = first;
                for (int i = 1; i < length && at + i < data.size(); ++i) {
                    value = (value << 8) | static_cast<unsigned char>(data[at + i]);
                }
                at += static_cast<size_t>(length);
                return value;
            };
            auto read_local_size = [&](size_t &at, bool &unk) -> uint64_t {
                unsigned char first = static_cast<unsigned char>(data[at]);
                int length = 1;
                for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
                    ++length;
                if (at + length > data.size()) {
                    ok = false;
                    return 0;
                }
                uint64_t value = first & (0xFF >> length);
                for (int i = 1; i < length; ++i) {
                    value = (value << 8) | static_cast<unsigned char>(data[at + i]);
                }
                at += static_cast<size_t>(length);
                unk = value == (static_cast<uint64_t>(1) << (7 * length)) - 1;
                return value;
            };
            size_t at = offset;
            uint64_t child_id = read_local_id(at);
            uint64_t child_size = read_local_size(at, unknown);
            if (!ok)
                break;
            if (child_id == kTimestampScale && !unknown && at + child_size <= data.size()) {
                uint64_t scale = 0;
                for (uint64_t i = 0; i < child_size; ++i) {
                    scale = (scale << 8) | static_cast<unsigned char>(data[at + i]);
                }
                if (scale != 0)
                    timestamp_scale_ = scale;
                return;
            }
            offset = at + static_cast<size_t>(child_size);
        }
    } else if (id == kTracks) {
        parse_tracks(as_view(buffer_, pos_, size));
    }
    // SeekHead/Cues/Chapters/Tags/Attachments: skipped.
}

void MkvDemuxer::parse_tracks(std::string_view data) {
    // Walk TrackEntry elements.
    size_t offset = 0;
    auto read_header = [&](size_t &at, uint64_t &id, uint64_t &size, bool &unknown) -> bool {
        if (at >= data.size())
            return false;
        unsigned char first = static_cast<unsigned char>(data[at]);
        int length = 1;
        for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
            ++length;
        if (at + length > data.size())
            return false;
        id = first;
        for (int i = 1; i < length; ++i) {
            id = (id << 8) | static_cast<unsigned char>(data[at + i]);
        }
        at += static_cast<size_t>(length);
        if (at >= data.size())
            return false;
        first = static_cast<unsigned char>(data[at]);
        length = 1;
        for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
            ++length;
        if (at + length > data.size())
            return false;
        size = first & (0xFF >> length);
        for (int i = 1; i < length; ++i) {
            size = (size << 8) | static_cast<unsigned char>(data[at + i]);
        }
        at += static_cast<size_t>(length);
        unknown = size == (static_cast<uint64_t>(1) << (7 * length)) - 1;
        return true;
    };

    while (offset + 2 <= data.size()) {
        uint64_t id = 0, size = 0;
        bool unknown = false;
        if (!read_header(offset, id, size, unknown))
            break;
        size_t content = offset;
        size_t content_end = std::min(data.size(), offset + static_cast<size_t>(size));
        if (id == kTrackEntry && !unknown) {
            Track track;
            std::string codec;
            size_t at = content;
            while (at + 2 <= content_end) {
                uint64_t child_id = 0, child_size = 0;
                if (!read_header(at, child_id, child_size, unknown))
                    break;
                size_t value = at;
                at += static_cast<size_t>(child_size);
                if (at > content_end)
                    break;
                if (child_id == kTrackNumber && child_size <= 8) {
                    track.number = 0;
                    for (size_t i = 0; i < child_size; ++i) {
                        track.number =
                            (track.number << 8) | static_cast<unsigned char>(data[value + i]);
                    }
                } else if (child_id == kTrackType && child_size == 1) {
                    int type = static_cast<unsigned char>(data[value]);
                    track.video = type == TRACK_TYPE_VIDEO;
                } else if (child_id == kCodecId) {
                    codec.assign(data.substr(value, static_cast<size_t>(child_size)));
                } else if (child_id == kCodecPrivate) {
                    track.config.assign(data.substr(value, static_cast<size_t>(child_size)));
                    if (track.video && track.config.size() >= 6) {
                        // The AVCDecoderConfigurationRecord carries the NAL
                        // unit length prefix size.
                        track.length_size =
                            (static_cast<unsigned char>(track.config[4]) & 0x03) + 1;
                    }
                }
            }
            // Only H.264 video and AAC audio are publishable; the codec id
            // may precede the track type, so the filter runs at entry end.
            if (track.video && codec != "V_MPEG4/ISO/AVC") {
                if (warner_) {
                    warner_("An MKV video track (" + codec +
                            ") is not H.264: Telegram's RTMP server accepts H.264 only, "
                            "publishing continues without it");
                }
                track.number = 0;
            }
            if (!track.video && codec.rfind("A_AAC", 0) != 0 && !codec.empty()) {
                if (warner_) {
                    warner_("An MKV audio track (" + codec +
                            ") is not AAC: Telegram's RTMP server accepts AAC only, "
                            "publishing continues without it");
                }
                track.number = 0;
            }
            if (track.number != 0) {
                // H.264 tracks without CodecPrivate still publish through
                // in-band SPS/PPS (like the TS path).
                tracks_.push_back(std::move(track));
            }
        }
        offset = content_end;
    }
    if (tracks_.empty()) {
        fail("The MKV file carries no H.264 video or AAC audio track that Telegram's "
             "RTMP server accepts; re-encode it, e.g. with ffmpeg -i input -c:v libx264 "
             "-c:a aac output.mkv");
    }
}

void MkvDemuxer::enter_cluster(uint64_t size, bool unknown) {
    state_ = State::Cluster;
    have_cluster_ticks_ = false;
    cluster_ticks_ = 0;
    cluster_remaining_ = unknown ? 0 : size;
    cluster_unknown_size_ = unknown;
}

void MkvDemuxer::parse_cluster_element(uint64_t id, uint64_t size, bool unknown,
                                       const DemuxSink &sink) {
    if (unknown) {
        // Only SimpleBlock/BlockGroup of unknown size would be pathological;
        // bail out rather than misparse the stream.
        throw std::runtime_error("Unsupported MKV block of unknown size");
    }
    if (id == kClusterTimestamp && size <= 8) {
        cluster_ticks_ = 0;
        for (uint64_t i = 0; i < size; ++i) {
            cluster_ticks_ = (cluster_ticks_ << 8) | static_cast<unsigned char>(buffer_[pos_ + i]);
        }
        have_cluster_ticks_ = true;
    } else if (id == kSimpleBlock) {
        if (have_cluster_ticks_)
            parse_block(as_view(buffer_, pos_, size), true, sink);
    } else if (id == kBlockGroup) {
        // Find the Block inside the group; a group without a ReferenceBlock
        // holds a keyframe.
        std::string_view data = as_view(buffer_, pos_, size);
        size_t at = 0;
        bool keyframe = true;
        std::string_view block;
        while (at + 2 <= data.size()) {
            unsigned char first = static_cast<unsigned char>(data[at]);
            int length = 1;
            for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
                ++length;
            uint64_t child_id = first;
            for (int i = 1; i < length && at + i < data.size(); ++i) {
                child_id = (child_id << 8) | static_cast<unsigned char>(data[at + i]);
            }
            at += static_cast<size_t>(length);
            if (at >= data.size())
                break;
            first = static_cast<unsigned char>(data[at]);
            length = 1;
            for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
                ++length;
            if (at + length > data.size())
                break;
            uint64_t child_size = first & (0xFF >> length);
            for (int i = 1; i < length; ++i) {
                child_size = (child_size << 8) | static_cast<unsigned char>(data[at + i]);
            }
            at += static_cast<size_t>(length);
            if (at + child_size > data.size())
                break;
            if (child_id == kBlock)
                block = data.substr(at, static_cast<size_t>(child_size));
            if (child_id == kReferenceBlock)
                keyframe = false;
            at += static_cast<size_t>(child_size);
        }
        if (!block.empty() && have_cluster_ticks_)
            parse_block(block, false, sink, keyframe);
    }
}

void MkvDemuxer::parse_block(std::string_view block, bool simple, const DemuxSink &sink,
                             bool group_keyframe) {
    if (block.empty())
        return;
    size_t at = 0;
    // Track number: a VINT whose value is the track number.
    unsigned char first = static_cast<unsigned char>(block[at]);
    int length = 1;
    for (int mask = 0x80; mask > 0 && !(first & mask); mask >>= 1)
        ++length;
    if (at + length + 3 > block.size())
        return;
    uint64_t number = first & (0xFF >> length);
    for (int i = 1; i < length; ++i) {
        number = (number << 8) | static_cast<unsigned char>(block[at + i]);
    }
    at += static_cast<size_t>(length);

    const Track *track = nullptr;
    for (const Track &t : tracks_) {
        if (t.number == number)
            track = &t;
    }
    if (track == nullptr)
        return; // A track we do not publish (or unknown).

    int64_t timecode = static_cast<int8_t>(block[at]); // Sign-extended high byte.
    timecode = (timecode << 8) | static_cast<unsigned char>(block[at + 1]);
    at += 2;
    if (timecode < -32768 || timecode > 32767)
        timecode = static_cast<int16_t>(timecode);
    unsigned char flags = static_cast<unsigned char>(block[at]);
    at += 1;
    bool keyframe = simple ? (flags & 0x80) != 0 : group_keyframe;
    int lacing = (flags >> 1) & 0x03;

    std::string_view payload = block.substr(at);
    int count = 1;
    if (lacing != 0) {
        if (payload.empty())
            return;
        count = static_cast<unsigned char>(payload[0]) + 1;
        payload = payload.substr(1);
    }
    int64_t base_ns =
        (have_cluster_ticks_ ? cluster_ticks_ * static_cast<int64_t>(timestamp_scale_) : 0) +
        timecode * static_cast<int64_t>(timestamp_scale_);

    if (lacing == 0) {
        emit_frame(*track, payload, base_ns, keyframe, 0, sink);
        return;
    }
    if (lacing == 2) { // Fixed-size lacing.
        size_t frame_size = payload.size() / static_cast<size_t>(count);
        for (int i = 0; i < count; ++i) {
            emit_frame(*track, payload.substr(static_cast<size_t>(i) * frame_size, frame_size),
                       base_ns, keyframe, static_cast<size_t>(i), sink);
        }
        return;
    }
    // Xiph or EBML lacing: read the sizes of all but the last frame.
    std::vector<size_t> sizes(static_cast<size_t>(count));
    size_t offset = 0;
    if (lacing == 1) { // Xiph.
        for (int i = 0; i < count - 1; ++i) {
            size_t value = 0;
            unsigned char byte = 0xFF;
            while (offset < payload.size() && byte == 0xFF) {
                byte = static_cast<unsigned char>(payload[offset++]);
                value += byte;
            }
            sizes[static_cast<size_t>(i)] = value;
        }
    } else { // EBML lacing.
        for (int i = 0; i < count - 1; ++i) {
            if (offset >= payload.size())
                return;
            unsigned char byte = static_cast<unsigned char>(payload[offset++]);
            int vlength = 1;
            for (int mask = 0x80; mask > 0 && !(byte & mask); mask >>= 1)
                ++vlength;
            uint64_t value = byte & (0xFF >> vlength);
            bool read_ok = static_cast<size_t>(vlength - 1) <= payload.size() - offset;
            for (int j = 1; j < vlength && read_ok; ++j) {
                value = (value << 8) | static_cast<unsigned char>(payload[offset++]);
            }
            if (!read_ok)
                return;
            if (i > 0) {
                // Sizes after the first are relative deltas.
                int64_t signed_value = static_cast<int64_t>(
                    value - ((static_cast<uint64_t>(1) << (7 * vlength - 1)) - 1));
                value = static_cast<uint64_t>(
                    static_cast<int64_t>(sizes[static_cast<size_t>(i) - 1]) + signed_value);
            }
            sizes[static_cast<size_t>(i)] = static_cast<size_t>(value);
        }
    }
    size_t consumed = 0;
    for (int i = 0; i < count - 1; ++i) {
        if (consumed + sizes[static_cast<size_t>(i)] > payload.size())
            return;
        emit_frame(*track, payload.substr(consumed, sizes[static_cast<size_t>(i)]), base_ns,
                   keyframe, static_cast<size_t>(i), sink);
        consumed += sizes[static_cast<size_t>(i)];
    }
    emit_frame(*track, payload.substr(consumed), base_ns, keyframe, static_cast<size_t>(count - 1),
               sink);
}

void MkvDemuxer::emit_frame(const Track &track, std::string_view frame, int64_t timestamp_ns,
                            bool keyframe, size_t lace_index, const DemuxSink &sink) {
    if (frame.empty())
        return;
    DemuxFrame out;
    int64_t timestamp_ms = timestamp_ns / 1000000;
    if (!track.video && lace_index != 0) {
        // Laced audio frames follow each other in time: advance by the AAC
        // frame duration (1024 samples).
        int rate = 48000;
        try {
            rate = flv::sample_rate(track.config);
        } catch (const std::exception &) {
        }
        timestamp_ms += static_cast<int64_t>(lace_index) * 1024 * 1000 / rate;
    }
    out.dts_ms = out.pts_ms = timestamp_ms;

    if (track.video) {
        out.kind = DemuxFrame::Kind::Video;
        out.keyframe = keyframe;
        bool annexb = frame.size() >= 4 && ((static_cast<unsigned char>(frame[0]) == 0 &&
                                             static_cast<unsigned char>(frame[1]) == 0 &&
                                             static_cast<unsigned char>(frame[2]) == 1) ||
                                            (static_cast<unsigned char>(frame[0]) == 0 &&
                                             static_cast<unsigned char>(frame[1]) == 0 &&
                                             static_cast<unsigned char>(frame[2]) == 0 &&
                                             static_cast<unsigned char>(frame[3]) == 1));
        if (annexb) {
            out.data = std::string(frame);
        } else {
            // Length-prefixed (AVCC) NAL units → Annex-B.
            size_t length_size = track.length_size;
            std::string &annexb_out = out.data;
            size_t offset = 0;
            while (offset + length_size <= frame.size()) {
                uint64_t length = 0;
                for (size_t i = 0; i < length_size; ++i) {
                    length = (length << 8) | static_cast<unsigned char>(frame[offset + i]);
                }
                offset += length_size;
                if (length == 0 || offset + length > frame.size())
                    break;
                annexb_out.append("\x00\x00\x00\x01", 4);
                annexb_out.append(frame.substr(offset, static_cast<size_t>(length)));
                offset += static_cast<size_t>(length);
            }
        }
        if (keyframe) {
            if (!track.config.empty()) {
                out.config = track.config;
            } else {
                // In-band parameter sets (like the TS path).
                try {
                    out.config = h264::describe(out.data).record;
                } catch (const std::exception &) {
                }
            }
        }
    } else {
        out.kind = DemuxFrame::Kind::Audio;
        out.data = std::string(frame);
        out.config = track.config; // The AudioSpecificConfig of the track.
    }
    sink(std::move(out));
}

} // namespace tgrtmp
