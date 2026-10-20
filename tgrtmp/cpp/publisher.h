// Publishes a demuxed H.264/AAC stream to an RTMP client, as an external RTMP
// encoder would: it gates video on the first keyframe (a decoder can only
// start there), sends the sequence headers and the onMetaData once the
// configurations are known, anchors each track's media clock to the wall
// clock (re-anchoring when it drifts away, after a gap or a pause), and paces
// the sends to real time with a bounded look-ahead, so a bursty source can
// never run ahead of the livestream.
//
// The protocol logic follows the behavior proven against Telegram's server by
// the reference implementation in MadelineProto; the reordering heuristic of
// that implementation is not needed here, as the demuxers provide true
// decoding timestamps.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include "client.h"
#include "sink.h"

#include <functional>

namespace tgrtmp {

class Publisher : public FrameSink {
  public:
    struct Options {
        // How far the pacer may run ahead of the wall clock before feed()
        // blocks (the throttle of the reference implementation).
        uint32_t lookahead_ms = 1000;
        // How far a track's timestamps may drift from the wall clock before
        // its timeline is re-anchored.
        uint32_t max_drift_ms = 1000;
        // How long the first audio may hold for the first video keyframe
        // before the stream is declared audio-only (the reference waits a
        // second; on a live source joined mid-GOP the keyframe follows within
        // a GOP, and the server re-arms video when the dimensioned metadata
        // and the sequence header ride it).
        uint32_t wait_video_ms = 1000;
        // Whether the source is expected to carry video (e.g. known from the
        // probe or an explicit choice): while true, the first audio holds for
        // wait_video_ms.
        bool expect_video = false;
    };

    explicit Publisher(Options options);
    ~Publisher();

    Publisher(const Publisher &) = delete;
    Publisher &operator=(const Publisher &) = delete;

    // Attach a live connection: resets the timeline (each connection starts a
    // fresh one, with its own sequence headers) and wakes the sender.
    void attach(Client &client);
    // Detach the current connection, if any: the sender parks until the next
    // attach. Called when a connection drops or the publisher is shutting
    // down.
    void detach();

    // Feed a demuxed video access unit. Timestamps are milliseconds of the
    // source; `avcc_config` is the AVCDecoderConfigurationRecord extracted
    // from the SPS/PPS (only present on keyframes). Annex-B input is
    // converted internally.
    void feed_video(int64_t dts_ms, int64_t pts_ms, std::string_view annexb, bool keyframe,
                    std::string_view avcc_config);
    // Feed a demuxed AAC frame (raw, no ADTS) with the AudioSpecificConfig of
    // its track.
    void feed_audio(int64_t pts_ms, std::string_view asc, std::string_view raw);

    // The source jumped (live discontinuity): re-anchor both tracks and drop
    // the stale frames; but the decoder keeps its state, so the gates, the
    // sequence headers and the metadata stay armed (the reference
    // implementation never resets them between files, only on reconnect).
    void on_discontinuity();

    // The FrameSink interface, for feeding through a Source.
    void on_video(int64_t dts_ms, int64_t pts_ms, std::string_view annexb, bool keyframe,
                  std::string_view avcc_config) override {
        feed_video(dts_ms, pts_ms, annexb, keyframe, avcc_config);
    }
    void on_audio(int64_t pts_ms, std::string_view asc, std::string_view raw) override {
        feed_audio(pts_ms, asc, raw);
    }
    void on_jump() override { on_discontinuity(); }
    void on_has_video() override {
        std::lock_guard<std::mutex> lock(mutex_);
        options_.expect_video = true;
    }
    void on_warn(std::string_view message) override { warn(message); }

    void set_expect_video(bool expect) {
        std::lock_guard<std::mutex> lock(mutex_);
        options_.expect_video = expect;
    }

    bool connected() const;

    // The wire skew of the audio timeline against the video one, in ms:
    // how much later (positive) audio plays than video at equal media time.
    // Constant for the session once both tracks are anchored.
    int64_t av_skew_ms() const;

    // Warnings for conditions the server will not accept (dropped tracks,
    // unsupported codecs, aspect ratio...). Set before feeding; called from
    // the feeding thread and must not throw.
    void set_logger(std::function<void(const std::string &)> logger) {
        logger_ = std::move(logger);
    }

    // The Python callable behind logger_, held as an opaque owned pointer so
    // this header stays Python-free; the bindings traverse it for the
    // garbage collector and release it in tp_clear/stop.
    void set_logger_object(void *object) { logger_obj_ = object; }
    void *logger_object() const { return logger_obj_; }
    // Detaches the owned Python object (returned for the caller to DECREF
    // under the GIL) and stops logging.
    void *release_logger_py() {
        void *object = logger_obj_;
        logger_obj_ = nullptr;
        logger_ = nullptr;
        return object;
    }
    void clear_logger() {
        std::lock_guard<std::mutex> lock(warn_mutex_);
        pending_warnings_.clear();
        logger_ = nullptr;
    }

    uint64_t warnings() const { return warnings_.load(std::memory_order_relaxed); }

    // Wait until every queued frame has been sent (end of a finite source).
    // Returns false on timeout.
    bool drain(double timeout_s);

    uint64_t frames_sent() const { return frames_sent_.load(std::memory_order_relaxed); }
    uint64_t video_frames_sent() const {
        return video_frames_sent_.load(std::memory_order_relaxed);
    }
    uint64_t bytes_sent() const { return bytes_sent_.load(std::memory_order_relaxed); }
    uint64_t frames_dropped() const { return frames_dropped_.load(std::memory_order_relaxed); }

    void stop();

  private:
    struct Frame {
        int64_t due = 0;        // Wall clock (steady ms) at which to send it.
        uint32_t timestamp = 0; // Media timestamp (ms) carried by the message.
        std::string body;       // Sequence headers: the FLV body. Video frames: raw
                                // Annex-B, wrapped by the fused sender path.
        bool sequence_header = false;
        bool keyframe = false;
        uint32_t composition_time = 0;
    };
    struct Anchor {
        int64_t wall = 0;  // Steady clock ms of the anchor.
        int64_t media = 0; // The media timestamp anchored there.
    };

    void run_sender();
    // The wall clock due time of a media timestamp of a track, anchored to
    // the connection's timeline (the reference implementation's timestamp()).
    int64_t anchor_due(Anchor &anchor, int64_t media_ms);
    int64_t steady_ms() const;
    void reset_timeline();
    void clear_queues();
    // Blocks the feeder while the pacer runs ahead of the wall clock.
    void throttle_locked(std::unique_lock<std::mutex> &lock);
    void wake_sender();
    // Surfaces a condition the server will not accept; counts it. The
    // logger runs at flush_warnings(), outside any publisher lock, so a
    // callback may safely touch the publisher.
    void warn(std::string_view message);
    void flush_warnings();

    Options options_;
    Client *client_ = nullptr;

    mutable std::mutex mutex_;
    std::condition_variable sender_cv_;
    std::condition_variable feed_cv_;
    uint64_t generation_ = 0; // Bumped on attach/detach/discontinuity/stop.
    bool stopping_ = false;

    int64_t epoch_ = 0; // Steady ms at which the connection started.
    Anchor audio_anchor_, video_anchor_;
    std::string avc_config_, aac_config_, metadata_;
    bool sent_keyframe_ = false;
    bool keyframe_seen_ = false; // The first keyframe with a config arrived.
    int64_t awaiting_video_since_ = 0;
    int64_t last_video_dts_ = -1;
    uint32_t last_sent_video_ts_ = 0, last_sent_audio_ts_ = 0; // Wire clamps.

    std::function<void(const std::string &)> logger_;
    void *logger_obj_ = nullptr; // Owned PyObject* of the logger callable.
    std::atomic<uint64_t> warnings_{0};
    std::mutex warn_mutex_;
    std::vector<std::string> pending_warnings_;
    bool audio_publishable_ = true;  // The ASC parses (or no audio yet).
    std::string warned_aspect_dims_; // The dimensions already warned about.
    bool warned_no_config_ = false;  // Keyframes without SPS/PPS.
    bool warned_hold_ = false;       // The audio hold expired keyframe-less.

    std::thread sender_;
    std::atomic<bool> running_{false};

    // The paced send queues, drained by the sender thread (FIFO; the due
    // times of each queue are monotonic within a timeline).
    std::deque<Frame> audio_queue_, video_queue_;

    std::atomic<uint64_t> frames_sent_{0}, video_frames_sent_{0}, bytes_sent_{0},
        frames_dropped_{0};
};

} // namespace tgrtmp
