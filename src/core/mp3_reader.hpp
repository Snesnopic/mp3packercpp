#ifndef MP3PACKERCPP_MP3_READER_HPP
#define MP3PACKERCPP_MP3_READER_HPP

#include "types.hpp"
#include <fstream>
#include <vector>
#include <optional>

namespace mp3packer {

class Mp3Reader {
public:
    explicit Mp3Reader(const std::string& filename);
    ~Mp3Reader();

    std::optional<Mp3Frame> read_next_frame();
    [[nodiscard]] const std::vector<uint8_t>& get_start_junk() const { return start_junk_; }
    [[nodiscard]] const std::vector<uint8_t>& get_end_junk() const { return end_junk_; }

private:
    std::ifstream file_;
    size_t file_size_;
    std::vector<uint8_t> start_junk_;
    std::vector<uint8_t> end_junk_;
    
    static std::optional<Mp3Header> parse_header(uint32_t header_bits);
    static int calculate_frame_size(const Mp3Header& header);
    void skip_id3v2_tag();
};

} // namespace mp3packer

#endif // MP3PACKERCPP_MP3_READER_HPP
