#ifndef MP3PACKERCPP_PACKER_HPP
#define MP3PACKERCPP_PACKER_HPP

#include <vector>
#include <string>

namespace mp3packer {

class Packer {
public:
    Packer();
    bool recompress_huffman = false;
    
    void process(const std::string& input_file, const std::string& output_file) const;
};

} // namespace mp3packer

#endif // MP3PACKERCPP_PACKER_HPP
