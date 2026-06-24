#include "include/mp3_reader.hpp"
#include "include/logger.hpp"
#include "include/bitstream.hpp"
#include <iostream>

namespace mp3packer {

Mp3Reader::Mp3Reader(const std::string& filename) : file_(filename, std::ios::binary), file_size_(0), start_junk_(), end_junk_() {
    if (!file_) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    file_.seekg(0, std::ios::end);
    file_size_ = static_cast<size_t>(file_.tellg());
    file_.seekg(0, std::ios::beg);
    skip_id3v2_tag();
}

Mp3Reader::~Mp3Reader() {
    if (file_.is_open()) {
        file_.close();
    }
}

void Mp3Reader::skip_id3v2_tag() {
    uint8_t header[10];
    if (!file_.read(reinterpret_cast<char*>(header), 10)) {
        file_.clear();
        file_.seekg(0, std::ios::beg);
        return;
    }

    if (header[0] == 'I' && header[1] == 'D' && header[2] == '3') {
        const uint32_t size = (static_cast<uint32_t>(header[6] & 0x7F) << 21) |
                        (static_cast<uint32_t>(header[7] & 0x7F) << 14) |
                        (static_cast<uint32_t>(header[8] & 0x7F) << 7)  |
                        static_cast<uint32_t>(header[9] & 0x7F);

        uint32_t total_tag_size = 10 + size;
        if (header[5] & 0x10) { // footer present flag
            total_tag_size += 10;
        }

        DEBUG_LOG("Found ID3v2 tag of size " << total_tag_size << " bytes. Storing in start_junk...");
        file_.seekg(0, std::ios::beg);
        start_junk_.resize(total_tag_size);
        file_.read(reinterpret_cast<char*>(start_junk_.data()), total_tag_size);
    } else {
        file_.seekg(0, std::ios::beg);
    }
}

std::optional<Mp3Header> Mp3Reader::parse_header(const uint32_t header_bits) {
    if ((header_bits & 0xFFE00000) != 0xFFE00000) return std::nullopt;

    Mp3Header header{};
    const uint32_t version_id = (header_bits >> 19) & 3;
    switch (version_id) {
        case 0: header.version = MpegVersion::MPEG25; break;
        case 2: header.version = MpegVersion::MPEG2;  break;
        case 3: header.version = MpegVersion::MPEG1;  break;
        default: return std::nullopt;
    }

    const uint32_t layer = (header_bits >> 17) & 3;
    if (layer != 1) return std::nullopt; // only layer iii

    header.has_crc = !((header_bits >> 16) & 1);

    const uint32_t bitrate_index = (header_bits >> 12) & 0xF;
    if (bitrate_index == 0 || bitrate_index == 15) return std::nullopt;

    const uint32_t samplerate_index = (header_bits >> 10) & 3;
    if (samplerate_index == 3) return std::nullopt;

    static constexpr uint32_t samplerates[3][3] = {
        {44100, 48000, 32000}, // mpeg 1
        {22050, 24000, 16000}, // mpeg 2
        {11025, 12000, 8000}   // mpeg 2.5
    };
    header.samplerate = samplerates[static_cast<uint32_t>(header.version)][samplerate_index];

    header.padding    = static_cast<bool>((header_bits >> 9) & 1);
    header.private_bit = static_cast<bool>((header_bits >> 8) & 1);

    const uint32_t mode = (header_bits >> 6) & 3;
    header.channel_mode = static_cast<ChannelMode>(mode);
    header.ms_stereo = (mode == 1) && static_cast<bool>((header_bits >> 5) & 1);
    header.is_stereo = (mode == 1) && static_cast<bool>((header_bits >> 4) & 1);

    header.copyright = static_cast<bool>((header_bits >> 3) & 1);
    header.original  = static_cast<bool>((header_bits >> 2) & 1);
    header.emphasis  = static_cast<Emphasis>(header_bits & 3);

    static const uint16_t bitrates[3][16] = {
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}, // mpeg 1
        {0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160, 0}, // mpeg 2
        {0,  8, 16, 24, 32, 40, 48, 56,  64,  80,  96, 112, 128, 144, 160, 0}  // mpeg 2.5
    };
    header.bitrate.bitrate_kbps = bitrates[static_cast<uint32_t>(header.version)][bitrate_index];
    header.bitrate.padding      = header.padding;
    header.bitrate.index        = static_cast<uint8_t>(bitrate_index);
    header.bitrate.frame_size   = calculate_frame_size(header);

    const int side_info_size = (header.version == MpegVersion::MPEG1)
                                ? (header.channel_mode == ChannelMode::Mono ? 17 : 32)
                                : (header.channel_mode == ChannelMode::Mono ?  9 : 17);
    header.bitrate.data_size = static_cast<uint16_t>(
        header.bitrate.frame_size - 4 - side_info_size - (header.has_crc ? 2 : 0));

    return header;
}

uint16_t Mp3Reader::calculate_frame_size(const Mp3Header& header) {
    const int mult = (header.version == MpegVersion::MPEG1) ? 144 : 72;
    return static_cast<uint16_t>(
        mult * static_cast<int>(header.bitrate.bitrate_kbps) * 1000
        / static_cast<int>(header.samplerate)
        + (header.padding ? 1 : 0));
}

std::optional<Mp3Frame> Mp3Reader::read_next_frame() {
    uint8_t buf[4];
    while (file_.read(reinterpret_cast<char*>(buf), 4)) {
        uint32_t header_bits = (static_cast<uint32_t>(buf[0]) << 24) |
                               (static_cast<uint32_t>(buf[1]) << 16) |
                               (static_cast<uint32_t>(buf[2]) << 8)  |
                               static_cast<uint32_t>(buf[3]);
        auto header = parse_header(header_bits);
        if (!header) {
            // invalid header: assume start of trailing junk (e.g. ID3v1 tag)
            file_.seekg(-4, std::ios::cur);
            size_t current_pos = static_cast<size_t>(file_.tellg());
            if (current_pos < file_size_) {
                size_t remaining = file_size_ - current_pos;
                end_junk_.resize(remaining);
                file_.read(reinterpret_cast<char*>(end_junk_.data()), static_cast<std::streamsize>(remaining));
                DEBUG_LOG("Found " << remaining << " bytes of junk at end of stream (e.g. ID3v1). Stored in end_junk.");
            }
            return std::nullopt;
        }

        DEBUG_LOG("Successfully parsed valid MP3 header: " << std::hex << header_bits << std::dec << " at stream pos " << (static_cast<size_t>(file_.tellg()) - 4));
        Mp3Frame frame;
        header->bitrate.frame_size = calculate_frame_size(*header);
        frame.header = *header;

        const int side_info_size = (header->version == MpegVersion::MPEG1)
                                   ? (header->channel_mode == ChannelMode::Mono ? 17 : 32)
                                   : (header->channel_mode == ChannelMode::Mono ?  9 : 17);

        uint8_t crc_bytes[2] = {0, 0};
        if (header->has_crc) {
            file_.read(reinterpret_cast<char*>(crc_bytes), 2);
        }

        frame.side_info_raw.resize(static_cast<size_t>(side_info_size));
        file_.read(reinterpret_cast<char*>(frame.side_info_raw.data()), side_info_size);

        BitstreamReader side_reader(frame.side_info_raw);
        SideInfo& si = frame.side_info;
        const int num_channels = (header->channel_mode == ChannelMode::Mono) ? 1 : 2;
        const int num_granules = (header->version == MpegVersion::MPEG1) ? 2 : 1;

        si.main_data_begin = static_cast<uint16_t>(
            side_reader.read_bits((header->version == MpegVersion::MPEG1) ? 9 : 8));
        if (header->channel_mode == ChannelMode::Mono) {
            si.private_bits = static_cast<uint8_t>(
                side_reader.read_bits((header->version == MpegVersion::MPEG1) ? 5 : 1));
        } else {
            si.private_bits = static_cast<uint8_t>(
                side_reader.read_bits((header->version == MpegVersion::MPEG1) ? 3 : 2));
        }

        if (header->version == MpegVersion::MPEG1) {
            for (int ch = 0; ch < num_channels; ++ch) {
                for (int scfsi_band = 0; scfsi_band < 4; ++scfsi_band) {
                    si.scfsi[ch][scfsi_band] = static_cast<bool>(side_reader.read_bits(1));
                }
            }
        }

        for (int gr = 0; gr < num_granules; ++gr) {
            for (int ch = 0; ch < num_channels; ++ch) {
                auto& gi = si.gr[gr][ch];
                gi.part2_3_length    = static_cast<uint16_t>(side_reader.read_bits(12));
                gi.big_values        = static_cast<uint16_t>(side_reader.read_bits(9));
                gi.global_gain       = static_cast<uint8_t>(side_reader.read_bits(8));
                gi.scalefac_compress = static_cast<uint16_t>(
                    side_reader.read_bits((header->version == MpegVersion::MPEG1) ? 4 : 9));
                gi.window_switching_flag = static_cast<bool>(side_reader.read_bits(1));
                if (gi.window_switching_flag) {
                    gi.block_type       = static_cast<uint8_t>(side_reader.read_bits(2));
                    gi.mixed_block_flag = static_cast<bool>(side_reader.read_bits(1));
                    gi.table_select[0]  = static_cast<uint8_t>(side_reader.read_bits(5));
                    gi.table_select[1]  = static_cast<uint8_t>(side_reader.read_bits(5));
                    gi.table_select[2]  = 0; // not used
                    for (uint8_t& gain : gi.subblock_gain) {
                        gain = static_cast<uint8_t>(side_reader.read_bits(3));
                    }
                    // preflag, scalefac_scale, count1table_select: same order as wsf=0 (ISO 11172-3)
                    gi.preflag           = (header->version == MpegVersion::MPEG1)
                                           ? static_cast<bool>(side_reader.read_bits(1)) : false;
                    gi.scalefac_scale    = static_cast<bool>(side_reader.read_bits(1));
                    gi.count1table_select = static_cast<bool>(side_reader.read_bits(1));
                    if (gi.block_type == 2 && !gi.mixed_block_flag) {
                        gi.region0_count = 8;
                    } else {
                        gi.region0_count = 7;
                    }
                    gi.region1_count = static_cast<uint8_t>(20 - gi.region0_count);
                } else {
                    for (uint8_t& sel : gi.table_select) {
                        sel = static_cast<uint8_t>(side_reader.read_bits(5));
                    }
                    gi.region0_count  = static_cast<uint8_t>(side_reader.read_bits(4));
                    gi.region1_count  = static_cast<uint8_t>(side_reader.read_bits(3));
                    gi.block_type     = 0;
                    gi.mixed_block_flag = false;
                    gi.preflag        = (header->version == MpegVersion::MPEG1)
                                        ? static_cast<bool>(side_reader.read_bits(1)) : false;
                    gi.scalefac_scale = static_cast<bool>(side_reader.read_bits(1));
                    gi.count1table_select = static_cast<bool>(side_reader.read_bits(1));
                }
            }
        }

        const int crc_size      = header->has_crc ? 2 : 0;
        const int main_data_size = static_cast<int>(header->bitrate.frame_size) - 4 - crc_size - side_info_size;

        frame.main_data_raw.resize(static_cast<size_t>(main_data_size));
        file_.read(reinterpret_cast<char*>(frame.main_data_raw.data()), main_data_size);

        frame.raw_bytes.reserve(static_cast<size_t>(4 + crc_size + side_info_size + main_data_size));
        frame.raw_bytes.insert(frame.raw_bytes.end(), buf, buf + 4);
        if (header->has_crc) {
            frame.raw_bytes.push_back(crc_bytes[0]);
            frame.raw_bytes.push_back(crc_bytes[1]);
        }
        frame.raw_bytes.insert(frame.raw_bytes.end(), frame.side_info_raw.begin(), frame.side_info_raw.end());
        frame.raw_bytes.insert(frame.raw_bytes.end(), frame.main_data_raw.begin(), frame.main_data_raw.end());

        return frame;
    }
    return std::nullopt;
}

} // namespace mp3packer
