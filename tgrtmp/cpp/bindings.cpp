// Python bindings (nanobind) for the tgrtmp core.

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/unique_ptr.h>
#include <nanobind/stl/vector.h>

#include <memory>
#include <thread>

#include "amf0.h"
#include "chunk.h"
#include "client.h"
#include "flv.h"
#include "h264.h"
#include "publisher.h"
#include "sink.h"
#include "source.h"

namespace nb = nanobind;
using namespace tgrtmp;

namespace {

// Media/feeder calls block on the network or the pacer: run them without the
// GIL. Nothing in the core ever calls back into Python, so this is safe.
using release_gil = nb::call_guard<nb::gil_scoped_release>;

Client::Options make_options(bool verify_tls, double connect_timeout, double command_timeout) {
    Client::Options options;
    options.verify_tls = verify_tls;
    options.connect_timeout_s = connect_timeout;
    options.command_timeout_s = command_timeout;
    return options;
}

std::string_view as_view(nb::bytes data) { return std::string_view(data.c_str(), data.size()); }

nb::object amf_to_python(const AmfValue &value) {
    if (auto *b = std::get_if<bool>(&value.v))
        return nb::bool_(*b);
    if (auto *d = std::get_if<double>(&value.v))
        return nb::float_(*d);
    if (auto *s = std::get_if<std::string>(&value.v))
        return nb::str(s->c_str());
    if (auto *list = std::get_if<std::vector<AmfValue>>(&value.v)) {
        nb::list out;
        for (const auto &item : *list)
            out.append(amf_to_python(item));
        return out;
    }
    if (auto *obj = std::get_if<AmfObject>(&value.v)) {
        nb::dict out;
        for (const auto &[key, item] : obj->entries)
            out[key.c_str()] = amf_to_python(item);
        return out;
    }
    return nb::none();
}

AmfValue python_to_amf(const nb::object &value, bool top_level_dict_is_ecma) {
    if (value.is_none())
        return amf0::null_value();
    if (nb::isinstance<nb::bool_>(value))
        return amf0::boolean(nb::cast<bool>(value));
    if (nb::isinstance<nb::int_>(value)) {
        return amf0::number(static_cast<double>(nb::cast<long long>(value)));
    }
    if (nb::isinstance<nb::float_>(value))
        return amf0::number(nb::cast<double>(value));
    if (nb::isinstance<nb::str>(value)) {
        return amf0::string(std::string_view(nb::borrow<nb::str>(value).c_str()));
    }
    if (nb::isinstance<nb::bytes>(value)) {
        return amf0::string(as_view(nb::borrow<nb::bytes>(value)));
    }
    if (nb::isinstance<nb::list>(value)) {
        std::vector<AmfValue> items;
        for (nb::handle item : nb::borrow<nb::list>(value)) {
            items.push_back(python_to_amf(nb::borrow<nb::object>(item), false));
        }
        return AmfValue(std::move(items));
    }
    if (nb::isinstance<nb::tuple>(value)) {
        std::vector<AmfValue> items;
        for (nb::handle item : nb::borrow<nb::tuple>(value)) {
            items.push_back(python_to_amf(nb::borrow<nb::object>(item), false));
        }
        return AmfValue(std::move(items));
    }
    if (nb::isinstance<nb::dict>(value)) {
        std::vector<std::pair<std::string, AmfValue>> entries;
        for (auto [key, item] : nb::borrow<nb::dict>(value)) {
            entries.emplace_back(nb::borrow<nb::str>(key).c_str(),
                                 python_to_amf(nb::borrow<nb::object>(item), false));
        }
        return AmfValue(AmfObject(top_level_dict_is_ecma, std::move(entries)));
    }
    throw std::runtime_error("Cannot convert the value to AMF0");
}

// A ChunkParser for tests and tools: feed() returns every complete message
// as a (type_id, timestamp_ms, message_stream_id, payload) tuple, and an
// optional handler sees them as they are parsed.
class PyChunkParser {
  public:
    nb::list feed(nb::bytes data) {
        nb::list messages;
        parser_.set_handler([&](uint8_t type, uint32_t timestamp, uint32_t stream,
                                std::string_view payload) {
            nb::tuple message =
                nb::make_tuple(type, timestamp, stream, nb::bytes(payload.data(), payload.size()));
            messages.append(message);
            if (handler_.is_valid()) {
                handler_(type, timestamp, stream, nb::bytes(payload.data(), payload.size()));
            }
        });
        parser_.feed(as_view(data));
        return messages;
    }
    void set_handler(nb::object handler) {
        // None clears the handler (breaking the handler -> parser cycle).
        handler_ = handler.is_none() ? nb::callable() : nb::borrow<nb::callable>(handler);
    }
    void set_chunk_size(uint32_t size) { parser_.set_chunk_size(size); }
    void abort_stream(uint32_t chunk_stream) { parser_.abort_stream(chunk_stream); }

    // For the GC traversal/clear slots: the handler is the only Python
    // reference this object holds.
    nb::object handler_object() const { return handler_; }
    void clear_for_gc() {
        parser_.set_handler({});
        handler_ = {};
    }

    ~PyChunkParser() {
        // Break the handler cycle (the handler may reference this parser).
        parser_.set_handler({});
        handler_ = {};
    }

  private:
    ChunkParser parser_;
    nb::callable handler_;
};

// Classes that hold Python references inside C++ members implement
// tp_traverse/tp_clear so the garbage collector can see through and collect
// reference cycles (nanobind's own destructors still run on deallocation).

int publisher_tp_traverse(PyObject *self, visitproc visit, void *arg) {
    if (!nb::inst_ready(self))
        return 0;
    Publisher *me = nb::inst_ptr<Publisher>(self);
    if (void *object = me->logger_object())
        Py_VISIT(static_cast<PyObject *>(object));
    return 0;
}

int publisher_tp_clear(PyObject *self) {
    Publisher *me = nb::inst_ptr<Publisher>(self);
    PyObject *released = static_cast<PyObject *>(me->release_logger_py());
    Py_XDECREF(released);
    me->clear_logger();
    return 0;
}

PyType_Slot publisher_slots[] = {{Py_tp_traverse, (void *)publisher_tp_traverse},
                                 {Py_tp_clear, (void *)publisher_tp_clear},
                                 {0, nullptr}};

int chunk_parser_tp_traverse(PyObject *self, visitproc visit, void *arg) {
    if (!nb::inst_ready(self))
        return 0;
    PyChunkParser *me = nb::inst_ptr<PyChunkParser>(self);
    nb::object handler = me->handler_object();
    if (handler.is_valid())
        Py_VISIT(handler.ptr());
    return 0;
}

int chunk_parser_tp_clear(PyObject *self) {
    PyChunkParser *me = nb::inst_ptr<PyChunkParser>(self);
    me->clear_for_gc();
    return 0;
}

PyType_Slot chunk_parser_slots[] = {{Py_tp_traverse, (void *)chunk_parser_tp_traverse},
                                    {Py_tp_clear, (void *)chunk_parser_tp_clear},
                                    {0, nullptr}};

} // namespace

NB_MODULE(_core, m) {
    m.doc() = "High-performance RTMP(S) publishing core for Telegram livestreams.";

    m.def(
        "amf0_encode",
        [](const nb::object &value, bool ecma_arrays) {
            std::string out;
            amf0::encode(out, python_to_amf(value, ecma_arrays));
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("value").none(), nb::arg("ecma_arrays") = false,
        "Encode one Python value as AMF0 (dicts become objects, or ECMA arrays when asked).");

    m.def(
        "amf0_decode",
        [](nb::bytes data) {
            nb::list out;
            for (const auto &value : amf0::decode(as_view(data)))
                out.append(amf_to_python(value));
            return out;
        },
        nb::arg("data"), "Decode every AMF0 value of a payload.");

    m.def(
        "chunk_write",
        [](uint32_t chunk_stream, uint8_t type_id, uint32_t message_stream_id, uint32_t timestamp,
           nb::bytes payload, uint32_t chunk_size) {
            std::string out;
            out.reserve(payload.size() + 32);
            ChunkWriter writer(chunk_size);
            writer.write_message(out, chunk_stream, type_id, message_stream_id, timestamp,
                                 as_view(payload));
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("chunk_stream"), nb::arg("type_id"), nb::arg("message_stream_id"),
        nb::arg("timestamp"), nb::arg("payload"), nb::arg("chunk_size") = 4096,
        "Chunk one RTMP message, as the stream carries it.");

    m.def(
        "chunk_write_avc",
        [](uint32_t chunk_stream, uint32_t message_stream_id, uint32_t timestamp, bool keyframe,
           nb::bytes annexb, uint32_t composition_time, uint32_t chunk_size) {
            std::string out;
            ChunkWriter writer(chunk_size);
            writer.write_avc_frame(out, chunk_stream, message_stream_id, timestamp, keyframe,
                                   as_view(annexb), composition_time);
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("chunk_stream"), nb::arg("message_stream_id"), nb::arg("timestamp"),
        nb::arg("keyframe"), nb::arg("annexb"), nb::arg("composition_time"),
        nb::arg("chunk_size") = 4096,
        "Build one video message straight from an Annex-B access unit, in a single pass "
        "(byte-identical to chunk_write of the wrapped body).");

    nb::class_<PyChunkParser>(m, "ChunkParser", nb::type_slots(chunk_parser_slots))
        .def(nb::init<>())
        .def("feed", &PyChunkParser::feed, nb::arg("data"),
             "Parse chunks; returns every complete message as "
             "(type_id, timestamp_ms, message_stream_id, payload: bytes).")
        .def("set_handler", &PyChunkParser::set_handler, nb::arg("handler").none(),
             "handler(type_id, timestamp_ms, message_stream_id, payload: bytes), called as "
             "messages are parsed; None clears it.")
        .def("set_chunk_size", &PyChunkParser::set_chunk_size, nb::arg("size"))
        .def("abort_stream", &PyChunkParser::abort_stream, nb::arg("chunk_stream"));

    m.def(
        "avcc_describe",
        [](nb::bytes annexb) {
            h264::AvccConfig config = h264::describe(as_view(annexb));
            nb::dict out;
            out["record"] = nb::bytes(config.record.data(), config.record.size());
            out["width"] = config.width;
            out["height"] = config.height;
            return out;
        },
        nb::arg("annexb"),
        "Extract the AVCDecoderConfigurationRecord and dimensions of an H.264 keyframe.");

    m.def(
        "sps_dimensions",
        [](nb::bytes sps) {
            int width = 0, height = 0;
            nb::dict out;
            out["ok"] = h264::sps_dimensions(as_view(sps), width, height);
            out["width"] = width;
            out["height"] = height;
            return out;
        },
        nb::arg("sps"), "Parse an SPS NAL unit for the coded dimensions.");

    m.def(
        "to_avcc",
        [](nb::bytes annexb) {
            std::string out = h264::to_avcc(as_view(annexb));
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("annexb"), "Annex-B access unit -> length-prefixed NAL units.");

    m.def(
        "is_keyframe", [](nb::bytes annexb) { return h264::is_keyframe(as_view(annexb)); },
        nb::arg("annexb"), "Whether an Annex-B access unit carries an IDR NAL unit.");

    m.def(
        "aac_sample_rate", [](nb::bytes asc) { return flv::sample_rate(as_view(asc)); },
        nb::arg("asc"), "The sampling frequency of an AudioSpecificConfig.");

    m.def(
        "adts_parse",
        [](nb::bytes frame) -> nb::object {
            std::string asc;
            std::string_view raw;
            if (!flv::parse_adts(as_view(frame), asc, raw))
                return nb::none();
            nb::dict out;
            out["asc"] = nb::bytes(asc.data(), asc.size());
            out["raw"] = nb::bytes(raw.data(), raw.size());
            return out;
        },
        nb::arg("frame"), "Split an ADTS frame into its AudioSpecificConfig and raw payload.");

    m.def(
        "adts_header",
        [](nb::bytes asc, size_t payload_length) {
            std::string out = flv::adts_header(as_view(asc), payload_length);
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("asc"), nb::arg("payload_length"), "Build the 7-byte ADTS header of a frame.");

    m.def(
        "flv_metadata",
        [](int64_t width, int64_t height, bool video, bool audio) {
            std::string out = flv::metadata(width, height, video, audio);
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("width"), nb::arg("height"), nb::arg("video"), nb::arg("audio"),
        "The onMetaData @setDataFrame body (dimensions < 0 are omitted).");

    m.def(
        "avc_frame",
        [](nb::bytes nal_units, bool keyframe, uint32_t composition_time) {
            std::string out = flv::avc_frame(as_view(nal_units), keyframe, composition_time);
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("nal_units"), nb::arg("keyframe"), nb::arg("composition_time"));

    m.def(
        "avc_sequence_header",
        [](nb::bytes avcc) {
            std::string out = flv::avc_sequence_header(as_view(avcc));
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("avcc"));

    m.def(
        "aac_frame",
        [](nb::bytes raw) {
            std::string out = flv::aac_frame(as_view(raw));
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("raw"));

    m.def(
        "aac_sequence_header",
        [](nb::bytes asc) {
            std::string out = flv::aac_sequence_header(as_view(asc));
            return nb::bytes(out.data(), out.size());
        },
        nb::arg("asc"));

    nb::class_<Client>(m, "Client")
        .def_prop_ro("is_closed", &Client::is_closed,
                     "Whether the connection is closed: the publisher has to reconnect.")
        .def_prop_ro("remote", &Client::remote,
                     "The IP address the connection actually reached.")
        .def_prop_ro("error", &Client::error, "Why the connection was closed, if by an error.")
        .def("close", &Client::close, "Stop publishing and close the connection.")
        .def("wait_closed", &Client::await_close, nb::arg("timeout") = 0.0, release_gil(),
             "Wait until the connection is closed. Returns False on timeout.")
        .def(
            "send_metadata",
            [](Client &self, nb::bytes amf_body) { self.send_metadata(as_view(amf_body)); },
            nb::arg("amf_body"), release_gil(),
            "Send the raw @setDataFrame AMF0 body of the onMetaData.")
        .def(
            "send_audio",
            [](Client &self, uint32_t timestamp_ms, nb::bytes body) {
                self.send_audio(timestamp_ms, as_view(body));
            },
            nb::arg("timestamp_ms"), nb::arg("body"), release_gil(),
            "Send an audio FLV tag body (an AAC sequence header or frame).")
        .def(
            "send_video",
            [](Client &self, uint32_t timestamp_ms, nb::bytes body) {
                self.send_video(timestamp_ms, as_view(body));
            },
            nb::arg("timestamp_ms"), nb::arg("body"), release_gil(),
            "Send a video FLV tag body (an AVC sequence header or frame).");

    m.def(
        "connect",
        [](std::string_view url, std::string_view key, bool verify_tls, double connect_timeout,
           double command_timeout) {
            return Client::publish(url, key,
                                   make_options(verify_tls, connect_timeout, command_timeout));
        },
        nb::arg("url"), nb::arg("key"), nb::arg("verify_tls") = true,
        nb::arg("connect_timeout") = 15.0, nb::arg("command_timeout") = 10.0, release_gil(),
        "Connect to an RTMP(S) server and start publishing a stream.\n\n"
        "`url` is the RTMP application, e.g. `rtmps://dc4-1.rtmp.t.me/s/`; `key` is the stream "
        "key. Raises RuntimeError if the server refuses the stream.");

    nb::class_<FrameSink>(m, "FrameSink").doc() = "Receives the demuxed frames of a source.";

    nb::class_<Publisher, FrameSink> publisher(m, "Publisher", nb::type_slots(publisher_slots));
    publisher
        .def(
            "__init__",
            [](Publisher *slot, uint32_t lookahead_ms, uint32_t max_drift_ms,
               uint32_t wait_video_ms, bool expect_video) {
                Publisher::Options options;
                options.lookahead_ms = lookahead_ms;
                options.max_drift_ms = max_drift_ms;
                options.wait_video_ms = wait_video_ms;
                options.expect_video = expect_video;
                new (slot) Publisher(options);
            },
            nb::arg("lookahead_ms") = 1000, nb::arg("max_drift_ms") = 1000,
            nb::arg("wait_video_ms") = 1000, nb::arg("expect_video") = false,
            "Paces demuxed frames to a live RTMP connection: keyframe gating, sequence "
            "headers, the onMetaData, and a bounded look-ahead.")
        .def("attach", &Publisher::attach, nb::arg("client"),
             "Attach a live connection (on every reconnect the timeline restarts).")
        .def("detach", &Publisher::detach, "Detach the current connection.")
        .def("stop", &Publisher::stop, "Stop the pacer thread for good.")
        .def("set_expect_video", &Publisher::set_expect_video, nb::arg("expect"),
             "Whether the source is expected to carry video.")
        .def(
            "feed_video",
            [](Publisher &self, int64_t dts_ms, int64_t pts_ms, nb::bytes annexb, bool keyframe,
               nb::bytes avcc_config) {
                self.feed_video(dts_ms, pts_ms, as_view(annexb), keyframe, as_view(avcc_config));
            },
            nb::arg("dts_ms"), nb::arg("pts_ms"), nb::arg("annexb"), nb::arg("keyframe"),
            nb::arg("avcc_config"), release_gil(),
            "Feed one H.264 access unit (Annex-B) with its timestamps in milliseconds.")
        .def(
            "feed_audio",
            [](Publisher &self, int64_t pts_ms, nb::bytes asc, nb::bytes raw) {
                self.feed_audio(pts_ms, as_view(asc), as_view(raw));
            },
            nb::arg("pts_ms"), nb::arg("asc"), nb::arg("raw"), release_gil(),
            "Feed one raw AAC frame with the AudioSpecificConfig of its track.")
        .def("drain", &Publisher::drain, nb::arg("timeout") = 10.0, release_gil(),
             "Wait until every queued frame has been sent (end of a finite source).")
        .def(
            "set_logger",
            [](Publisher &self, nb::object logger) {
                // The owned PyObject* is registered for GC traversal and
                // released by tp_clear/stop.
                Py_XDECREF(static_cast<PyObject *>(self.release_logger_py()));
                if (logger.is_none()) {
                    self.clear_logger();
                    return;
                }
                nb::callable callable = nb::borrow<nb::callable>(logger);
                PyObject *object = callable.ptr();
                Py_INCREF(object);
                self.set_logger_object(object);
                self.set_logger([callable](const std::string &message) {
                    nb::gil_scoped_acquire acquire;
                    try {
                        callable(message);
                    } catch (...) {
                        // A logging callback must never take the stream down.
                    }
                });
            },
            nb::arg("logger").none(),
            "logger(message: str), called with warnings about what the server will not "
            "accept; None clears it.")
        .def_prop_ro("warnings", &Publisher::warnings,
                     "How many warnings fired (0 in a clean, fully-publishable stream).")
        .def("on_discontinuity", &Publisher::on_discontinuity,
             "Signal a discontinuity of the source: the timeline re-anchors.")
        .def_prop_ro("connected", &Publisher::connected, "Whether a live connection is attached.")
        .def_prop_ro(
            "av_skew_ms", &Publisher::av_skew_ms,
            "The stream skew of the audio timeline against the video one, in ms; zero until "
            "both tracks are anchored, and constant for the session after that.")
        .def_prop_ro("frames_sent", &Publisher::frames_sent)
        .def_prop_ro("video_frames_sent", &Publisher::video_frames_sent)
        .def_prop_ro("bytes_sent", &Publisher::bytes_sent)
        .def_prop_ro("send_blocked_ms", &Publisher::send_blocked_ms,
                     "Cumulative time spent inside socket sends, in ms: grows fast on a "
                     "connection that cannot drain the stream.")
        .def_prop_ro("backlog_ms", &Publisher::backlog_ms,
                     "How far the oldest queued frame is past its send time, in ms; "
                     "near zero is healthy, growth means the connection cannot keep up.")
        .def_prop_ro("frames_dropped", &Publisher::frames_dropped);

    nb::class_<CollectorSink, FrameSink> collector(m, "CollectorSink");
    collector.def(nb::init<>())
        .def(
            "clear", [](CollectorSink &self) { self.clear(); },
            "Drop the recorded frames, warnings and jumps.")
        .def_prop_ro(
            "has_video", [](CollectorSink &self) { return self.has_video; },
            "Whether the container declared a video track.")
        .def_prop_ro("frames",
                     [](CollectorSink &self) {
                         nb::list out;
                         for (const auto &frame : self.frames) {
                             nb::dict item;
                             item["video"] = frame.video;
                             item["dts_ms"] = frame.dts_ms;
                             item["pts_ms"] = frame.pts_ms;
                             item["keyframe"] = frame.keyframe;
                             item["data"] = nb::bytes(frame.data.data(), frame.data.size());
                             item["config"] = nb::bytes(frame.config.data(), frame.config.size());
                             out.append(item);
                         }
                         return out;
                     })
        .def_prop_ro("jumps", [](CollectorSink &self) { return self.jumps; })
        .def_prop_ro(
            "warnings",
            [](CollectorSink &self) {
                nb::list out;
                for (const std::string &warning : self.warnings) {
                    out.append(warning);
                }
                return out;
            },
            "Warnings about what the server will not accept.");

    nb::class_<Source>(m, "Source")
        .def(nb::init<FrameSink &>(), nb::arg("sink"),
             "Demuxes a container stream (MPEG-TS, MP4 faststart/fragmented, MKV/WebM) and "
             "feeds its H.264/AAC frames to a sink (a Publisher or a CollectorSink), without "
             "transcoding.")
        .def(
            "feed", [](Source &self, nb::bytes data) { self.feed(as_view(data)); }, nb::arg("data"),
            release_gil(), "Feed raw container bytes (any chunking).")
        .def(
            "feed_at",
            [](Source &self, nb::bytes data, uint64_t offset) {
                self.feed_at(as_view(data), offset);
            },
            nb::arg("data"), nb::arg("offset"), release_gil(),
            "Feed MP4 media bytes located at an absolute file offset (after priming).")
        .def(
            "prime",
            [](Source &self, nb::bytes moov) { self.prime(as_view(moov)); }, nb::arg("moov"),
            release_gil(),
            "Prime the MP4 sample table from a moov box read elsewhere in the file.")
        .def("finish", &Source::finish, release_gil(), "Signal the end of the stream.")
        .def_prop_ro("saw_video", &Source::saw_video, "Whether a video track was seen.");
}
