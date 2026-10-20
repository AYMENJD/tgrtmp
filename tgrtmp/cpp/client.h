// An RTMP(S) client that publishes a stream, as an RTMP encoder (OBS,
// ffmpeg) does: the simple handshake, the chunk stream, the protocol control
// messages and the AMF0 commands of NetConnection (connect, createStream) and
// NetStream (publish), per the Adobe RTMP spec.
//
// Threading model: one background reader thread services the socket (control
// messages, acknowledgements, pings, command results); media messages are
// written synchronously by the caller (at most one writer per client besides
// control replies, which are mutex-guarded).
//
// Clean-room implementation written against the public RTMP/AMF0 specs.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "amf0.h"
#include "chunk.h"

namespace tgrtmp {

class Client {
  public:
    struct Options {
        uint32_t out_chunk_size = 4096; // Fewer chunk headers than the default 128 bytes.
        uint32_t ack_window = 2500000;  // Our receive acknowledgement window, bytes.
        bool verify_tls = true;
        double connect_timeout_s = 15.0;
        double command_timeout_s = 10.0;
    };

    // Connect to an RTMP(S) server and start publishing a stream.
    // `url` is the RTMP application, e.g. "rtmps://dc4-1.rtmp.t.me/s/";
    // `key` is the stream key. Throws std::runtime_error on failure.
    static std::unique_ptr<Client> publish(std::string_view url, std::string_view key,
                                           const Options &options);

    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    // Send the onMetaData data message (its raw @setDataFrame AMF0 body).
    void send_metadata(std::string_view amf_body);

    // Send an audio (FLV tag body) message; timestamp in milliseconds.
    void send_audio(uint32_t timestamp_ms, std::string_view body);

    // Send a video (FLV tag body) message; timestamp in milliseconds.
    void send_video(uint32_t timestamp_ms, std::string_view body);

    // Send one video access unit straight from its Annex-B form: the wire
    // bytes are built in a single pass (the fused hot path, byte-identical
    // to send_video with a pre-wrapped body, ~8x fewer copies).
    void send_video_annexb(uint32_t timestamp_ms, bool keyframe, std::string_view annexb,
                           uint32_t composition_time);

    // Stop publishing and close the connection (safe to call twice).
    void close();

    bool is_closed() const { return closed_.load(std::memory_order_acquire); }

    // The numeric IP address the connection reached, valid once connected.
    const std::string &remote() const { return remote_; }

    // Why the connection was closed; empty if closed without an error.
    std::string error() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return error_;
    }

    // Resolve once the connection is closed, by us or by the server.
    // Returns false if it times out.
    bool await_close(double timeout_s);

  private:
    Client() = default;

    struct Transaction {
        uint32_t id = 0;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        bool is_error = false;
        std::string error;            // When is_error.
        std::vector<AmfValue> result; // The _result values after the transaction id.

        void complete(std::vector<AmfValue> values);
        void fail(std::string message);
    };

    void handshake_();
    void run_reader();
    bool read_some(); // One TLS/TCP read into the parser; false on EOF/error.
    void write_all(std::string_view data);
    void write_message(uint32_t chunk_stream, uint8_t type_id, uint32_t stream_id,
                       uint32_t timestamp_ms, std::string_view payload);
    void on_message(uint8_t type_id, uint32_t timestamp_ms, uint32_t stream_id,
                    std::string_view payload);
    void on_command(std::vector<AmfValue> &&values);
    void fail(const std::string &error);

    std::shared_ptr<Transaction> call(const std::string &name, std::vector<AmfValue> args,
                                      uint32_t stream_id, bool notify);

    // Waits for a transaction; throws std::runtime_error on error/timeout.
    std::vector<AmfValue> await(const std::shared_ptr<Transaction> &transaction);

    // "code description" of an onStatus-style info object.
    static std::string describe(const AmfValue *info);

    Options options_;
    bool tls_ = false;
    std::string host_, app_, tc_url_, key_;
    std::string remote_;      // The numeric address the connection reached.
    uint16_t port_ = 1935;

    int fd_ = -1;             // Platform socket.
    void *ssl_ = nullptr;     // SSL* (opaque here to keep OpenSSL out of the header).
    void *ssl_ctx_ = nullptr; // SSL_CTX*.

    ChunkWriter writer_;
    ChunkParser parser_;
    std::mutex write_mutex_;
    std::atomic<bool> closed_{false};
    mutable std::mutex state_mutex_;
    std::string error_;
    std::condition_variable closed_cv_;

    std::mutex transactions_mutex_;
    std::unordered_map<uint32_t, std::shared_ptr<Transaction>> transactions_;
    uint32_t next_transaction_ = 0;

    // The publishing onStatus promise: value empty on success, else the error.
    std::mutex publish_mutex_;
    std::condition_variable publish_cv_;
    bool publish_done_ = false;
    bool publish_ok_ = false;
    std::string publish_error_;

    uint32_t stream_id_ = 0;
    uint64_t bytes_received_ = 0;
    uint64_t bytes_acknowledged_ = 0;
    uint32_t peer_ack_window_ = 2500000;
    std::thread reader_;
};

} // namespace tgrtmp
