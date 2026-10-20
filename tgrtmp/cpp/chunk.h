// The RTMP chunk stream protocol (Adobe RTMP spec, §5.3): splitting outgoing
// messages into chunks and reassembling incoming ones.
//
// Clean-room implementation written against the public spec.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "h264.h"

namespace tgrtmp {

// Message type ids (RTMP spec §5.4).
namespace msg {
constexpr uint8_t SET_CHUNK_SIZE = 1;
constexpr uint8_t ABORT = 2;
constexpr uint8_t ACKNOWLEDGEMENT = 3;
constexpr uint8_t USER_CONTROL = 4;
constexpr uint8_t WINDOW_ACK_SIZE = 5;
constexpr uint8_t SET_PEER_BANDWIDTH = 6;
constexpr uint8_t AUDIO = 8;
constexpr uint8_t VIDEO = 9;
constexpr uint8_t DATA_AMF0 = 18;
constexpr uint8_t COMMAND_AMF0 = 20;
} // namespace msg

// Chunk stream ids used by publishers (small ids keep headers at one byte).
namespace csid {
constexpr uint32_t CONTROL = 2;
constexpr uint32_t COMMAND = 3;
constexpr uint32_t AUDIO = 4;
constexpr uint32_t DATA = 5;
constexpr uint32_t VIDEO = 6;
} // namespace csid

// Append one RTMP message, chunked at `out_chunk_size`, to `out`. The first
// chunk carries a full type-0 header; continuation chunks carry type-3
// headers with the extended timestamp echoed (MadelineProto's proven wire
// behavior against Telegram's server).
class ChunkWriter {
  public:
    explicit ChunkWriter(uint32_t out_chunk_size = 4096) : out_chunk_size_(out_chunk_size) {}

    void set_chunk_size(uint32_t size) { out_chunk_size_ = size; }
    uint32_t chunk_size() const { return out_chunk_size_; }

    void write_message(std::string &out, uint32_t chunk_stream, uint8_t type_id,
                       uint32_t message_stream_id, uint32_t timestamp_ms,
                       std::string_view payload) const;

    // Build one video message straight from an Annex-B access unit in a
    // single pass: the FLV tag header, the chunk headers and the
    // length-prefixed NAL units are written into `out` without intermediate
    // copies. Produces byte-identical output to
    // write_message(flv::avc_frame(h264::to_avcc(annexb), ...)), ~8x faster
    // (measured in bench/bench_internal.cpp).
    void write_avc_frame(std::string &out, uint32_t chunk_stream, uint32_t message_stream_id,
                         uint32_t timestamp_ms, bool keyframe, std::string_view annexb,
                         uint32_t composition_time) const;

  private:
    uint32_t out_chunk_size_;
};

// Reassembles incoming chunks into messages. Feed it network data; complete
// messages are delivered through `handler`. Holds partial data internally.
class ChunkParser {
  public:
    using Handler = std::function<void(uint8_t type_id, uint32_t timestamp_ms,
                                       uint32_t message_stream_id, std::string_view payload)>;

    void set_handler(Handler handler) { handler_ = std::move(handler); }
    void set_chunk_size(uint32_t size) { incoming_chunk_size_ = size; }

    // Process as much of `data` as possible; the remainder is buffered for
    // the next call.
    void feed(std::string_view data);

    // Abort (message type 2): discard the partial message of a chunk stream.
    void abort_stream(uint32_t chunk_stream);

  private:
    struct StreamState {
        uint32_t timestamp = 0;
        uint32_t delta = 0;
        uint32_t length = 0;
        uint8_t type = 0;
        uint32_t stream = 0;
        bool extended = false;
        std::string payload;
    };

    // Parse one chunk from `data`, returning false if more data is needed.
    // `consumed` is advanced past the chunk bytes.
    bool parse_chunk(std::string_view data, size_t &consumed);

    void deliver(StreamState &state);

    StreamState *state_for(uint32_t chunk_stream);

    uint32_t incoming_chunk_size_ = 128;
    std::string buffer_; // Unparsed remainder.
    // Fast path for the common small chunk stream ids (2..6); the rest of the
    // id space lives in a map.
    std::array<StreamState, 64> small_streams_{};
    std::unordered_map<uint32_t, StreamState> extended_streams_;
    Handler handler_;
};

} // namespace tgrtmp
