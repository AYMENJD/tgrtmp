#include "publisher.h"

#include <chrono>
#include <cmath>

#include "flv.h"
#include "h264.h"

namespace tgrtmp {

Publisher::Publisher(Options options) : options_(options) {
    running_.store(true, std::memory_order_release);
    sender_ = std::thread([this] { run_sender(); });
}

Publisher::~Publisher() { stop(); }

void Publisher::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel))
        return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        client_ = nullptr;
        generation_++;
        clear_queues();
    }
    sender_cv_.notify_all();
    feed_cv_.notify_all();
    if (sender_.joinable())
        sender_.join();
    // Drop the logger function; the owned logger PyObject* stays until
    // tp_clear or the next set_logger (both run under the GIL).
    std::lock_guard<std::mutex> lock(warn_mutex_);
    pending_warnings_.clear();
    logger_ = nullptr;
}

int64_t Publisher::steady_ms() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Publisher::warn(std::string_view message) {
    warnings_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(warn_mutex_);
        pending_warnings_.emplace_back(message);
    }
}

void Publisher::flush_warnings() {
    if (!logger_) {
        std::lock_guard<std::mutex> lock(warn_mutex_);
        pending_warnings_.clear();
        return;
    }
    std::vector<std::string> pending;
    {
        std::lock_guard<std::mutex> lock(warn_mutex_);
        pending.swap(pending_warnings_);
    }
    for (const std::string &message : pending)
        logger_(message);
}

bool Publisher::drain(double timeout_s) {
    std::unique_lock<std::mutex> lock(mutex_);
    const int64_t deadline = steady_ms() + static_cast<int64_t>(timeout_s * 1000.0);
    while (running_.load(std::memory_order_acquire)) {
        if (client_ == nullptr)
            return false;
        if (audio_queue_.empty() && video_queue_.empty())
            return true;
        int64_t now = steady_ms();
        if (now >= deadline)
            return false;
        feed_cv_.wait_for(lock, std::chrono::milliseconds(std::min<int64_t>(deadline - now, 50)));
    }
    return false;
}

bool Publisher::connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_ != nullptr && !client_->is_closed();
}

int64_t Publisher::av_skew_ms() const {
    // Each track paces by its own anchor; the skew is the difference of
    // their wall-clock mappings at equal media time. Both clocks run at the
    // file's rate, so the skew is constant for the session.
    std::lock_guard<std::mutex> lock(mutex_);
    if (audio_anchor_.wall == 0 || video_anchor_.wall == 0)
        return 0;
    return (audio_anchor_.wall - audio_anchor_.media) -
           (video_anchor_.wall - video_anchor_.media);
}

void Publisher::reset_timeline() {
    // Each connection starts fresh, with its own sequence headers, as the
    // reference publisher resets on every reconnect.
    epoch_ = steady_ms();
    audio_anchor_ = video_anchor_ = Anchor{};
    avc_config_.clear();
    aac_config_.clear();
    metadata_.clear();
    sent_keyframe_ = false;
    keyframe_seen_ = false;
    awaiting_video_since_ = 0;
    last_video_dts_ = -1;
    last_sent_video_ts_ = last_sent_audio_ts_ = 0;
    audio_publishable_ = true;
    warned_aspect_dims_.clear();
    warned_no_config_ = false;
    warned_hold_ = false;
}

void Publisher::clear_queues() {
    frames_dropped_.fetch_add(audio_queue_.size() + video_queue_.size(), std::memory_order_relaxed);
    audio_queue_.clear();
    video_queue_.clear();
}

void Publisher::attach(Client &client) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client_ = &client;
        reset_timeline();
        clear_queues();
        generation_++;
    }
    feed_cv_.notify_all();
    sender_cv_.notify_all();
}

void Publisher::detach() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        client_ = nullptr;
        clear_queues();
        generation_++;
    }
    feed_cv_.notify_all();
    sender_cv_.notify_all();
}

void Publisher::on_discontinuity() {
    // A jump in the source only re-anchors the timelines and drops the stale
    // frames: the decoder on the server keeps its state, so the gates, the
    // sequence headers and the metadata stay armed; the reference resets
    // them only on reconnect, and a re-sent dimensionless metadata mid-stream
    // can make the server commit the livestream to audio-only.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (client_ == nullptr)
            return;
        audio_anchor_ = video_anchor_ = Anchor{};
        clear_queues();
        last_video_dts_ = -1;
        generation_++;
    }
    feed_cv_.notify_all();
    sender_cv_.notify_all();
    flush_warnings();
}

int64_t Publisher::anchor_due(Anchor &anchor, int64_t media_ms) {
    // Each track is paced by the clock of its source, anchored to the wall
    // clock from its first frame, as the reference implementation does.
    // Returns the ABSOLUTE wall clock time at which the frame is due; the
    // message timestamp it carries is relative to the connection epoch.
    //
    // Re-anchoring happens only when the media falls BEHIND the wall clock by
    // more than max_drift_ms (a pause or a gap; the case the reference
    // handles). A source that runs AHEAD of the wall clock (a live stream's
    // backfill bursts, which the reference's file player never produces)
    // keeps its future dues instead: the pacer holds the frames and the
    // look-ahead throttle slows the feeder, so the wire stays at real time
    // and the timestamps never burst or repeat.
    int64_t now = steady_ms();
    if (anchor.wall != 0) {
        double media =
            static_cast<double>(anchor.wall) + static_cast<double>(media_ms - anchor.media);
        if (media >= static_cast<double>(now) - static_cast<double>(options_.max_drift_ms)) {
            return static_cast<int64_t>(media);
        }
    }
    anchor.wall = now;
    anchor.media = media_ms;
    return now;
}

void Publisher::wake_sender() {
    sender_cv_.notify_all();
    // The feed thread may be throttled on the look-ahead: wake it too.
    feed_cv_.notify_all();
}

void Publisher::feed_audio(int64_t pts_ms, std::string_view asc, std::string_view raw) {
    struct WarningFlush {
        Publisher *self;
        ~WarningFlush() { self->flush_warnings(); }
    } flush_on_exit{this};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (client_ == nullptr || client_->is_closed() || stopping_) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The server sizes the livestream from the onMetaData and waits only
        // briefly for the first keyframe; a dimensionless metadata sent too early
        // can make it commit to audio-only and discard the video that follows.
        // So audio holds for the first keyframe WITH a configuration record (a
        // source joined mid-GOP inter frame is not enough), up to wait_video_ms,
        // long enough to cover a live GOP.
        if (metadata_.empty()) {
            bool hold_for_video = options_.expect_video && !keyframe_seen_;
            if (hold_for_video) {
                if (awaiting_video_since_ == 0)
                    awaiting_video_since_ = steady_ms();
                if (steady_ms() - awaiting_video_since_ <
                    static_cast<int64_t>(options_.wait_video_ms)) {
                    frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                if (!warned_hold_) {
                    warned_hold_ = true;
                    warn("No video keyframe with its parameter sets within wait_video_ms: "
                         "starting audio-only, and the server may not pick video up later");
                }
            }
            metadata_ = flv::metadata(-1, -1, true, true);
            client_->send_metadata(metadata_);
            frames_sent_.fetch_add(1, std::memory_order_relaxed);
        }

        // Audio the server cannot carry must not silently ride the wire: an
        // AudioSpecificConfig that does not parse (explicit sampling frequency,
        // program config element channels) is dropped with a warning, exactly as
        // the reference drops non-AAC audio; the stream continues without it.
        if (asc != aac_config_) {
            int object_type = 0, frequency_index = 0, channels = 0;
            try {
                flv::parse_audio_specific_config(asc, object_type, frequency_index, channels);
                audio_publishable_ = true;
            } catch (const std::exception &error) {
                audio_publishable_ = false;
                warn(std::string("Dropping audio the server will not accept: ") + error.what());
            }
        }
        if (!audio_publishable_) {
            aac_config_ = std::string(asc); // Remember, to not re-warn per frame.
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The source carries one clock: whichever track anchors first lends
        // its mapping to the other, so cross-track offsets stay faithful to
        // the source instead of depending on which frame arrived first.
        if (audio_anchor_.wall == 0 && video_anchor_.wall != 0)
            audio_anchor_ = video_anchor_;

        int64_t due = anchor_due(audio_anchor_, pts_ms);
        // The reference rounds its timestamps: (int) round(media - epoch).
        uint32_t timestamp = static_cast<uint32_t>(std::max<int64_t>(
            0, std::llround(static_cast<double>(due) - static_cast<double>(epoch_))));

        if (asc != aac_config_) {
            Frame header;
            header.due = due;
            header.timestamp = timestamp;
            header.body = flv::aac_sequence_header(asc);
            audio_queue_.push_back(std::move(header));
            aac_config_ = std::string(asc);
        }
        Frame frame;
        frame.due = due;
        frame.timestamp = timestamp;
        frame.body = flv::aac_frame(raw);
        audio_queue_.push_back(std::move(frame));
        wake_sender();
        throttle_locked(lock);
    }
}

void Publisher::feed_video(int64_t dts_ms, int64_t pts_ms, std::string_view annexb, bool keyframe,
                           std::string_view avcc_config) {
    struct WarningFlush {
        Publisher *self;
        ~WarningFlush() { self->flush_warnings(); }
    } flush_on_exit{this};
    {
        std::unique_lock<std::mutex> lock(mutex_);
        if (client_ == nullptr || client_->is_closed() || stopping_) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // One clock for both tracks: see feed_audio.
        if (video_anchor_.wall == 0 && audio_anchor_.wall != 0)
            video_anchor_ = audio_anchor_;

        if (keyframe) {
            // The sequence header (and the metadata, once the dimensions are
            // known) ride the first keyframe that carries a configuration record.
            if (!avcc_config.empty() && avcc_config != avc_config_) {
                avc_config_ = std::string(avcc_config);
                int width = 0, height = 0;
                const bool dims_ok = h264::avcc_dimensions(avcc_config, width, height);
                // Unparseable dimensions must not ride the metadata as 0x0: the
                // server sizes the livestream from them.
                std::string dims_metadata =
                    flv::metadata(dims_ok ? width : -1, dims_ok ? height : -1, true, true);
                if (dims_metadata != metadata_) {
                    metadata_ = dims_metadata;
                    client_->send_metadata(metadata_);
                    frames_sent_.fetch_add(1, std::memory_order_relaxed);
                }
                if (dims_ok && width * 9 != height * 16 && warned_aspect_dims_ != dims_metadata) {
                    warned_aspect_dims_ = dims_metadata;
                    warn("Telegram scales RTMP livestreams to 1280x720 without keeping the "
                         "aspect ratio: the " +
                         std::to_string(width) + "x" + std::to_string(height) +
                         " video looks stretched; pad it to 16:9 first, e.g. with ffmpeg -i "
                         "input -vf "
                         "'scale=1280:720:force_original_aspect_ratio=decrease,"
                         "pad=1280:720:(ow-iw)/2:(oh-ih)/2' -c:v libx264 -c:a aac output.mp4");
                }
                // The sequence header itself precedes the keyframe on the wire,
                // as its own paced video message with the same wall-derived
                // timestamp as the frames. The source's raw media timestamps (a
                // live DVR timeline can be tens of hours) must never leak onto
                // the wire: the server drops video that far ahead of audio.
                Frame header;
                header.due = anchor_due(video_anchor_, dts_ms);
                header.timestamp = static_cast<uint32_t>(std::max<int64_t>(
                    0,
                    std::llround(static_cast<double>(header.due) - static_cast<double>(epoch_))));
                header.body = flv::avc_sequence_header(avcc_config);
                header.sequence_header = true;
                video_queue_.push_back(std::move(header));
            }
            if (avc_config_.empty()) {
                // Without the parameter sets no decoder can start, so video stays
                // gated forever: warn rather than drop silently.
                if (!warned_no_config_) {
                    warned_no_config_ = true;
                    warn("The stream's keyframes carry no SPS/PPS parameter sets: video cannot "
                         "start on the server; re-mux with in-band parameter sets (as MPEG-TS "
                         "and MKV carry them)");
                }
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            sent_keyframe_ = true;
            keyframe_seen_ = true; // The audio hold ends: dimensions are known.
        }
        if (!sent_keyframe_) {
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Decoding timestamps must not go backwards on the wire.
        int64_t dts = std::max(dts_ms, last_video_dts_);
        last_video_dts_ = dts;
        int64_t due = anchor_due(video_anchor_, dts);
        // The composition time is the presentation timestamp minus the decoding
        // one; it is signed (a deeper reordering than expected is allowed).
        int64_t composition = pts_ms - dts;
        Frame frame;
        frame.due = due;
        // The reference rounds its timestamps: (int) round(media - epoch).
        frame.timestamp = static_cast<uint32_t>(std::max<int64_t>(
            0, std::llround(static_cast<double>(due) - static_cast<double>(epoch_))));
        frame.body = std::string(annexb); // Wrapped by the fused sender path.
        frame.keyframe = keyframe;
        frame.composition_time = static_cast<uint32_t>(composition & 0xFFFFFF);
        video_queue_.push_back(std::move(frame));
        wake_sender();
        throttle_locked(lock);
    }
}

void Publisher::throttle_locked(std::unique_lock<std::mutex> &lock) {
    if (client_ == nullptr || stopping_)
        return;
    const uint64_t generation = generation_;
    const int64_t limit = static_cast<int64_t>(options_.lookahead_ms);
    while (running_.load(std::memory_order_acquire)) {
        if (client_ == nullptr || client_->is_closed() || stopping_ || generation != generation_) {
            return;
        }
        int64_t now = steady_ms();
        int64_t audio_ahead = audio_queue_.empty() ? 0 : audio_queue_.back().due - now;
        int64_t video_ahead = video_queue_.empty() ? 0 : video_queue_.back().due - now;
        if (audio_ahead <= limit && video_ahead <= limit)
            return;
        feed_cv_.wait_for(lock, std::chrono::milliseconds(5));
    }
}

void Publisher::run_sender() {
    while (running_.load(std::memory_order_acquire)) {
        Frame frame;
        bool is_video = false;
        uint64_t generation = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            while (running_.load(std::memory_order_acquire)) {
                if (stopping_)
                    return;
                generation = generation_;
                if (client_ != nullptr && !client_->is_closed() &&
                    (!audio_queue_.empty() || !video_queue_.empty())) {
                    int64_t now = steady_ms();
                    bool video_first = !video_queue_.empty() &&
                                       (audio_queue_.empty() ||
                                        video_queue_.front().due <= audio_queue_.front().due);
                    auto &queue = video_first ? video_queue_ : audio_queue_;
                    int64_t wait_ms = queue.front().due - now;
                    if (wait_ms <= 0) {
                        frame = std::move(queue.front());
                        queue.pop_front();
                        is_video = video_first;
                        break;
                    }
                    sender_cv_.wait_for(lock,
                                        std::chrono::milliseconds(std::min<int64_t>(wait_ms, 100)));
                    continue;
                }
                sender_cv_.wait_for(lock, std::chrono::milliseconds(100));
            }
        }
        if (!running_.load(std::memory_order_acquire))
            return;

        // Sent outside the lock: TCP backpressure must not stall the feeder.
        try {
            Client *client = nullptr;
            bool stale = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                client = client_;
                stale = client == nullptr || generation != generation_;
            }
            if (client != nullptr && !client->is_closed() && !stale) {
                // The wire timestamps must never go backwards or repeat;
                // a decoder drops the track otherwise (the reference clamps
                // its video dts for the same reason). Clamp per track.
                uint32_t &last = is_video ? last_sent_video_ts_ : last_sent_audio_ts_;
                if (frame.timestamp <= last)
                    frame.timestamp = last + 1;
                last = frame.timestamp;
                if (is_video && frame.sequence_header) {
                    client->send_video(frame.timestamp, frame.body);
                } else if (is_video) {
                    client->send_video_annexb(frame.timestamp, frame.keyframe, frame.body,
                                              frame.composition_time);
                } else {
                    client->send_audio(frame.timestamp, frame.body);
                }
                frames_sent_.fetch_add(1, std::memory_order_relaxed);
                if (is_video)
                    video_frames_sent_.fetch_add(1, std::memory_order_relaxed);
                bytes_sent_.fetch_add(frame.body.size(), std::memory_order_relaxed);
            } else {
                frames_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (const std::exception &) {
            // The connection dropped: park until the next attach.
            std::lock_guard<std::mutex> lock(mutex_);
            client_ = nullptr;
            clear_queues();
            generation_++;
        }
        feed_cv_.notify_all();
    }
}

} // namespace tgrtmp
