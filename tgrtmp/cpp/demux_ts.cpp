#include "demux_ts.h"

#include <cstring>
#include <stdexcept>

#include "flv.h"
#include "h264.h"

namespace tgrtmp {

namespace {

constexpr size_t TS_PACKET = 188;

[[noreturn]] void fail(const char *what) { throw std::runtime_error(what); }

int64_t pes_timestamp(const unsigned char *p) {
    // Five bytes of '001x' marker bits interleaved with a 33-bit value.
    int64_t value = static_cast<int64_t>((p[0] >> 1) & 0x07) << 30;
    value |= static_cast<int64_t>(p[1]) << 22;
    value |= static_cast<int64_t>(p[2] >> 1) << 15;
    value |= static_cast<int64_t>(p[3]) << 7;
    value |= static_cast<int64_t>(p[4]) >> 1;
    return value;
}

unsigned char u8(const std::string &data, size_t offset) {
    return static_cast<unsigned char>(data[offset]);
}

} // namespace

bool TsDemuxer::PsiAssembler::feed(const unsigned char *payload, size_t length,
                                   std::string &section, bool pusi) {
    if (length == 0)
        return false;
    if (!collecting_) {
        if (!pusi)
            return false;            // A section can only start on a payload-unit start.
        size_t pointer = payload[0]; // pointer_field.
        if (1 + pointer > length)
            return false;
        payload += 1 + pointer;
        length -= 1 + pointer;
        collecting_ = true;
    }
    buffer_.append(reinterpret_cast<const char *>(payload), length);
    if (buffer_.size() < 3)
        return false;
    size_t section_length = (static_cast<size_t>(u8(buffer_, 1) & 0x0F) << 8) | u8(buffer_, 2);
    if (buffer_.size() < 3 + section_length)
        return false;
    section.assign(buffer_, 0, 3 + section_length);
    buffer_.clear();
    collecting_ = false;
    return true;
}

void TsDemuxer::feed(std::string_view data, const DemuxSink &sink) {
    fed_bytes_ += data.size();
    // A transport stream always carries its program map early; none within a
    // reasonable amount of media means this is not demuxable input; fail
    // loudly instead of publishing silence forever.
    if (pmt_pid_ < 0 && fed_bytes_ > (2 << 20) && !warned_no_pmt_) {
        warned_no_pmt_ = true;
        fail("No MPEG-TS program map within the first 2 MiB of the stream");
    }
    buffer_.append(data);
    const auto *base = reinterpret_cast<const unsigned char *>(buffer_.data());
    size_t size = buffer_.size();
    size_t offset = 0;
    while (true) {
        while (offset < size && base[offset] != 0x47)
            ++offset; // Resynchronize.
        if (size - offset < TS_PACKET)
            break;
        // A packet boundary needs three consecutive sync bytes: locking onto
        // a spurious pair steps the whole grid off by that offset, where no
        // PSI ever appears (seen on forward seeks into the live edge).
        if (offset + 2 * TS_PACKET < size &&
            (base[offset + TS_PACKET] != 0x47 || base[offset + 2 * TS_PACKET] != 0x47)) {
            ++offset;
            continue;
        }
        if (offset + TS_PACKET < size && base[offset + TS_PACKET] != 0x47) {
            ++offset;
            continue;
        }
        process_packet(base + offset, sink);
        offset += TS_PACKET;
    }
    buffer_.erase(0, offset);
}

void TsDemuxer::finish(const DemuxSink &sink) {
    emit_video(sink);
    emit_audio(sink);
}

void TsDemuxer::process_packet(const unsigned char *p, const DemuxSink &sink) {
    bool pusi = (p[1] & 0x40) != 0;
    int pid = (static_cast<int>(p[1] & 0x1F) << 8) | p[2];
    size_t af_control = (p[3] >> 4) & 0x03;
    if (!(af_control & 0x01))
        return; // No payload.

    // A discontinuity in the adaptation field of a media packet starts a new
    // timeline.
    if ((af_control & 0x02) != 0 && (pid == video_pid_ || pid == audio_pid_)) {
        size_t af_length = p[4];
        if (af_length > 0 && (p[5] & 0x80) != 0) {
            DemuxFrame jump;
            jump.kind = DemuxFrame::Kind::Discontinuity;
            sink(std::move(jump));
            last_video_dts_ = INT64_MIN;
            video_first_ = false;
        }
    }

    size_t offset = 4;
    if (af_control & 0x02)
        offset += 1 + p[4];
    if (offset >= TS_PACKET)
        return;

    if (pid == 0 || pid == pmt_pid_) {
        process_psi(p + offset, TS_PACKET - offset, pusi, pid);
    } else if (pid == video_pid_ || pid == audio_pid_) {
        process_pes(p + offset, TS_PACKET - offset, pusi, pid, sink);
    }
}

void TsDemuxer::process_psi(const unsigned char *payload, size_t length, bool pusi, int pid) {
    std::string section;
    if (!(pid == 0 ? pat_asm_.feed(payload, length, section, pusi)
                   : pmt_asm_.feed(payload, length, section, pusi))) {
        return;
    }

    if (pid == 0) {
        // Program association: the program map PID of the first program.
        size_t offset = 8;
        size_t end = section.size() - 4; // Minus the CRC32.
        while (offset + 4 <= end) {
            int program = (static_cast<int>(u8(section, offset)) << 8) | u8(section, offset + 1);
            int pid_field =
                (static_cast<int>(u8(section, offset + 2) & 0x1F) << 8) | u8(section, offset + 3);
            if (program != 0) {
                pmt_pid_ = pid_field;
                return;
            }
            offset += 4;
        }
        return;
    }

    // Program map: pick the H.264 video and the ADTS AAC streams.
    size_t section_length = (static_cast<size_t>(u8(section, 1) & 0x0F) << 8) | u8(section, 2);
    size_t program_info = (static_cast<size_t>(u8(section, 10) & 0x0F) << 8) | u8(section, 11);
    size_t offset = 12 + program_info;
    size_t end = 3 + section_length - 4; // Minus the CRC32.
    bool saw_video_ish = false, saw_audio_ish = false;
    while (offset + 5 <= end) {
        int stream_type = u8(section, offset);
        int elementary =
            (static_cast<int>(u8(section, offset + 1) & 0x1F) << 8) | u8(section, offset + 2);
        size_t es_info =
            (static_cast<size_t>(u8(section, offset + 3) & 0x0F) << 8) | u8(section, offset + 4);
        if (stream_type == 0x1B && video_pid_ < 0) { // H.264.
            video_pid_ = elementary;
        } else if (stream_type == 0x0F && audio_pid_ < 0) { // AAC, ADTS.
            audio_pid_ = elementary;
        } else if (stream_type == 0x24 || stream_type == 0x42 || stream_type == 0x10 ||
                   stream_type == 0x20) {
            // HEVC, AVS or MPEG-4 Part 2 video: Telegram's server accepts
            // H.264 only. Warn; the stream continues without video.
            saw_video_ish = true;
        } else if (stream_type == 0x03 || stream_type == 0x04 || stream_type == 0x11 ||
                   stream_type == 0x81 || stream_type == 0x82 || stream_type == 0x90) {
            // MP3, LATM, AC-3, DTS, Opus: the server accepts AAC only.
            saw_audio_ish = true;
        }
        offset += 5 + es_info;
    }
    if ((saw_video_ish || saw_audio_ish) && warner_) {
        if (saw_video_ish && video_pid_ < 0) {
            warner_("The stream's video codec is not H.264: Telegram's RTMP server accepts "
                    "H.264 only, publishing continues without video");
        }
        if (saw_audio_ish && audio_pid_ < 0) {
            warner_("The stream's audio codec is not AAC: Telegram's RTMP server accepts "
                    "AAC only, publishing continues without audio");
        }
    }
    if (video_pid_ < 0 && audio_pid_ < 0) {
        fail("The stream carries no H.264 video and no AAC audio that Telegram's RTMP "
             "server accepts; re-encode it, e.g. with ffmpeg -i input -c:v libx264 "
             "-c:a aac -movflags +faststart output.mp4");
    }
}

void TsDemuxer::process_pes(const unsigned char *payload, size_t length, bool pusi, int pid,
                            const DemuxSink &sink) {
    PesAssembly &pes = pid == video_pid_ ? video_pes_ : audio_pes_;
    if (!pusi) {
        if (pes.started)
            pes.buffer.append(reinterpret_cast<const char *>(payload), length);
        return;
    }
    // A payload unit starts: the previous one is complete.
    if (pid == video_pid_) {
        emit_video(sink);
    } else {
        emit_audio(sink);
    }
    pes.started = false;
    pes.buffer.clear();
    if (length < 9 || !(payload[0] == 0 && payload[1] == 0 && payload[2] == 1))
        return;
    size_t header_length = payload[8];
    if (9 + header_length > length)
        return;
    int flags = payload[7];
    bool has_pts = (flags & 0x80) != 0;
    bool has_dts = (flags & 0x40) != 0;
    pes.pts = has_pts ? pes_timestamp(payload + 9) : 0;
    pes.dts = has_dts ? pes_timestamp(payload + 14) : pes.pts;
    pes.buffer.append(reinterpret_cast<const char *>(payload) + 9 + header_length,
                      length - 9 - header_length);
    pes.started = true;
}

void TsDemuxer::check_discontinuity(int64_t dts_ms, const DemuxSink &sink) {
    if (video_first_) {
        video_first_ = false;
        return;
    }
    if (last_video_dts_ == INT64_MIN)
        return;
    if (dts_ms < last_video_dts_ - 500 || dts_ms > last_video_dts_ + 3000) {
        DemuxFrame jump;
        jump.kind = DemuxFrame::Kind::Discontinuity;
        sink(std::move(jump));
    }
}

void TsDemuxer::emit_video(const DemuxSink &sink) {
    PesAssembly &pes = video_pes_;
    if (!pes.started || pes.buffer.empty())
        return;
    pes.started = false;

    int64_t dts_ms = pes.dts * 1000 / 90000;
    int64_t pts_ms = pes.pts * 1000 / 90000;
    check_discontinuity(dts_ms, sink);
    last_video_dts_ = dts_ms;

    DemuxFrame frame;
    frame.kind = DemuxFrame::Kind::Video;
    frame.dts_ms = dts_ms;
    frame.pts_ms = pts_ms;
    frame.keyframe = h264::is_keyframe(pes.buffer);
    if (frame.keyframe) {
        try {
            h264::AvccConfig config = h264::describe(pes.buffer);
            frame.config = std::move(config.record);
        } catch (const std::exception &) {
            // No in-band parameter sets in this keyframe: publishing starts
            // once a keyframe carries them.
        }
    }
    frame.data = std::move(pes.buffer);
    pes.buffer.clear();
    sink(std::move(frame));
}

void TsDemuxer::emit_audio(const DemuxSink &sink) {
    PesAssembly &pes = audio_pes_;
    if (!pes.started || pes.buffer.empty())
        return;
    pes.started = false;

    // One PES carries one or more ADTS frames; each spans 1024 samples.
    std::string_view data(pes.buffer);
    int64_t pts_ms = pes.pts * 1000 / 90000;
    int64_t frame_ms = 0;
    size_t index = 0;
    while (true) {
        while (index < data.size() && static_cast<unsigned char>(data[index]) != 0xFF)
            ++index;
        if (index + 7 > data.size())
            break;
        std::string asc;
        std::string_view raw;
        if (!flv::parse_adts(data.substr(index), asc, raw)) {
            ++index;
            continue;
        }
        if (frame_ms == 0) {
            try {
                frame_ms = 1024 * 1000 / flv::sample_rate(asc);
            } catch (const std::exception &) {
                frame_ms = 1024 * 1000 / 48000; // Fallback: 48 kHz.
            }
        }
        DemuxFrame frame;
        frame.kind = DemuxFrame::Kind::Audio;
        frame.data = std::string(raw);
        frame.dts_ms = frame.pts_ms = pts_ms;
        frame.config = asc;
        pts_ms += frame_ms;
        index = static_cast<size_t>(raw.data() - data.data()) + raw.size();
        sink(std::move(frame));
    }
}

} // namespace tgrtmp
