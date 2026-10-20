// AMF0 (Action Message Format 0) encoder/decoder for the RTMP command and
// metadata messages, per the Adobe AMF0 spec (§2.1 of "Action Message Format
// -- AMF 0", 2006). Only the marker types RTMP servers actually use are
// supported: number, boolean, string, object, null, undefined, ECMA array,
// strict array, date and long string.
//
// This is a clean-room implementation written against the public spec.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace tgrtmp {

struct AmfValue;

// An ordered string -> value map. Encoded as an AMF0 ECMA array (0x08) when
// `ecma` is true (the type of @setDataFrame's onMetaData), otherwise as an
// AMF0 object (0x03).
struct AmfObject {
    bool ecma = false;
    // AmfValue is incomplete here; std::vector tolerates incomplete value
    // types as long as it is completed before members are used (C++17).
    std::vector<std::pair<std::string, AmfValue>> entries;

    AmfObject() = default;
    AmfObject(bool ecma_, std::vector<std::pair<std::string, AmfValue>> entries_)
        : ecma(ecma_), entries(std::move(entries_)) {}
};

struct AmfValue {
    std::variant<std::monostate, bool, double, std::string, std::vector<AmfValue>, AmfObject> v;

    AmfValue() = default;
    AmfValue(std::monostate) {}
    AmfValue(bool b) : v(b) {}
    AmfValue(double d) : v(d) {}
    AmfValue(int i) : v(static_cast<double>(i)) {}
    AmfValue(const char *s) : v(std::string(s)) {}
    AmfValue(std::string s) : v(std::move(s)) {}
    AmfValue(std::string_view s) : v(std::string(s)) {}
    AmfValue(std::vector<AmfValue> list) : v(std::move(list)) {}
    AmfValue(AmfObject obj) : v(std::move(obj)) {}

    bool is_null() const { return std::holds_alternative<std::monostate>(v); }
    bool is_number() const { return std::holds_alternative<double>(v); }
    bool is_string() const { return std::holds_alternative<std::string>(v); }
    bool is_object() const { return std::holds_alternative<AmfObject>(v); }

    double as_number(double fallback = 0.0) const {
        if (auto *d = std::get_if<double>(&v))
            return *d;
        return fallback;
    }
    const std::string *as_string() const { return std::get_if<std::string>(&v); }
    const AmfObject *as_object() const { return std::get_if<AmfObject>(&v); }

    // Object/ECMA property lookup by key.
    const AmfValue *find(std::string_view key) const {
        if (auto *obj = std::get_if<AmfObject>(&v)) {
            for (const auto &[k, val] : obj->entries) {
                if (k == key)
                    return &val;
            }
        }
        return nullptr;
    }
};

namespace amf0 {

// Append the AMF0 encoding of `value` to `out`.
void encode(std::string &out, const AmfValue &value);

// Encode several values back to back, as the arguments of a command message.
template <typename... Rest>
void encode_args(std::string &out, const AmfValue &first, const Rest &...rest) {
    encode(out, first);
    (encode(out, rest), ...);
}

// Decode every value of a payload. Throws std::runtime_error on malformed or
// truncated data, or on unsupported markers.
std::vector<AmfValue> decode(std::string_view data);

inline AmfValue null_value() { return AmfValue(); }
inline AmfValue boolean(bool b) { return AmfValue(b); }
inline AmfValue number(double d) { return AmfValue(d); }
inline AmfValue string(std::string_view s) { return AmfValue(s); }

inline AmfValue object(std::vector<std::pair<std::string, AmfValue>> entries) {
    return AmfValue(AmfObject(false, std::move(entries)));
}
inline AmfValue ecma_array(std::vector<std::pair<std::string, AmfValue>> entries) {
    return AmfValue(AmfObject(true, std::move(entries)));
}

} // namespace amf0
} // namespace tgrtmp
