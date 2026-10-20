#include "flv.h"

#include <stdexcept>

#include "amf0.h"

namespace tgrtmp::flv {

std::string avc_sequence_header(std::string_view avcc) {
    // FLV video tag body: (keyframe << 4 | codec) | AVCPacketType=0 (sequence
    // header), then a 3-byte composition time of zero, then the record.
    std::string out;
    out.reserve(5 + avcc.size());
    out.push_back(static_cast<char>((1 << 4) | VIDEO_CODEC_AVC));
    out.append("\x00\x00\x00\x00", 4);
    out.append(avcc);
    return out;
}

std::string avc_frame(std::string_view nal_units, bool keyframe, uint32_t composition_time) {
    std::string out;
    out.reserve(5 + nal_units.size());
    out.push_back(static_cast<char>(((keyframe ? 1 : 2) << 4) | VIDEO_CODEC_AVC));
    out.push_back('\x01'); // AVCPacketType=1: NALUs.
    out.push_back(static_cast<char>((composition_time >> 16) & 0xFF));
    out.push_back(static_cast<char>((composition_time >> 8) & 0xFF));
    out.push_back(static_cast<char>(composition_time & 0xFF));
    out.append(nal_units);
    return out;
}

std::string aac_sequence_header(std::string_view asc) {
    // Whatever the real rate and channels: for AAC, the AudioSpecificConfig
    // describes them.
    std::string out;
    out.reserve(2 + asc.size());
    out.push_back(static_cast<char>(0xAF)); // SoundFormat=AAC | rate/channels ignored.
    out.push_back('\x00');                  // AACPacketType=0: sequence header.
    out.append(asc);
    return out;
}

std::string aac_frame(std::string_view raw) {
    std::string out;
    out.reserve(2 + raw.size());
    out.push_back(static_cast<char>(0xAF));
    out.push_back('\x01'); // AACPacketType=1: raw frame.
    out.append(raw);
    return out;
}

std::string metadata(int64_t width, int64_t height, bool video, bool audio) {
    std::vector<std::pair<std::string, AmfValue>> values;
    values.emplace_back("duration", AmfValue(0.0));
    if (video) {
        values.emplace_back("videocodecid", AmfValue(static_cast<double>(VIDEO_CODEC_AVC)));
        if (width >= 0 && height >= 0) {
            values.emplace_back("width", AmfValue(static_cast<double>(width)));
            values.emplace_back("height", AmfValue(static_cast<double>(height)));
        }
    }
    if (audio) {
        values.emplace_back("audiocodecid", AmfValue(static_cast<double>(AUDIO_CODEC_AAC)));
    }
    values.emplace_back("encoder", AmfValue("tgrtmp"));

    std::string out;
    amf0::encode_args(out, AmfValue("@setDataFrame"), AmfValue("onMetaData"),
                      amf0::ecma_array(std::move(values)));
    return out;
}

void parse_audio_specific_config(std::string_view asc, int &object_type, int &frequency_index,
                                 int &channels) {
    if (asc.size() < 2)
        throw std::runtime_error("missing the AudioSpecificConfig of the AAC audio");

    // Minimal MSB-first bit reader over the config bytes.
    size_t offset = 0;
    size_t bits_left = asc.size() * 8;
    auto read = [&](int count) -> int {
        if (static_cast<size_t>(count) > bits_left)
            throw std::runtime_error("truncated AudioSpecificConfig");
        int value = 0;
        while (count-- > 0) {
            value = (value << 1) |
                    ((static_cast<unsigned char>(asc[offset >> 3]) >> (7 - (offset & 7))) & 1);
            ++offset;
            --bits_left;
        }
        return value;
    };

    object_type = read(5);
    if (object_type == 31)
        object_type = 32 + read(6);
    frequency_index = read(4);
    if (frequency_index == 15) {
        throw std::runtime_error(
            "AAC audio with an explicit sampling frequency is not supported, please re-encode it "
            "at a standard rate (e.g. 48000 Hz)");
    }
    channels = read(4);
    if (channels == 0) {
        throw std::runtime_error(
            "AAC audio whose channels are described by a program config element is not supported, "
            "please re-encode it as mono or stereo");
    }
    if (object_type == 5 || object_type == 29) {
        // HE-AAC (SBR, PS) signalled explicitly: ADTS carries the core AAC
        // coder, at the sampling frequency above; decoders detect SBR
        // implicitly.
        int extension_index = read(4);
        if (extension_index == 15)
            read(24);
        object_type = read(5);
        if (object_type == 31)
            object_type = 32 + read(6);
    }
}

int sample_rate(std::string_view asc) {
    int object_type = 0, frequency_index = 0, channels = 0;
    parse_audio_specific_config(asc, object_type, frequency_index, channels);
    return SAMPLING_FREQUENCIES[frequency_index];
}

bool parse_adts(std::string_view frame, std::string &asc, std::string_view &raw) {
    if (frame.size() < 7 || static_cast<unsigned char>(frame[0]) != 0xFF)
        return false;
    unsigned char b1 = static_cast<unsigned char>(frame[1]);
    // syncword(12)=1, MPEG version(1)=0, layer(2)=00, protection_absent(1).
    if ((b1 & 0xF6) != 0xF0)
        return false;
    size_t header_length = (b1 & 0x01) ? 7 : 9; // Two more bytes for the CRC, if present.
    if (frame.size() < header_length)
        return false;
    unsigned char b2 = static_cast<unsigned char>(frame[2]);
    unsigned char b3 = static_cast<unsigned char>(frame[3]);
    unsigned char b4 = static_cast<unsigned char>(frame[4]);
    unsigned char b5 = static_cast<unsigned char>(frame[5]);
    int object_type = (b2 >> 6) + 1;
    int frequency_index = (b2 >> 2) & 0x0F;
    int channels = ((b2 & 0x01) << 2) | (b3 >> 6);
    // audioObjectType (5 bits), samplingFrequencyIndex (4), channelConfiguration
    // (4), then the GASpecificConfig: 1024 samples per frame, no core coder,
    // no extension (3 zero bits).
    uint16_t config =
        static_cast<uint16_t>((object_type << 11) | (frequency_index << 7) | (channels << 3));
    asc.assign(2, '\x00');
    asc[0] = static_cast<char>(config >> 8);
    asc[1] = static_cast<char>(config & 0xFF);
    // The declared frame length bounds the payload: a PES often carries
    // several ADTS frames back to back.
    size_t declared = (static_cast<size_t>(b3 & 0x03) << 11) | (static_cast<size_t>(b4) << 3) |
                      (static_cast<size_t>(b5) >> 5);
    if (declared < header_length)
        return false;
    size_t payload = declared - header_length;
    if (payload > frame.size() - header_length)
        return false; // A partial frame: wait.
    raw = frame.substr(header_length, payload);
    return true;
}

std::string adts_header(std::string_view asc, size_t payload_length) {
    int object_type = 0, frequency_index = 0, channels = 0;
    parse_audio_specific_config(asc, object_type, frequency_index, channels);
    if (object_type < 1 || object_type > 4) {
        throw std::runtime_error("AAC audio object type " + std::to_string(object_type) +
                                 " is not supported, please re-encode the audio as AAC-LC");
    }
    if (payload_length + 7 > 0x1FFF) {
        throw std::runtime_error("AAC frame too large for an ADTS header");
    }
    size_t length = payload_length + 7;
    std::string out;
    out.reserve(7);
    out.push_back('\xFF');
    out.push_back('\xF1'); // MPEG-4, layer 0, no CRC.
    out.push_back(
        static_cast<char>(((object_type - 1) << 6) | (frequency_index << 2) | (channels >> 2)));
    out.push_back(static_cast<char>((((channels & 3) << 6) | (length >> 11)) & 0xFF));
    out.push_back(static_cast<char>((length >> 3) & 0xFF));
    out.push_back(static_cast<char>((((length & 7) << 5) | 0x1F) & 0xFF));
    out.push_back('\xFC');
    return out;
}

} // namespace tgrtmp::flv
