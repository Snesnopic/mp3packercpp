#include "mp3_reader.hpp"
#include <iostream>

#include "bitstream.hpp"

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
        uint32_t size = (static_cast<uint32_t>(header[6] & 0x7F) << 21) |
                        (static_cast<uint32_t>(header[7] & 0x7F) << 14) |
                        (static_cast<uint32_t>(header[8] & 0x7F) << 7)  |
                        static_cast<uint32_t>(header[9] & 0x7F);

        uint32_t total_tag_size = 10 + size;
        if (header[5] & 0x10) { // Footer present flag
            total_tag_size += 10;
        }

        std::cout << "Found ID3v2 tag of size " << total_tag_size << " bytes. Storing in start_junk..." << std::endl;
        file_.seekg(0, std::ios::beg);
        start_junk_.resize(total_tag_size);
        file_.read(reinterpret_cast<char*>(start_junk_.data()), total_tag_size);
    } else {
        file_.seekg(0, std::ios::beg);
    }
}

std::optional<Mp3Header> Mp3Reader::parse_header(uint32_t header_bits) {
    if ((header_bits & 0xFFE00000) != 0xFFE00000) return std::nullopt;

    Mp3Header header{};
    int id = (header_bits >> 19) & 3;
    switch (id) {
        case 0: header.version = MpegVersion::MPEG25; break;
        case 2: header.version = MpegVersion::MPEG2; break;
        case 3: header.version = MpegVersion::MPEG1; break;
        default: return std::nullopt;
    }

    int layer = (header_bits >> 17) & 3;
    if (layer != 1) return std::nullopt; // Only Layer III

    header.has_crc = !((header_bits >> 16) & 1);

    int bitrate_index = (header_bits >> 12) & 0xF;
    if (bitrate_index == 0 || bitrate_index == 15) return std::nullopt;

    int samplerate_index = (header_bits >> 10) & 3;
    if (samplerate_index == 3) return std::nullopt;

    static constexpr int samplerates[3][3] = {
        {44100, 48000, 32000}, // MPEG 1
        {22050, 24000, 16000}, // MPEG 2
        {11025, 12000, 8000}   // MPEG 2.5
    };
    header.samplerate = samplerates[static_cast<int>(header.version)][samplerate_index];

    header.padding = (header_bits >> 9) & 1;
    header.private_bit = (header_bits >> 8) & 1;

    int mode = (header_bits >> 6) & 3;
    header.channel_mode = static_cast<ChannelMode>(mode);
    header.ms_stereo = (mode == 1) && ((header_bits >> 5) & 1);
    header.is_stereo = (mode == 1) && ((header_bits >> 4) & 1);

    header.copyright = (header_bits >> 3) & 1;
    header.original = (header_bits >> 2) & 1;
    header.emphasis = static_cast<Emphasis>(header_bits & 3);

    // Bitrate calculation
    static const int bitrates[3][16] = {
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0}, // MPEG 1
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0},    // MPEG 2
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}     // MPEG 2.5
    };
    header.bitrate.bitrate_kbps = bitrates[static_cast<int>(header.version)][bitrate_index];
    header.bitrate.padding = header.padding;
    header.bitrate.index = bitrate_index;
    
    header.bitrate.frame_size = calculate_frame_size(header);
    
    int side_info_size = (header.version == MpegVersion::MPEG1) ? 
                         (header.channel_mode == ChannelMode::Mono ? 17 : 32) :
                         (header.channel_mode == ChannelMode::Mono ? 9 : 17);
    header.bitrate.data_size = header.bitrate.frame_size - 4 - side_info_size - (header.has_crc ? 2 : 0);

    return header;
}

int Mp3Reader::calculate_frame_size(const Mp3Header& header) {
    if (header.version == MpegVersion::MPEG1) {
        return (144 * header.bitrate.bitrate_kbps * 1000 / header.samplerate) + (header.padding ? 1 : 0);
    } else {
        return (72 * header.bitrate.bitrate_kbps * 1000 / header.samplerate) + (header.padding ? 1 : 0);
    }
}

std::optional<Mp3Frame> Mp3Reader::read_next_frame() {
    uint8_t buf[4];
    while (file_.read(reinterpret_cast<char*>(buf), 4)) {
        uint32_t header_bits = (static_cast<uint32_t>(buf[0]) << 24) |
                               (static_cast<uint32_t>(buf[1]) << 16) |
                               (static_cast<uint32_t>(buf[2]) << 8)  |
                               static_cast<uint32_t>(buf[3]);
        auto header = parse_header(header_bits);
        if (header) {
            std::cout << "Successfully parsed valid MP3 header: " << std::hex << header_bits << std::dec 
                      << " at stream pos " << (static_cast<size_t>(file_.tellg()) - 4) << std::endl;
            Mp3Frame frame;
            header->bitrate.frame_size = calculate_frame_size(*header);
            frame.header = *header;

            int side_info_size = (header->version == MpegVersion::MPEG1) ? 
                                 (header->channel_mode == ChannelMode::Mono ? 17 : 32) :
                                 (header->channel_mode == ChannelMode::Mono ? 9 : 17);
            
            if (header->has_crc) {
                file_.ignore(2); // Skip CRC for now
            }

            frame.side_info_raw.resize(static_cast<size_t>(side_info_size));
            file_.read(reinterpret_cast<char*>(frame.side_info_raw.data()), side_info_size);

            // Parse Side Info
            BitstreamReader side_reader(frame.side_info_raw);
            SideInfo& si = frame.side_info;
            int num_channels = (header->channel_mode == ChannelMode::Mono) ? 1 : 2;
            int num_granules = (header->version == MpegVersion::MPEG1) ? 2 : 1;

            si.main_data_begin = static_cast<int>(side_reader.read_bits( (header->version == MpegVersion::MPEG1) ? 9 : 8 ));
            if (header->channel_mode == ChannelMode::Mono) {
                si.private_bits = static_cast<int>(side_reader.read_bits( (header->version == MpegVersion::MPEG1) ? 5 : 1 ));
            } else {
                si.private_bits = static_cast<int>(side_reader.read_bits( (header->version == MpegVersion::MPEG1) ? 3 : 2 ));
            }

            if (header->version == MpegVersion::MPEG1) {
                for (int ch = 0; ch < num_channels; ++ch) {
                    for (int scfsi_band = 0; scfsi_band < 4; ++scfsi_band) {
                        si.scfsi[ch][scfsi_band] = static_cast<uint8_t>(side_reader.read_bits(1));
                    }
                }
            }

            for (int gr = 0; gr < num_granules; ++gr) {
                for (int ch = 0; ch < num_channels; ++ch) {
                    auto& gi = si.gr[gr][ch];
                    gi.part2_3_length = static_cast<int>(side_reader.read_bits(12));
                    gi.big_values = static_cast<int>(side_reader.read_bits(9));
                    gi.global_gain = static_cast<int>(side_reader.read_bits(8));
                    gi.scalefac_compress = static_cast<int>(side_reader.read_bits( (header->version == MpegVersion::MPEG1) ? 4 : 9 ));
                    gi.window_switching_flag = static_cast<int>(side_reader.read_bits(1));
                    if (gi.window_switching_flag) {
                        gi.block_type = static_cast<int>(side_reader.read_bits(2));
                        gi.mixed_block_flag = static_cast<int>(side_reader.read_bits(1));
                        for (int i = 0; i < 2; ++i) gi.table_select[i] = static_cast<int>(side_reader.read_bits(5));
                        gi.table_select[2] = 0; // Not used
                        for (int i = 0; i < 3; ++i) gi.subblock_gain[i] = static_cast<int>(side_reader.read_bits(3));
                        
                        if (gi.block_type == 2 && gi.mixed_block_flag == 0) gi.region0_count = 8;
                        else gi.region0_count = 7;
                        gi.region1_count = 20 - gi.region0_count;
                    } else {
                        for (int i = 0; i < 3; ++i) gi.table_select[i] = static_cast<int>(side_reader.read_bits(5));
                        gi.region0_count = static_cast<int>(side_reader.read_bits(4));
                        gi.region1_count = static_cast<int>(side_reader.read_bits(3));
                        gi.block_type = 0;
                        gi.mixed_block_flag = 0;
                    }
                    gi.preflag = static_cast<int>((header->version == MpegVersion::MPEG1) ? side_reader.read_bits(1) : 0);
                    gi.scalefac_scale = static_cast<int>(side_reader.read_bits(1));
                    gi.count1table_select = static_cast<int>(side_reader.read_bits(1));
                }
            }

            int crc_size = header->has_crc ? 2 : 0;
            int header_size = 4;
            int main_data_size = header->bitrate.frame_size - header_size - crc_size - side_info_size;
            
            frame.main_data_raw.resize(static_cast<size_t>(main_data_size));
            file_.read(reinterpret_cast<char*>(frame.main_data_raw.data()), main_data_size);
            
            // Removed has_xing logic from here, packer.cpp does it!

            return frame;
        } else {
            // Invalid header found! Assume it's the start of end junk (like ID3v1 tag)
            file_.seekg(-4, std::ios::cur);
            size_t current_pos = static_cast<size_t>(file_.tellg());
            if (current_pos < file_size_) {
                size_t remaining = file_size_ - current_pos;
                end_junk_.resize(remaining);
                file_.read(reinterpret_cast<char*>(end_junk_.data()), static_cast<std::streamsize>(remaining));
                std::cout << "Found " << remaining << " bytes of junk at end of stream (e.g. ID3v1). Stored in end_junk." << std::endl;
            }
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace mp3packer
