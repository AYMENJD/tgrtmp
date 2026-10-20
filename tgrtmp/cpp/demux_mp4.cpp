#include "demux_mp4.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "h264.h"

namespace tgrtmp {

namespace {

constexpr uint32_t operator""_box(char c) = delete;

constexpr uint32_t make_box_type(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(d));
}

constexpr uint32_t kMoov = make_box_type('m', 'o', 'o', 'v');
constexpr uint32_t kMoof = make_box_type('m', 'o', 'o', 'f');
constexpr uint32_t kMdat = make_box_type('m', 'd', 'a', 't');
constexpr uint32_t kTrak = make_box_type('t', 'r', 'a', 'k');
constexpr uint32_t kMdia = make_box_type('m', 'd', 'i', 'a');
constexpr uint32_t kMinf = make_box_type('m', 'i', 'n', 'f');
constexpr uint32_t kMvex = make_box_type('m', 'v', 'e', 'x');
constexpr uint32_t kMdhd = make_box_type('m', 'd', 'h', 'd');
constexpr uint32_t kHdlr = make_box_type('h', 'd', 'l', 'r');
constexpr uint32_t kTkhd = make_box_type('t', 'k', 'h', 'd');
constexpr uint32_t kTrex = make_box_type('t', 'r', 'e', 'x');
constexpr uint32_t kTraf = make_box_type('t', 'r', 'a', 'f');
constexpr uint32_t kTfhd = make_box_type('t', 'f', 'h', 'd');
constexpr uint32_t kTrun = make_box_type('t', 'r', 'u', 'n');
constexpr uint32_t kStbl = make_box_type('s', 't', 'b', 'l');
constexpr uint32_t kStsd = make_box_type('s', 't', 's', 'd');
constexpr uint32_t kStts = make_box_type('s', 't', 't', 's');
constexpr uint32_t kCtts = make_box_type('c', 't', 't', 's');
constexpr uint32_t kStsc = make_box_type('s', 't', 's', 'c');
constexpr uint32_t kStco = make_box_type('s', 't', 'c', 'o');
constexpr uint32_t kCo64 = make_box_type('c', 'o', '6', '4');
constexpr uint32_t kStsz = make_box_type('s', 't', 's', 'z');
constexpr uint32_t kStss = make_box_type('s', 't', 's', 's');
constexpr uint32_t kAvc1 = make_box_type('a', 'v', 'c', '1');
constexpr uint32_t kAvc3 = make_box_type('a', 'v', 'c', '3');
constexpr uint32_t kAvcC = make_box_type('a', 'v', 'c', 'C');
constexpr uint32_t kMp4a = make_box_type('m', 'p', '4', 'a');
constexpr uint32_t kEsds = make_box_type('e', 's', 'd', 's');
constexpr uint32_t kHvc1 = make_box_type('h', 'v', 'c', '1');
constexpr uint32_t kHev1 = make_box_type('h', 'e', 'v', '1');

uint32_t rd32(std::string_view data, size_t offset) {
    return (static_cast<uint32_t>(static_cast<unsigned char>(data[offset])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(data[offset + 3]));
}

uint64_t rd64(std::string_view data, size_t offset) {
    return (static_cast<uint64_t>(rd32(data, offset)) << 32) | rd32(data, offset + 4);
}

int32_t rds32(std::string_view data, size_t offset) {
    return static_cast<int32_t>(rd32(data, offset));
}

// Reads one full box header at `offset`; returns false if more data is
// needed. `size` includes the header.
bool read_box_header(std::string_view data, size_t offset, uint64_t &size, uint32_t &type,
                     size_t &header) {
    if (data.size() - offset < 8)
        return false;
    size = rd32(data, offset);
    type = rd32(data, offset + 4);
    header = 8;
    if (size == 1) {
        if (data.size() - offset < 16)
            return false;
        size = rd64(data, offset + 8);
        header = 16;
    } else if (size == 0) {
        size = data.size() - offset; // To the end of this data.
    }
    return size >= header;
}

size_t descriptor_length(std::string_view data, size_t &offset) {
    size_t length = 0;
    while (offset < data.size()) {
        unsigned char byte = static_cast<unsigned char>(data[offset++]);
        length = (length << 7) | (byte & 0x7F);
        if (!(byte & 0x80))
            break;
    }
    return length;
}

} // namespace

void Mp4Demuxer::fail(const std::string &message) { throw std::runtime_error(message); }

void Mp4Demuxer::feed(std::string_view data, const DemuxSink &sink) {
    buffer_.append(data);
    process(sink);
}

void Mp4Demuxer::prime(std::string_view moov_box, const DemuxSink &sink) {
    if (!moov_parsed_) {
        uint64_t size = 0;
        uint32_t type = 0;
        size_t header = 0;
        if (!read_box_header(moov_box, 0, size, type, header) || type != kMoov)
            fail("The primed tail is not a moov box");
        primed_ = true;
        parse_moov(moov_box.substr(header), sink);
    }

    // The blind pass may have buffered the whole file: with the table in
    // hand those bytes are dead weight, since the caller replays the media
    // from offset 0 through feed_at().
    buffer_.clear();
    at_offset_ = 0;
    at_set_ = false;
}

void Mp4Demuxer::feed_at(std::string_view data, uint64_t offset, const DemuxSink &sink) {
    if (!moov_parsed_)
        fail("The MP4 demuxer was fed media before its sample table was primed");

    // The windows must tile the file: a sample may straddle feeds, so the
    // straddling bytes stay buffered here until the next window completes
    // them; the same continuity the mdat streaming path keeps.
    if (!at_set_) {
        at_offset_ = offset;
        at_set_ = true;
    } else if (offset != at_offset_ + buffer_.size()) {
        fail("The offset feeds of a primed MP4 are not contiguous (got " +
             std::to_string(offset) + ", expected " + std::to_string(at_offset_ + buffer_.size()) +
             ")");
    }

    buffer_.append(data);

    size_t covered = emit_from_mdat(buffer_, at_offset_, sink);

    if (covered > 0) {
        buffer_.erase(0, covered);
        at_offset_ += covered;
    }
}

void Mp4Demuxer::finish(const DemuxSink &) {
    // Samples are only emitted with their bytes in hand; nothing to flush.
}

void Mp4Demuxer::process(const DemuxSink &sink) {
    // Continue an mdat that spans feeds first: its bytes stream through, but
    // never past the first sample that is not complete yet; a sample may
    // straddle feeds, so the window stops where it starts.
    while (mdat_open_) {
        size_t available = static_cast<size_t>(std::min<uint64_t>(buffer_.size(), mdat_remaining_));
        if (available == 0)
            return;
        size_t covered =
            emit_from_mdat(std::string_view(buffer_).substr(0, available), mdat_position_, sink);
        if (covered > 0) {
            buffer_.erase(0, covered);
            position_ += covered;
            mdat_position_ += covered;
            mdat_remaining_ -= covered;
        }
        if (mdat_remaining_ == 0) {
            mdat_open_ = false;
            prev_fragment_end_ = last_emitted_end_;
        } else {
            return; // Need the next feed (or the next sample's bytes).
        }
    }

    size_t offset = 0;
    while (true) {
        std::string_view view(buffer_);
        if (view.size() - offset < 8)
            break;
        uint64_t size = 0;
        uint32_t type = 0;
        size_t header = 0;
        if (!read_box_header(view, offset, size, type, header))
            break;

        if (view.size() - offset < size) {
            // moov/moof are small; wait for them whole. mdat (potentially the
            // rest of the file) streams through.
            if (type == kMdat) {
                mdat_open_ = true;
                mdat_remaining_ = size - header;
                mdat_position_ = position_ + offset + header;
                // Re-enter through the mdat streaming path, consuming what we
                // have of this box right now, the header included.
                buffer_.erase(0, offset + header);
                position_ += offset + header;
                process(sink);
                return;
            }
            break;
        }

        std::string_view box = view.substr(offset, static_cast<size_t>(size));
        if (type == kMoov) {
            moov_end_ = position_ + offset + size;
            parse_moov(box.substr(header), sink);
        } else if (type == kMoof) {
            // The default fragment base (default-base-is-moof) is the moof
            // box start, header included (ISO 14496-12 §8.8.7).
            parse_moof(box.substr(header), position_ + offset, sink);
        } else if (type == kMdat) {
            emit_from_mdat(box.substr(header), position_ + offset + header, sink);
            prev_fragment_end_ = last_emitted_end_;
        }
        // ftyp/styp/free/skip/prft/sidx/emsg/etc: skipped.
        offset += static_cast<size_t>(size);
    }
    buffer_.erase(0, offset);
    position_ += offset;
}

void Mp4Demuxer::parse_moov(std::string_view box, const DemuxSink &) {
    if (moov_parsed_)
        return;
    moov_parsed_ = true;

    struct Level {
        std::string_view data;
        size_t offset = 0;
    };
    std::vector<Level> stack;
    stack.push_back({box, 0});
    while (!stack.empty()) {
        Level &level = stack.back();
        if (level.data.size() - level.offset < 8) {
            stack.pop_back();
            continue;
        }
        uint64_t size = 0;
        uint32_t type = 0;
        size_t header = 0;
        if (!read_box_header(level.data, level.offset, size, type, header) ||
            level.data.size() - level.offset < size) {
            fail("Corrupt MP4 box inside the moov");
        }
        std::string_view content =
            level.data.substr(level.offset + header, static_cast<size_t>(size) - header);
        level.offset += static_cast<size_t>(size);

        switch (type) {
        case kTrak:
            tracks_.emplace_back();
            stack.push_back({content, 0});
            break;
        case kMdia:
        case kMinf:
            stack.push_back({content, 0});
            break;
        case kMvex:
            fragmented_ = true;
            break;
        case kMdhd:
            if (!tracks_.empty()) {
                tracks_.back().timescale = static_cast<unsigned char>(content[0]) == 1
                                               ? rd32(content, 20)
                                               : rd32(content, 12);
            }
            break;
        case kHdlr:
            if (!tracks_.empty()) {
                tracks_.back().video = rd32(content, 8) == make_box_type('v', 'i', 'd', 'e');
            }
            break;
        case kTkhd:
            if (!tracks_.empty()) {
                // version/flags(4) then v0: times(4+4), v1: times(8+8),
                // and only then the track id.
                tracks_.back().id = static_cast<int>(
                    rd32(content, static_cast<unsigned char>(content[0]) == 1 ? 20 : 12));
            }
            break;
        case kTrex:
            if (!tracks_.empty()) {
                int id = static_cast<int>(rd32(content, 4));
                for (Track &track : tracks_) {
                    if (track.id != id)
                        continue;
                    track.default_duration = rd32(content, 12);
                    track.default_size = rd32(content, 16);
                    track.default_flags = rd32(content, 20);
                }
            }
            break;
        case kStsd:
            if (!tracks_.empty())
                parse_stsd(tracks_.back(), content);
            break;
        case kStbl:
            if (!tracks_.empty())
                parse_stbl(tracks_.back(), content);
            break;
        default:
            break;
        }
    }

    bool has_video = false, has_audio = false;
    for (const Track &track : tracks_) {
        if (track.video) {
            has_video = true;
            if (track.config.empty()) {
                fail("The MP4 file carries video but no H.264 track that Telegram's RTMP server "
                     "accepts");
            }
        } else if (!track.config.empty()) {
            has_audio = true;
        }
    }
    if (!has_video && !has_audio) {
        fail("The MP4 file carries no H.264/AAC track that Telegram's RTMP server accepts");
    }

    if (!fragmented_) {
        // Progressive: expand every track's samples into the pending list,
        // ordered by file offset.
        for (size_t index = 0; index < tracks_.size(); ++index) {
            for (const Sample &sample : tracks_[index].samples) {
                pending_.push_back(SampleOut{sample.offset, sample.size, sample.dts,
                                             sample.dts + sample.cts, sample.sync, index});
            }
        }
        std::stable_sort(
            pending_.begin(), pending_.end(),
            [](const SampleOut &a, const SampleOut &b) { return a.offset < b.offset; });
        // A primed table describes media the caller will replay from its
        // start, so samples before the moov are expected here.
        if (!primed_ && !pending_.empty() && pending_.front().offset < moov_end_) {
            fail("The MP4 file keeps its moov after its media, which a live stream cannot read; "
                 "remux it first, e.g. with ffmpeg -i input -c copy -movflags +faststart out.mp4");
        }
    }
}

void Mp4Demuxer::parse_stsd(Track &track, std::string_view stsd) {
    if (stsd.size() < 8)
        return;
    size_t count = rd32(stsd, 4);
    size_t offset = 8;
    for (size_t i = 0; i < count && offset + 8 <= stsd.size(); ++i) {
        uint64_t size = 0;
        uint32_t format = 0;
        size_t header = 0;
        if (!read_box_header(stsd, offset, size, format, header))
            break;
        if (stsd.size() - offset < size)
            break;
        std::string_view entry = stsd.substr(offset + header, static_cast<size_t>(size) - header);

        if (format == kAvc1 || format == kAvc3) {
            track.video = true;
            // Walk the child boxes of the sample entry. The fixed fields
            // before them differ between muxers (the ISO layout ends at 74,
            // QuickTime-flavoured files carry extra version fields), so try
            // each plausible offset and keep the walk that tiles the entry.
            struct Child {
                uint32_t type = 0;
                std::string_view body;
            };
            auto walk_children = [&](size_t start) -> std::vector<Child> {
                // Walk while valid boxes tile; trailing padding is tolerated.
                std::vector<Child> children;
                size_t at = start;
                while (at + 8 <= entry.size()) {
                    uint64_t size = 0;
                    uint32_t type = 0;
                    size_t header = 0;
                    if (!read_box_header(entry, at, size, type, header))
                        break;
                    if (size < header || at + size > entry.size())
                        break;
                    children.push_back(
                        {type, entry.substr(at + header, static_cast<size_t>(size) - header)});
                    at += static_cast<size_t>(size);
                }
                return children;
            };
            for (size_t start : {size_t(78), size_t(74), size_t(82), size_t(32)}) {
                for (const Child &child : walk_children(start)) {
                    if (child.type == kAvcC) {
                        track.config.assign(child.body);
                        // lengthSizeMinusOne lives in the record's 6th byte.
                        if (track.config.size() >= 6) {
                            track.length_size =
                                (static_cast<unsigned char>(track.config[4]) & 0x03) + 1;
                        }
                    }
                }
                if (!track.config.empty())
                    break;
            }
            if (track.config.empty()) {
                fail("The H.264 track of the MP4 file carries no avcC record");
            }
        } else if (format == kMp4a) {
            track.video = false;
            struct Child {
                uint32_t type = 0;
                std::string_view body;
            };
            auto walk_children = [&](size_t start) -> std::vector<Child> {
                std::vector<Child> children;
                size_t at = start;
                while (at + 8 <= entry.size()) {
                    uint64_t size = 0;
                    uint32_t type = 0;
                    size_t header = 0;
                    if (!read_box_header(entry, at, size, type, header))
                        break;
                    if (size < header || at + size > entry.size())
                        break;
                    children.push_back(
                        {type, entry.substr(at + header, static_cast<size_t>(size) - header)});
                    at += static_cast<size_t>(size);
                }
                return children;
            };
            for (size_t start : {size_t(28), size_t(24), size_t(32), size_t(36)}) {
                for (const Child &child : walk_children(start)) {
                    if (child.type == kEsds) {
                        parse_esds(track, child.body);
                    }
                }
                if (!track.config.empty())
                    break;
            }
        } else if (format == kHvc1 || format == kHev1) {
            fail("The MP4 file carries H.265/HEVC video, which Telegram's RTMP server does not "
                 "accept; please use an H.264 stream");
        }
        offset += static_cast<size_t>(size);
    }
}

void Mp4Demuxer::parse_esds(Track &track, std::string_view esds) {
    // version/flags(4), then the descriptor chain (ISO 14496-1). The
    // DecoderSpecificInfo that carries the AudioSpecificConfig is a child
    // of the DecoderConfigDescriptor, which is a child of the ES descriptor.
    if (esds.size() < 4)
        return;
    size_t offset = 4;
    while (offset < esds.size()) {
        unsigned char tag = static_cast<unsigned char>(esds[offset++]);
        size_t length = descriptor_length(esds, offset);
        size_t end = std::min(offset + length, esds.size());
        switch (tag) {
        case 0x03: { // ES_Descriptor: ES_ID(2) + flags(1), children follow.
            if (offset + 3 > end)
                return;
            offset += 2;
            unsigned char flags = static_cast<unsigned char>(esds[offset]);
            offset += 1;
            if (flags & 0x80)
                offset += 2; // streamDependenceFlag.
            if (flags & 0x40)
                offset += 2; // URL_Flag (approximate: 1+1).
            if (flags & 0x20)
                offset += 2; // OCRstreamFlag.
            break;
        }
        case 0x04: { // DecoderConfigDescriptor: 13 fixed bytes, then
                     // its children (the DecoderSpecificInfo).
            if (offset + 13 > end)
                return;
            unsigned char object_type = static_cast<unsigned char>(esds[offset]);
            if (object_type != 0x40 && object_type != 0x66 && object_type != 0x67 &&
                object_type != 0x68) {
                // Not AAC: the track stays unpublishable; say so.
                if (warner_) {
                    warner_("The MP4 audio track is not AAC (object type " +
                            std::to_string(object_type) +
                            "): Telegram's RTMP server accepts AAC only, publishing "
                            "continues without audio");
                }
                return;
            }
            offset += 13;
            break;
        }
        case 0x05: { // DecoderSpecificInfo: the AudioSpecificConfig.
            track.config.assign(esds.substr(offset, end - offset));
            return;
        }
        default:
            offset = end; // SLConfigDescriptor and friends: skipped.
            break;
        }
    }
}

void Mp4Demuxer::parse_stbl(Track &track, std::string_view stbl) {
    struct Entry2 {
        uint32_t a, b;
    };
    std::vector<Entry2> stts, ctts, stsc;
    std::vector<uint64_t> chunk_offsets;
    std::vector<uint32_t> sizes;
    std::vector<uint32_t> sync;

    struct Level {
        std::string_view data;
        size_t offset = 0;
    };
    std::vector<Level> stack;
    stack.push_back({stbl, 0});
    while (!stack.empty()) {
        Level &level = stack.back();
        if (level.data.size() - level.offset < 8) {
            stack.pop_back();
            continue;
        }
        uint64_t size = 0;
        uint32_t type = 0;
        size_t header = 0;
        if (!read_box_header(level.data, level.offset, size, type, header) ||
            level.data.size() - level.offset < size) {
            stack.pop_back();
            continue;
        }
        std::string_view content =
            level.data.substr(level.offset + header, static_cast<size_t>(size) - header);
        level.offset += static_cast<size_t>(size);

        auto read_entries = [&](size_t entry_size) {
            size_t count = rd32(content, 4);
            std::vector<Entry2> entries;
            for (size_t i = 0; i < count && 8 + i * entry_size + entry_size <= content.size();
                 ++i) {
                entries.push_back(
                    {rd32(content, 8 + i * entry_size), rd32(content, 8 + i * entry_size + 4)});
            }
            return entries;
        };
        switch (type) {
        case kStsd:
            parse_stsd(track, content);
            break;
        case kStts:
            stts = read_entries(8);
            break;
        case kCtts:
            // Offsets are stored raw; the version-1 signedness is applied
            // when they are consumed.
            ctts = read_entries(8);
            break;
        case kStsc:
            stsc = read_entries(12);
            break;
        case kStco: {
            size_t count = rd32(content, 4);
            for (size_t i = 0; i < count && 8 + i * 4 + 4 <= content.size(); ++i) {
                chunk_offsets.push_back(rd32(content, 8 + i * 4));
            }
            break;
        }
        case kCo64: {
            size_t count = rd32(content, 4);
            for (size_t i = 0; i < count && 8 + i * 8 + 8 <= content.size(); ++i) {
                chunk_offsets.push_back(rd64(content, 8 + i * 8));
            }
            break;
        }
        case kStsz: {
            uint32_t uniform = rd32(content, 4);
            size_t count = rd32(content, 8);
            if (uniform != 0) {
                sizes.assign(count, uniform);
            } else {
                for (size_t i = 0; i < count && 12 + i * 4 + 4 <= content.size(); ++i) {
                    sizes.push_back(rd32(content, 12 + i * 4));
                }
            }
            break;
        }
        case kStss: {
            size_t count = rd32(content, 4);
            for (size_t i = 0; i < count && 8 + i * 4 + 4 <= content.size(); ++i) {
                sync.push_back(rd32(content, 8 + i * 4));
            }
            break;
        }
        default:
            break;
        }
    }

    if (sizes.empty() || chunk_offsets.empty())
        return;
    track.samples.reserve(sizes.size());
    int64_t dts = 0;
    size_t sample_index = 0;
    size_t stts_index = 0, stts_left = stts.empty() ? 0 : stts[0].a;
    size_t ctts_index = 0, ctts_left = ctts.empty() ? 0 : ctts[0].a;
    int64_t ctts_value = 0;
    for (size_t chunk = 0; chunk < chunk_offsets.size() && sample_index < sizes.size(); ++chunk) {
        uint32_t per_chunk = 0;
        for (size_t i = stsc.size(); i > 0; --i) {
            if (stsc[i - 1].a <= chunk + 1) {
                per_chunk = stsc[i - 1].b;
                break;
            }
        }
        uint64_t offset = chunk_offsets[chunk];
        for (uint32_t s = 0; s < per_chunk && sample_index < sizes.size(); ++s) {
            while (stts_left == 0 && stts_index + 1 < stts.size()) {
                ++stts_index;
                stts_left = stts[stts_index].a;
            }
            uint32_t delta = stts_left ? stts[stts_index].b : 0;
            if (stts_left)
                --stts_left;
            while (ctts_left == 0 && ctts_index + 1 < ctts.size()) {
                ++ctts_index;
                ctts_left = ctts[ctts_index].a;
            }
            if (ctts_left) {
                ctts_value = static_cast<int32_t>(ctts[ctts_index].b);
                --ctts_left;
            }
            bool sync_sample = sync.empty()
                                   ? true
                                   : std::binary_search(sync.begin(), sync.end(), sample_index + 1);
            track.samples.push_back(
                Sample{offset, sizes[sample_index], dts, ctts_value, sync_sample});
            offset += sizes[sample_index];
            dts += delta;
            ++sample_index;
        }
    }
}

void Mp4Demuxer::parse_moof(std::string_view box, uint64_t moof_offset, const DemuxSink &) {
    if (!moov_parsed_)
        fail("The MP4 stream starts with a moof before its moov");
    fragmented_ = true;
    have_base_ = false;

    // Each traf carries its tfhd (the track and the fragment defaults) and
    // its truns; parse them per traf.
    struct Level {
        std::string_view data;
        size_t offset = 0;
    };

    auto walk_trafs = [&](auto &&handle_traf) {
        std::vector<Level> stack;
        stack.push_back({box, 0});
        while (!stack.empty()) {
            Level &level = stack.back();
            if (level.data.size() - level.offset < 8) {
                stack.pop_back();
                continue;
            }
            uint64_t size = 0;
            uint32_t type = 0;
            size_t header = 0;
            if (!read_box_header(level.data, level.offset, size, type, header) ||
                level.data.size() - level.offset < size) {
                stack.pop_back();
                continue;
            }
            std::string_view content =
                level.data.substr(level.offset + header, static_cast<size_t>(size) - header);
            level.offset += static_cast<size_t>(size);
            if (type == kTraf) {
                handle_traf(content);
            }
        }
    };

    walk_trafs([&](std::string_view traf) {
        Track *track = nullptr;
        uint32_t tfhd_flags = 0;
        // First pass: the tfhd of this traf.
        {
            std::vector<Level> stack;
            stack.push_back({traf, 0});
            while (!stack.empty()) {
                Level &level = stack.back();
                if (level.data.size() - level.offset < 8) {
                    stack.pop_back();
                    continue;
                }
                uint64_t size = 0;
                uint32_t type = 0;
                size_t header = 0;
                if (!read_box_header(level.data, level.offset, size, type, header) ||
                    level.data.size() - level.offset < size) {
                    stack.pop_back();
                    continue;
                }
                std::string_view content =
                    level.data.substr(level.offset + header, static_cast<size_t>(size) - header);
                level.offset += static_cast<size_t>(size);
                if (type == kTfhd) {
                    tfhd_flags = rd32(content, 0) & 0xFFFFFF;
                    int track_id = static_cast<int>(rd32(content, 4));
                    for (Track &t : tracks_) {
                        if (t.id == track_id)
                            track = &t;
                    }
                    if (track == nullptr)
                        return;
                    size_t offset = 8;
                    uint64_t base = 0;
                    bool explicit_base = false;
                    if (tfhd_flags & 0x01) {
                        base = rd64(content, offset);
                        offset += 8;
                        explicit_base = true;
                    }
                    if (tfhd_flags & 0x02)
                        offset += 4; // sample_description_index.
                    if (tfhd_flags & 0x08)
                        track->default_duration = rd32(content, offset), offset += 4;
                    if (tfhd_flags & 0x10)
                        track->default_size = rd32(content, offset), offset += 4;
                    if (tfhd_flags & 0x20)
                        track->default_flags = rd32(content, offset), offset += 4;
                    if (explicit_base) {
                        moof_base_ = base;
                        have_base_ = true;
                    } else if (tfhd_flags & 0x020000) { // default-base-is-moof.
                        moof_base_ = moof_offset;
                        have_base_ = true;
                    } else if (!have_base_) {
                        moof_base_ = prev_fragment_end_ ? prev_fragment_end_ : moof_offset;
                        have_base_ = true;
                    }
                }
            }
        }
        if (track == nullptr)
            return;

        // Second pass: the truns of this traf.
        std::vector<Level> stack;
        stack.push_back({traf, 0});
        while (!stack.empty()) {
            Level &level = stack.back();
            if (level.data.size() - level.offset < 8) {
                stack.pop_back();
                continue;
            }
            uint64_t size = 0;
            uint32_t type = 0;
            size_t header = 0;
            if (!read_box_header(level.data, level.offset, size, type, header) ||
                level.data.size() - level.offset < size) {
                stack.pop_back();
                continue;
            }
            std::string_view content =
                level.data.substr(level.offset + header, static_cast<size_t>(size) - header);
            level.offset += static_cast<size_t>(size);
            if (type != kTrun)
                continue;

            uint32_t flags = rd32(content, 0) & 0xFFFFFF;
            uint32_t version = rd32(content, 0) >> 24;
            size_t count = rd32(content, 4);
            size_t offset = 8;
            int64_t data_offset = 0;
            if (flags & 0x01) {
                data_offset = rds32(content, offset);
                offset += 4;
            }
            uint32_t first_flags = 0;
            if (flags & 0x04) {
                first_flags = rd32(content, offset);
                offset += 4;
            }
            uint64_t position =
                moof_base_ + static_cast<uint64_t>(std::max<int64_t>(0, data_offset));
            int64_t dts = track->dts;
            for (size_t i = 0; i < count && offset <= content.size(); ++i) {
                uint32_t duration = track->default_duration;
                uint32_t sample_size = track->default_size;
                uint32_t sample_flags = track->default_flags;
                int64_t cts = 0;
                if (flags & 0x100) {
                    duration = rd32(content, offset);
                    offset += 4;
                }
                if (flags & 0x200) {
                    sample_size = rd32(content, offset);
                    offset += 4;
                }
                if (flags & 0x400) {
                    sample_flags = rd32(content, offset);
                    offset += 4;
                }
                if (flags & 0x800) {
                    cts = version == 1 ? rds32(content, offset)
                                       : static_cast<int64_t>(rd32(content, offset));
                    offset += 4;
                }
                bool first = i == 0;
                uint32_t effective_flags = first && (flags & 0x04) ? first_flags : sample_flags;
                // sample_is_non_sync_sample (bit 16): clear means sync.
                bool sync = (effective_flags & 0x10000) == 0;
                if (track->video || !track->config.empty()) {
                    pending_.push_back(SampleOut{position, sample_size, dts, dts + cts, sync,
                                                 static_cast<size_t>(track - tracks_.data())});
                }
                position += sample_size;
                dts += duration;
            }
            track->dts = dts;
        }
    });

    std::stable_sort(pending_.begin(), pending_.end(),
                     [](const SampleOut &a, const SampleOut &b) { return a.offset < b.offset; });
}

size_t Mp4Demuxer::emit_from_mdat(std::string_view content, uint64_t content_start,
                                  const DemuxSink &sink) {
    // Emits every sample fully contained in [content_start, content_start +
    // content.size()); returns how many bytes of the window it covered, so a
    // sample that straddles the window keeps its bytes for the next feed.
    size_t covered = 0;
    while (!pending_.empty()) {
        const SampleOut &sample = pending_.front();
        if (sample.offset < content_start) {
            fail("The MP4 sample table points before the media of the stream; remux with "
                 "ffmpeg -i input -c copy -movflags +faststart output.mp4");
        }
        if (sample.offset + sample.size > content_start + content.size()) {
            covered = static_cast<size_t>(
                std::min<uint64_t>(sample.offset - content_start, content.size()));
            break;
        }
        emit_sample(sample, content.substr(sample.offset - content_start, sample.size), sink);
        covered = static_cast<size_t>(sample.offset - content_start) + sample.size;
        last_emitted_end_ = std::max(last_emitted_end_, sample.offset + sample.size);
        pending_.pop_front();
    }
    return covered;
}

void Mp4Demuxer::emit_sample(const SampleOut &sample, std::string_view bytes,
                             const DemuxSink &sink) {
    const Track &track = tracks_[sample.track];
    if (track.config.empty())
        return; // A codec we do not publish.

    DemuxFrame frame;
    int64_t timescale = track.timescale ? track.timescale : 1000;
    frame.dts_ms = sample.dts * 1000 / timescale;
    frame.pts_ms = sample.pts * 1000 / timescale;
    if (track.video) {
        frame.kind = DemuxFrame::Kind::Video;
        frame.keyframe = sample.sync;
        // Length-prefixed (AVCC) NAL units → Annex-B.
        size_t length_size = track.length_size ? track.length_size : 4;
        std::string &annexb = frame.data;
        size_t offset = 0;
        while (offset + length_size <= bytes.size()) {
            uint64_t length = 0;
            for (size_t i = 0; i < length_size; ++i) {
                length = (length << 8) | static_cast<unsigned char>(bytes[offset + i]);
            }
            offset += length_size;
            if (length == 0 || offset + length > bytes.size())
                break;
            annexb.append("\x00\x00\x00\x01", 4);
            annexb.append(bytes.substr(offset, static_cast<size_t>(length)));
            offset += static_cast<size_t>(length);
        }
        if (frame.keyframe)
            frame.config = track.config;
    } else {
        frame.kind = DemuxFrame::Kind::Audio;
        frame.data = std::string(bytes);
        frame.config = track.config;
    }
    sink(std::move(frame));
}

} // namespace tgrtmp
