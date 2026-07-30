#ifndef MP3PACKERCPP_MP3_READER_HPP
#define MP3PACKERCPP_MP3_READER_HPP

#include "types.hpp"
#include <vector>
#include <optional>

namespace mp3packer {

/**
 * @brief Handles reading and parsing of MP3 audio files.
 *
 * It is responsible for bypassing ID3 tags, finding frame headers,
 * and parsing the side info to assemble complete Mp3Frame structs.
 *
 * The file is read once into memory and parsed from there: a frame needs three or
 * four reads of a few bytes each, and going to the stream for them cost more than
 * the parsing did.
 */
class Mp3Reader {
public:
    /**
     * @brief Constructs an Mp3Reader to process the specified file.
     * @param filename Path to the input MP3 file.
     * @throws std::runtime_error If the file cannot be opened.
     */
    explicit Mp3Reader(const std::string& filename);
    ~Mp3Reader();

    /**
     * @brief Reads the next valid MPEG audio frame from the stream.
     * @return The parsed Mp3Frame, or std::nullopt if the end of the stream is reached.
     */
    std::optional<Mp3Frame> read_next_frame();

    /**
     * @brief Retrieves any junk data found before the first MP3 frame (e.g. ID3v2 tag).
     * @return Constant reference to the prefix junk data.
     */
    [[nodiscard]] const std::vector<uint8_t>& get_start_junk() const { return start_junk_; }

    /**
     * @brief Retrieves any junk data found after the last MP3 frame (e.g. ID3v1 tag).
     * @return Constant reference to the suffix junk data.
     */
    [[nodiscard]] const std::vector<uint8_t>& get_end_junk() const { return end_junk_; }

private:
    std::vector<uint8_t> data_;           ///< Whole file contents
    size_t pos_ = 0;                      ///< Read cursor into data_
    bool exhausted_ = false;              ///< Set once a read has run off the end
    std::vector<uint8_t> start_junk_;     ///< Buffer storing ID3v2 or pre-audio data
    std::vector<uint8_t> end_junk_;       ///< Buffer storing ID3v1 or post-audio data

    /**
     * @brief Copies n bytes from the cursor, zero-filling anything past the end.
     *
     * Mirrors what a short std::ifstream::read left behind: the bytes that were
     * there, the rest of a zero-initialised buffer untouched, and every later read
     * failing too.
     * @return False if the data ran out.
     */
    bool take(uint8_t* dst, size_t n);
    
    /**
     * @brief Parses a 32-bit header into an Mp3Header struct.
     * @param header_bits The 32-bit big-endian header value.
     * @return Parsed Mp3Header, or std::nullopt if the header is invalid.
     */
    static std::optional<Mp3Header> parse_header(uint32_t header_bits);

    /**
     * @brief Calculates the exact frame size in bytes based on the header.
     * @param header The parsed header for the frame.
     * @return The frame size in bytes.
     */
    static uint16_t calculate_frame_size(const Mp3Header& header);

    /**
     * @brief Identifies and skips over an ID3v2 tag if present at the current position.
     */
    void skip_id3v2_tag();
};

} // namespace mp3packer

#endif // MP3PACKERCPP_MP3_READER_HPP
