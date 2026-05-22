#ifndef MP3PACKERCPP_TYPES_HPP
#define MP3PACKERCPP_TYPES_HPP

#include <vector>

namespace mp3packer {

enum class MpegVersion { MPEG1, MPEG2, MPEG25 };
enum class ChannelMode { Stereo, JointStereo, DualChannel, Mono };
enum class Emphasis { None, _50_15, Reserved, CCITT };

struct BitrateInfo {
    int bitrate_kbps;
    int frame_size;
    int data_size;
    bool padding;
    int index;
};

struct Mp3Header {
    MpegVersion version;
    bool has_crc;
    BitrateInfo bitrate;
    int samplerate;
    bool padding;
    bool private_bit;
    ChannelMode channel_mode;
    bool ms_stereo;
    bool is_stereo;
    bool copyright;
    bool original;
    Emphasis emphasis;
};

struct GrChInfo {
    int part2_3_length = 0;
    int big_values = 0;
    int global_gain = 0;
    int scalefac_compress = 0;
    int window_switching_flag = 0;
    int block_type = 0;
    int mixed_block_flag = 0;
    int table_select[3] = {0, 0, 0};
    int subblock_gain[3] = {0, 0, 0};
    int region0_count = 0;
    int region1_count = 0;
    int preflag = 0;
    int scalefac_scale = 0;
    int count1table_select = 0;
    std::vector<int> scalefactors;
};

struct SideInfo {
    int main_data_begin = 0;
    int private_bits = 0;
    uint8_t scfsi[2][4] = {{0, 0, 0, 0}, {0, 0, 0, 0}}; // [channel][band]
    GrChInfo gr[2][2];   // [granule][channel]
};

struct Mp3Frame {
    Mp3Header header;
    SideInfo side_info;
    std::vector<uint8_t> side_info_raw;
    std::vector<uint8_t> main_data_raw;
};

} // namespace mp3packer

#endif // MP3PACKERCPP_TYPES_HPP
