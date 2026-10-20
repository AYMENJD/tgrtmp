#include "amf0.h"

#include <cstring>

namespace tgrtmp::amf0 {

namespace {

constexpr uint8_t MARKER_NUMBER = 0x00;
constexpr uint8_t MARKER_BOOLEAN = 0x01;
constexpr uint8_t MARKER_STRING = 0x02;
constexpr uint8_t MARKER_OBJECT = 0x03;
constexpr uint8_t MARKER_NULL = 0x05;
constexpr uint8_t MARKER_UNDEFINED = 0x06;
constexpr uint8_t MARKER_ECMA_ARRAY = 0x08;
constexpr uint8_t MARKER_OBJECT_END = 0x09;
constexpr uint8_t MARKER_STRICT_ARRAY = 0x0A;
constexpr uint8_t MARKER_DATE = 0x0B;
constexpr uint8_t MARKER_LONG_STRING = 0x0C;

// Doubles are stored big-endian in AMF0.
void put_be64(std::string &out, double value) {
    uint64_t bits;
    std::memcpy(&bits, &value, 8);
    for (int i = 7; i >= 0; --i)
        out.push_back(static_cast<char>((bits >> (8 * i)) & 0xFF));
}

void put_be32(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 24) & 0xFF));
    out.push_back(static_cast<char>((v >> 16) & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void put_be16(std::string &out, uint32_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

struct Decoder {
    std::string_view data;
    size_t offset = 0;

    [[noreturn]] static void fail(const char *what) { throw std::runtime_error(what); }

    uint8_t u8() {
        if (offset >= data.size())
            fail("truncated AMF0 data");
        return static_cast<uint8_t>(data[offset++]);
    }

    uint32_t u24() {
        return (static_cast<uint32_t>(u8()) << 16) | (static_cast<uint32_t>(u8()) << 8) | u8();
    }

    uint32_t u32() {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v = (v << 8) | u8();
        return v;
    }

    uint32_t u16() { return (static_cast<uint32_t>(u8()) << 8) | u8(); }

    double f64() {
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i)
            bits = (bits << 8) | u8();
        double d;
        std::memcpy(&d, &bits, 8);
        return d;
    }

    std::string_view take(size_t length) {
        if (length > data.size() - offset)
            fail("truncated AMF0 data");
        std::string_view out = data.substr(offset, length);
        offset += length;
        return out;
    }

    std::string take_string() {
        uint32_t length = u16();
        return std::string(take(length));
    }

    void decode_properties(std::vector<std::pair<std::string, AmfValue>> &out) {
        while (true) {
            if (offset + 2 > data.size())
                fail("truncated AMF0 properties");
            uint32_t key_length = (static_cast<uint32_t>(static_cast<uint8_t>(data[offset])) << 8) |
                                  static_cast<uint8_t>(data[offset + 1]);
            if (key_length == 0) {
                // Object end: 0x00 0x00 followed by the object-end marker.
                if (offset + 3 > data.size())
                    fail("truncated AMF0 object end");
                if (static_cast<uint8_t>(data[offset + 2]) != MARKER_OBJECT_END) {
                    fail("missing AMF0 object end marker");
                }
                offset += 3;
                return;
            }
            offset += 2;
            std::string key(take(key_length));
            out.emplace_back(std::move(key), decode_value());
        }
    }

    AmfValue decode_value() {
        switch (uint8_t marker = u8(); marker) {
        case MARKER_NUMBER:
            return AmfValue(f64());
        case MARKER_BOOLEAN:
            return AmfValue(u8() != 0);
        case MARKER_STRING:
            return AmfValue(take_string());
        case MARKER_LONG_STRING:
            return AmfValue(std::string(take(u32())));
        case MARKER_NULL:
        case MARKER_UNDEFINED:
            return AmfValue();
        case MARKER_ECMA_ARRAY: {
            u32(); // The associative-count is only a hint.
            AmfObject obj(true, {});
            decode_properties(obj.entries);
            return AmfValue(std::move(obj));
        }
        case MARKER_OBJECT: {
            AmfObject obj(false, {});
            decode_properties(obj.entries);
            return AmfValue(std::move(obj));
        }
        case MARKER_STRICT_ARRAY: {
            uint32_t count = u32();
            std::vector<AmfValue> list;
            list.reserve(count);
            for (uint32_t i = 0; i < count; ++i)
                list.push_back(decode_value());
            return AmfValue(std::move(list));
        }
        case MARKER_DATE: {
            double ms = f64();
            u16(); // Time zone, unused (reserved to be 0).
            return AmfValue(ms);
        }
        default:
            fail("unsupported AMF0 marker");
        }
    }
};

void encode_properties(std::string &out, const AmfObject &obj) {
    for (const auto &[key, value] : obj.entries) {
        put_be16(out, static_cast<uint32_t>(key.size()));
        out.append(key);
        encode(out, value);
    }
    out.push_back('\x00');
    out.push_back('\x00');
    out.push_back(static_cast<char>(MARKER_OBJECT_END));
}

} // namespace

void encode(std::string &out, const AmfValue &value) {
    if (auto *b = std::get_if<bool>(&value.v)) {
        out.push_back(static_cast<char>(MARKER_BOOLEAN));
        out.push_back(*b ? '\x01' : '\x00');
        return;
    }
    if (auto *d = std::get_if<double>(&value.v)) {
        out.push_back(static_cast<char>(MARKER_NUMBER));
        put_be64(out, *d);
        return;
    }
    if (auto *s = std::get_if<std::string>(&value.v)) {
        if (s->size() > 0xFFFF) {
            out.push_back(static_cast<char>(MARKER_LONG_STRING));
            put_be32(out, static_cast<uint32_t>(s->size()));
        } else {
            out.push_back(static_cast<char>(MARKER_STRING));
            put_be16(out, static_cast<uint32_t>(s->size()));
        }
        out.append(*s);
        return;
    }
    if (auto *list = std::get_if<std::vector<AmfValue>>(&value.v)) {
        out.push_back(static_cast<char>(MARKER_STRICT_ARRAY));
        put_be32(out, static_cast<uint32_t>(list->size()));
        for (const auto &item : *list)
            encode(out, item);
        return;
    }
    if (auto *obj = std::get_if<AmfObject>(&value.v)) {
        if (obj->ecma) {
            out.push_back(static_cast<char>(MARKER_ECMA_ARRAY));
            put_be32(out, static_cast<uint32_t>(obj->entries.size()));
        } else {
            out.push_back(static_cast<char>(MARKER_OBJECT));
        }
        encode_properties(out, *obj);
        return;
    }
    // std::monostate: null and undefined both decode to it; encode null.
    out.push_back(static_cast<char>(MARKER_NULL));
}

std::vector<AmfValue> decode(std::string_view data) {
    Decoder decoder{data, 0};
    std::vector<AmfValue> values;
    while (decoder.offset < data.size()) {
        values.push_back(decoder.decode_value());
    }
    return values;
}

} // namespace tgrtmp::amf0
