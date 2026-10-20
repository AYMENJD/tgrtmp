#include "h264.h"

#include <vector>

namespace tgrtmp::h264 {

namespace {

// MSB-first bit reader for RBSP parsing.
struct BitReader {
    std::string_view data;
    size_t offset = 0; // In bits.

    [[noreturn]] static void fail(const char *what) { throw std::runtime_error(what); }

    int u(int bits) {
        int value = 0;
        while (bits-- > 0) {
            if (offset >= data.size() * 8)
                fail("truncated SPS");
            value = (value << 1) |
                    ((static_cast<unsigned char>(data[offset >> 3]) >> (7 - (offset & 7))) & 1);
            ++offset;
        }
        return value;
    }

    int ue() { // Unsigned Exp-Golomb.
        int zeros = 0;
        while (u(1) == 0) {
            if (++zeros > 31)
                fail("invalid Exp-Golomb code");
        }
        return (1 << zeros) - 1 + (zeros == 0 ? 0 : u(zeros));
    }

    int se() { // Signed Exp-Golomb.
        int code = ue();
        return (code & 1) ? (code + 1) / 2 : -(code / 2);
    }
};

// Remove emulation prevention bytes (00 00 03) from an RBSP.
std::string unescape_rbsp(std::string_view nalu) {
    std::string out;
    out.reserve(nalu.size());
    size_t zeros = 0;
    for (char c : nalu) {
        if (zeros >= 2 && static_cast<unsigned char>(c) == 0x03) {
            zeros = 0;
            continue;
        }
        zeros = (static_cast<unsigned char>(c) == 0x00) ? zeros + 1 : 0;
        out.push_back(c);
    }
    return out;
}

int sub_width_c(int chroma_format_idc) {
    return chroma_format_idc == 0 || chroma_format_idc == 3 ? 1 : 2;
}

void skip_scaling_list(BitReader &r, int size) {
    int last_scale = 8, next_scale = 8;
    for (int i = 0; i < size * 8 / 8; ++i) { // size entries of delta_scale.
        (void)i;
        if (next_scale != 0) {
            int delta = r.se();
            next_scale = (last_scale + delta + 256) % 256;
        }
        if (next_scale != 0)
            last_scale = next_scale;
    }
}

} // namespace

std::string to_avcc(std::string_view annexb) {
    std::string out;
    out.reserve(annexb.size() + 16);
    split_annexb(annexb, [&](std::string_view nalu) {
        if (nalu.empty())
            return;
        uint32_t length = static_cast<uint32_t>(nalu.size());
        out.push_back(static_cast<char>((length >> 24) & 0xFF));
        out.push_back(static_cast<char>((length >> 16) & 0xFF));
        out.push_back(static_cast<char>((length >> 8) & 0xFF));
        out.push_back(static_cast<char>(length & 0xFF));
        out.append(nalu);
    });
    return out;
}

bool is_keyframe(std::string_view annexb) {
    bool keyframe = false;
    split_annexb(annexb, [&](std::string_view nalu) {
        if (!nalu.empty() && nal_type(static_cast<unsigned char>(nalu[0])) == NAL_IDR)
            keyframe = true;
    });
    return keyframe;
}

bool sps_dimensions(std::string_view sps, int &width, int &height) {
    try {
        if (sps.empty() || nal_type(static_cast<unsigned char>(sps[0])) != NAL_SPS)
            return false;
        std::string rbsp = unescape_rbsp(sps.substr(1));
        BitReader r{rbsp, 0};
        int profile_idc = r.u(8);
        (void)r.u(8); // Constraint flags.
        (void)r.u(8); // Level idc.
        (void)r.ue(); // seq_parameter_set_id.

        int chroma_format_idc = 1;
        static const int HIGH_PROFILES[] = {100, 110, 122, 244, 44,  83, 86,
                                            118, 128, 138, 139, 134, 135};
        bool high = false;
        for (int p : HIGH_PROFILES) {
            if (p == profile_idc) {
                high = true;
                break;
            }
        }
        if (high) {
            chroma_format_idc = r.ue();
            if (chroma_format_idc == 3)
                (void)r.u(1); // separate_colour_plane_flag.
            (void)r.ue();     // bit_depth_luma_minus8.
            (void)r.ue();     // bit_depth_chroma_minus8.
            (void)r.u(1);     // qpprime_y_zero_transform_bypass_flag.
            if (r.u(1)) {     // seq_scaling_matrix_present_flag.
                int lists = (chroma_format_idc != 3) ? 8 : 12;
                for (int i = 0; i < lists; ++i) {
                    if (r.u(1))
                        skip_scaling_list(r, i < 6 ? 16 : 64);
                }
            }
        }

        (void)r.ue(); // log2_max_frame_num_minus4.
        int poc_type = r.ue();
        if (poc_type == 0) {
            (void)r.ue(); // log2_max_pic_order_cnt_lsb_minus4.
        } else if (poc_type == 1) {
            (void)r.u(1); // delta_pic_order_always_zero_flag.
            (void)r.se(); // offset_for_non_ref_pic.
            (void)r.se(); // offset_for_top_to_bottom_field.
            int cycles = r.ue();
            for (int i = 0; i < cycles; ++i)
                (void)r.se();
        }

        (void)r.ue();            // max_num_ref_frames.
        (void)r.u(1);            // gaps_in_frame_num_value_allowed_flag.
        int width_mbs = r.ue();  // pic_width_in_mbs_minus1.
        int height_map = r.ue(); // pic_height_in_map_units_minus1.
        int frame_mbs_only = r.u(1);
        if (!frame_mbs_only)
            (void)r.u(1); // mb_adaptive_frame_field_flag.
        (void)r.u(1);     // direct_8x8_inference_flag.
        int crop = r.u(1);

        int w = (width_mbs + 1) * 16;
        int h = (2 - frame_mbs_only) * (height_map + 1) * 16;
        if (crop) {
            int crop_left = r.ue();
            int crop_right = r.ue();
            int crop_top = r.ue();
            int crop_bottom = r.ue();
            int sw = sub_width_c(chroma_format_idc);
            int sh = sub_width_c(chroma_format_idc); // SubHeightC mirrors SubWidthC.
            w -= (crop_left + crop_right) * sw;
            h -= (crop_top + crop_bottom) * sh * (2 - frame_mbs_only);
        }
        width = w;
        height = h;
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

AvccConfig describe(std::string_view annexb) {
    std::vector<std::string_view> sps_list, pps_list;
    split_annexb(annexb, [&](std::string_view nalu) {
        if (nalu.empty())
            return;
        switch (nal_type(static_cast<unsigned char>(nalu[0]))) {
        case NAL_SPS:
            if (sps_list.size() < 31)
                sps_list.push_back(nalu);
            break;
        case NAL_PPS:
            if (pps_list.size() < 31)
                pps_list.push_back(nalu);
            break;
        default:
            break;
        }
    });
    if (sps_list.empty() || pps_list.empty()) {
        throw std::runtime_error("the H.264 keyframe carries no SPS/PPS to describe the track");
    }

    AvccConfig info;
    std::string &record = info.record;
    record.reserve(7 + sps_list.size() * 3 + pps_list.size() * 3);
    record.push_back('\x01');                // configurationVersion.
    record.append(sps_list[0].substr(1, 3)); // profile_idc, constraints, level_idc.
    record.push_back('\xFF');                // Reserved(6) | lengthSizeMinusOne(2) = 3.
    record.push_back(static_cast<char>(0xE0 | sps_list.size()));
    for (auto sps : sps_list) {
        record.push_back(static_cast<char>((sps.size() >> 8) & 0xFF));
        record.push_back(static_cast<char>(sps.size() & 0xFF));
        record.append(sps);
    }
    record.push_back(static_cast<char>(pps_list.size()));
    for (auto pps : pps_list) {
        record.push_back(static_cast<char>((pps.size() >> 8) & 0xFF));
        record.push_back(static_cast<char>(pps.size() & 0xFF));
        record.append(pps);
    }
    if (!sps_dimensions(sps_list[0], info.width, info.height))
        info.width = info.height = 0;
    return info;
}

bool avcc_dimensions(std::string_view record, int &width, int &height) {
    // AVCDecoderConfigurationRecord (ISO 14496-15): version(1), profile(1),
    // compat(1), level(1), 0xFF (lengthSizeMinusOne=3), 0xE0|numSPS, then the
    // SPS NAL units, each length-prefixed with two bytes.
    if (record.size() < 7)
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(record.data());
    size_t num_sps = p[5] & 0x1F;
    size_t offset = 6;
    for (size_t i = 0; i < num_sps; ++i) {
        if (offset + 2 > record.size())
            return false;
        size_t length = (static_cast<size_t>(p[offset]) << 8) | p[offset + 1];
        offset += 2;
        if (offset + length > record.size())
            return false;
        if (nal_type(p[offset]) == NAL_SPS) {
            return sps_dimensions(record.substr(offset, length), width, height);
        }
        offset += length;
    }
    return false;
}

} // namespace tgrtmp::h264
