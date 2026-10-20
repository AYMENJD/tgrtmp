// H.264 (AVC) bitstream helpers for RTMP/FLV publishing:
//
//  - splitting Annex-B byte streams (ITU-T H.264 Annex B) into NAL units,
//  - converting to the length-prefixed form FLV/RTMP carries (AVCC),
//  - building the AVCDecoderConfigurationRecord (ISO 14496-15 §5.3.3.1) from
//    the SPS/PPS of a keyframe,
//  - parsing an SPS for the coded dimensions (ITU-T H.264 §7.3.2.1.1).
//
// Clean-room implementation written against the public specs.

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace tgrtmp::h264 {

constexpr uint8_t NAL_SLICE = 1;
constexpr uint8_t NAL_IDR = 5;
constexpr uint8_t NAL_SEI = 6;
constexpr uint8_t NAL_SPS = 7;
constexpr uint8_t NAL_PPS = 8;
constexpr uint8_t NAL_AUD = 9;

inline uint8_t nal_type(unsigned char first_byte) { return first_byte & 0x1F; }

// Call fn(nalu) for each NAL unit of an Annex-B byte stream (start codes are
// stripped; NAL units are views into `data`).
template <typename Fn> void split_annexb(std::string_view data, Fn &&fn);

// Convert an Annex-B access unit to length-prefixed (4-byte) NAL units,
// as an FLV/RTMP AVC frame body carries them.
std::string to_avcc(std::string_view annexb);

// Whether the access unit contains an IDR NAL unit.
bool is_keyframe(std::string_view annexb);

struct AvccConfig {
    std::string record; // AVCDecoderConfigurationRecord.
    int width = 0;
    int height = 0;
};

// Extract the SPS/PPS of a keyframe into an AVCDecoderConfigurationRecord,
// with the coded dimensions. Throws std::runtime_error if the access unit
// carries no SPS or PPS.
AvccConfig describe(std::string_view annexb);

// Parse an SPS NAL unit (complete, including its NAL header byte) for the
// coded dimensions. Returns false on unsupported profiles.
bool sps_dimensions(std::string_view sps, int &width, int &height);

// Parse an AVCDecoderConfigurationRecord for the coded dimensions (through
// its first SPS). Returns false if the record carries no usable SPS.
bool avcc_dimensions(std::string_view record, int &width, int &height);

// Locate Annex-B start codes. Returns the index just past a start code's
// 0x01 byte at/after `from` (where the NAL unit content begins), or npos.
inline constexpr size_t npos = static_cast<size_t>(-1);
inline size_t find_start_code(std::string_view data, size_t from) {
    // The needle must carry its length explicitly: a string_view built from
    // the literal stops at the first NUL and would search for nothing.
    static constexpr std::string_view kStartCode("\x00\x00\x01", 3);
    size_t at = data.find(kStartCode, from);
    return at == std::string_view::npos ? npos : at + 3;
}

template <typename Fn> void split_annexb(std::string_view data, Fn &&fn) {
    size_t start = find_start_code(data, 0);
    if (start == npos)
        return;
    while (true) {
        size_t next = find_start_code(data, start);
        if (next == npos) {
            if (start < data.size())
                fn(data.substr(start));
            return;
        }
        // The NAL unit ends where the next start code's leading zeros begin;
        // a coded NAL unit never legitimately ends in 00 00 (emulation
        // prevention forbids it), so the back-scan only strips the prefix.
        size_t end = next - 3;
        while (end > start && data[end - 1] == '\x00')
            --end;
        fn(data.substr(start, end - start));
        start = next;
    }
}

} // namespace tgrtmp::h264
