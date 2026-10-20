// FLV tag bodies carried by RTMP audio (type 8) and video (type 9) messages,
// for H.264 (AVC) and AAC (the codecs Telegram's RTMP server accepts), plus
// ADTS parsing/building and the @setDataFrame onMetaData body.
//
// Clean-room implementation written against the public FLV/ISO-MPEG specs.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tgrtmp::flv {

constexpr uint8_t VIDEO_CODEC_AVC = 7;
constexpr uint8_t AUDIO_CODEC_AAC = 10;

// The sampling frequencies of an ADTS header or an AudioSpecificConfig, by index.
constexpr int SAMPLING_FREQUENCIES[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                          22050, 16000, 12000, 11025, 8000,  7350};

// The sequence header of an H.264 stream: its AVCDecoderConfigurationRecord.
std::string avc_sequence_header(std::string_view avcc);

// An H.264 frame as length-prefixed NAL units.
// composition_time = presentation timestamp - decoding timestamp, in ms.
std::string avc_frame(std::string_view nal_units, bool keyframe, uint32_t composition_time);

// The sequence header of an AAC stream: its AudioSpecificConfig.
std::string aac_sequence_header(std::string_view asc);

// A raw AAC frame (without ADTS).
std::string aac_frame(std::string_view raw);

// The @setDataFrame (onMetaData) data message body. width/height < 0 omits them.
std::string metadata(int64_t width, int64_t height, bool video, bool audio);

// Split an ADTS frame into the AudioSpecificConfig it describes and its raw
// AAC payload. `raw` points into `frame`. Returns false if it isn't ADTS.
bool parse_adts(std::string_view frame, std::string &asc, std::string_view &raw);

// The 7-byte ADTS header of a raw AAC frame. Throws std::runtime_error if the
// AudioSpecificConfig can't be carried by ADTS.
std::string adts_header(std::string_view asc, size_t payload_length);

// The sampling frequency described by an AudioSpecificConfig, in Hz. Throws.
int sample_rate(std::string_view asc);

// Read the core-coder audio object type, the sampling frequency index and the
// channel configuration of an AudioSpecificConfig. Throws on unsupported
// configurations (explicit sampling frequency, program config element).
void parse_audio_specific_config(std::string_view asc, int &object_type, int &frequency_index,
                                 int &channels);

} // namespace tgrtmp::flv
