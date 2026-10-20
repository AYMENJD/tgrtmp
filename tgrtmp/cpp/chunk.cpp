#include "chunk.h"

#include <cstring>

namespace tgrtmp {

namespace {

void put_be24(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void put_be32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void put_le32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

constexpr size_t HEADER_LENGTH[4] = {11, 7, 3, 0};

uint32_t read_be24(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

uint32_t read_be32(const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t read_le32(const unsigned char *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

} // namespace

void ChunkWriter::write_message(std::string &out, uint32_t chunk_stream, uint8_t type_id,
                                uint32_t message_stream_id, uint32_t timestamp_ms,
                                std::string_view payload) const {
    const bool extended = timestamp_ms >= 0xFFFFFF;
    const size_t length = payload.size();

    // Type-0 chunk header (RTMP spec §5.3.1.1); chunk stream ids >= 64 use
    // the basic-header escape values 0/1 in the low bits.
    if (chunk_stream < 64) {
        out.push_back(static_cast<char>(chunk_stream));
    } else if (chunk_stream < 320) {
        out.push_back(static_cast<char>(0x00));
        out.push_back(static_cast<char>(chunk_stream - 64));
    } else {
        out.push_back(static_cast<char>(0x01));
        uint32_t rest = chunk_stream - 64;
        out.push_back(static_cast<char>(rest & 0xFF));
        out.push_back(static_cast<char>((rest >> 8) & 0xFF));
    }
    put_be24(out, extended ? 0xFFFFFF : timestamp_ms);
    put_be24(out, static_cast<uint32_t>(length));
    out.push_back(static_cast<char>(type_id));
    put_le32(out, message_stream_id);
    if (extended)
        put_be32(out, timestamp_ms);

    size_t offset = 0;
    while (offset < length || (length == 0 && offset == 0)) {
        if (offset > 0) {
            // Type-3 continuation header; the extended timestamp is echoed
            // on every continuation, as MadelineProto does against
            // Telegram's server.
            if (chunk_stream < 64) {
                out.push_back(static_cast<char>(0xC0 | chunk_stream));
            } else if (chunk_stream < 320) {
                out.push_back(static_cast<char>(0xC0 | 0x00));
                out.push_back(static_cast<char>(chunk_stream - 64));
            } else {
                out.push_back(static_cast<char>(0xC0 | 0x01));
                uint32_t rest = chunk_stream - 64;
                out.push_back(static_cast<char>(rest & 0xFF));
                out.push_back(static_cast<char>((rest >> 8) & 0xFF));
            }
            if (extended)
                put_be32(out, timestamp_ms);
        }
        size_t take = out_chunk_size_ < length - offset ? out_chunk_size_ : length - offset;
        out.append(payload.substr(offset, take));
        offset += take;
        if (length == 0)
            break;
    }
}

void ChunkWriter::write_avc_frame(std::string &out, uint32_t chunk_stream,
                                  uint32_t message_stream_id, uint32_t timestamp_ms, bool keyframe,
                                  std::string_view annexb, uint32_t composition_time) const {
    // First pass: the final FLV body size (4-byte length prefix per NAL
    // unit; empty NAL units are skipped, exactly as the staged path does).
    size_t payload = 5; // FLV video tag header.
    h264::split_annexb(annexb, [&](std::string_view nalu) {
        if (!nalu.empty())
            payload += 4 + nalu.size();
    });

    const bool extended = timestamp_ms >= 0xFFFFFF;
    const size_t chunks = payload / out_chunk_size_ + 1;
    out.reserve(out.size() + 13 + chunks * 5 + payload);

    // Type-0 chunk header (the fused path mirrors write_message exactly).
    if (chunk_stream < 64) {
        out.push_back(static_cast<char>(chunk_stream));
    } else if (chunk_stream < 320) {
        out.push_back(static_cast<char>(0x00));
        out.push_back(static_cast<char>(chunk_stream - 64));
    } else {
        out.push_back(static_cast<char>(0x01));
        uint32_t rest = chunk_stream - 64;
        out.push_back(static_cast<char>(rest & 0xFF));
        out.push_back(static_cast<char>((rest >> 8) & 0xFF));
    }
    put_be24(out, extended ? 0xFFFFFF : timestamp_ms);
    put_be24(out, static_cast<uint32_t>(payload));
    out.push_back(static_cast<char>(msg::VIDEO));
    put_le32(out, message_stream_id);
    if (extended)
        put_be32(out, timestamp_ms);

    // Bulk copies, split only at chunk boundaries.
    size_t body = 0;
    auto put = [&](const char *data, size_t length) {
        while (length > 0) {
            if (body == out_chunk_size_) {
                // Type-3 continuation, with the extended timestamp echoed.
                if (chunk_stream < 64) {
                    out.push_back(static_cast<char>(0xC0 | chunk_stream));
                } else if (chunk_stream < 320) {
                    out.push_back(static_cast<char>(0xC0));
                    out.push_back(static_cast<char>(chunk_stream - 64));
                } else {
                    out.push_back(static_cast<char>(0xC1));
                    uint32_t rest = chunk_stream - 64;
                    out.push_back(static_cast<char>(rest & 0xFF));
                    out.push_back(static_cast<char>((rest >> 8) & 0xFF));
                }
                if (extended)
                    put_be32(out, timestamp_ms);
                body = 0;
            }
            size_t take = length < out_chunk_size_ - body ? length : out_chunk_size_ - body;
            out.append(data, take);
            data += take;
            length -= take;
            body += take;
        }
    };
    // FLV video tag header: (frame type << 4) | codec 7 (AVC), then
    // AVCPacketType 1 (NAL units), then the 24-bit composition time.
    const char flv_header[5] = {static_cast<char>(((keyframe ? 1 : 2) << 4) | 7), '\x01',
                                static_cast<char>((composition_time >> 16) & 0xFF),
                                static_cast<char>((composition_time >> 8) & 0xFF),
                                static_cast<char>(composition_time & 0xFF)};
    put(flv_header, 5);
    h264::split_annexb(annexb, [&](std::string_view nalu) {
        if (nalu.empty())
            return;
        char prefix[4] = {static_cast<char>((nalu.size() >> 24) & 0xFF),
                          static_cast<char>((nalu.size() >> 16) & 0xFF),
                          static_cast<char>((nalu.size() >> 8) & 0xFF),
                          static_cast<char>(nalu.size() & 0xFF)};
        put(prefix, 4);
        put(nalu.data(), nalu.size());
    });
}

ChunkParser::StreamState *ChunkParser::state_for(uint32_t chunk_stream) {
    if (chunk_stream < 64)
        return &small_streams_[chunk_stream];
    return &extended_streams_[chunk_stream];
}

void ChunkParser::abort_stream(uint32_t chunk_stream) {
    if (StreamState *state = state_for(chunk_stream))
        state->payload.clear();
}

void ChunkParser::deliver(StreamState &state) {
    if (handler_) {
        std::string_view payload(state.payload.data(), state.payload.size());
        handler_(state.type, state.timestamp, state.stream, payload);
    }
    state.payload.clear();
}

void ChunkParser::feed(std::string_view data) {
    buffer_.append(data);
    size_t consumed = 0;
    std::string_view view(buffer_);
    while (parse_chunk(view, consumed)) {
        view.remove_prefix(consumed);
        consumed = 0;
    }
    // Compact the buffer: keep only the unparsed tail.
    buffer_.erase(0, buffer_.size() - view.size());
}

bool ChunkParser::parse_chunk(std::string_view data, size_t &consumed) {
    size_t length = data.size();
    if (length < 1)
        return false;
    const unsigned char *base = reinterpret_cast<const unsigned char *>(data.data());

    unsigned char first = base[0];
    size_t format = first >> 6;
    uint32_t chunk_stream = first & 0x3F;
    size_t offset = 1;
    if (chunk_stream == 0) {
        if (length < 2)
            return false;
        chunk_stream = 64 + base[1];
        offset = 2;
    } else if (chunk_stream == 1) {
        if (length < 3)
            return false;
        chunk_stream = 64 + base[1] + static_cast<uint32_t>(base[2]) * 256;
        offset = 3;
    }
    if (length < offset + HEADER_LENGTH[format])
        return false;

    StreamState &state = *state_for(chunk_stream);
    uint32_t field = 0;
    if (format <= 2)
        field = read_be24(base + offset);
    if (format <= 1) {
        state.length = read_be24(base + offset + 3);
        state.type = base[offset + 6];
    }
    if (format == 0)
        state.stream = read_le32(base + offset + 7);
    offset += HEADER_LENGTH[format];

    bool extended = (format <= 2) ? (field == 0xFFFFFF) : state.extended;
    if (extended) {
        if (length < offset + 4)
            return false;
        if (format <= 2)
            field = read_be32(base + offset);
        offset += 4;
    }

    bool starting = state.payload.empty();
    if (format == 0) {
        state.timestamp = field;
        state.delta = 0;
    } else if (format <= 2) {
        state.delta = field;
        state.timestamp += field;
    } else if (starting) {
        // A new message that reuses the previous header: advance its timestamp.
        state.timestamp += state.delta;
    }
    state.extended = extended;

    size_t remaining = state.length - state.payload.size();
    size_t take = incoming_chunk_size_ < remaining ? incoming_chunk_size_ : remaining;
    if (length < offset + take)
        return false;
    state.payload.append(data.substr(offset, take));
    consumed = offset + take;

    if (state.payload.size() >= state.length)
        deliver(state);
    return true;
}

} // namespace tgrtmp
