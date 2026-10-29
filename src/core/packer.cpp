#include "include/packer.hpp"
#include "include/huffman.hpp"
#include "include/logger.hpp"
#include "include/mp3_reader.hpp"
#include "include/huffman.hpp"
#include "include/bitstream.hpp"
#include <array>
#include <iostream>
#include <algorithm>
#include <vector>
#include <fstream>
#include <stdexcept>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <random>
#include <thread>

namespace mp3packer {

// MPEG-2/2.5 LSF scalefactor band group counts, indexed by [block category][slen
// set][group] (ISO 13818-3 table B.1). Block category is 0 for long blocks, 1 for
// pure short blocks and 2 for mixed blocks.
static constexpr int lsf_band_counts[3][6][4] = {
    {{6, 5, 5, 5}, {6, 5, 7, 3}, {11, 10, 0, 0}, {7, 7, 7, 0}, {6, 6, 6, 3}, {8, 8, 5, 0}},
    {{9, 9, 9, 9}, {9, 9, 12, 6}, {18, 18, 0, 0}, {12, 12, 12, 0}, {12, 9, 9, 6}, {15, 12, 9, 0}},
    {{6, 9, 9, 9}, {6, 9, 12, 6}, {15, 18, 0, 0}, {6, 15, 12, 0}, {6, 12, 9, 6}, {6, 15, 9, 0}},
};

// The LSF scalefac_compress field is a compound index selecting both a group of
// band counts and the bit length used within each group (ISO 13818-3 2.4.3.2).
// Each entry packs four 3-bit lengths in bits 0..11 and the band-count set in
// bits 12..14.
struct LsfSlenTables {
    std::array<uint32_t, 512> normal{};
    std::array<uint32_t, 256> intensity{};
};

static const LsfSlenTables& lsf_slen_tables() {
    static const LsfSlenTables tables = [] {
        LsfSlenTables t;
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                for (int k = 0; k < 4; ++k)
                    for (int l = 0; l < 4; ++l)
                        t.normal[static_cast<size_t>(l + k * 4 + j * 16 + i * 80)] =
                            static_cast<uint32_t>(i | j << 3 | k << 6 | l << 9);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 5; ++j)
                for (int k = 0; k < 4; ++k)
                    t.normal[static_cast<size_t>(k + j * 4 + i * 20 + 400)] =
                        static_cast<uint32_t>(i | j << 3 | k << 6 | 1 << 12);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 6; ++j)
                for (int k = 0; k < 6; ++k)
                    t.intensity[static_cast<size_t>(k + j * 6 + i * 36)] =
                        static_cast<uint32_t>(i | j << 3 | k << 6 | 2 << 12);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    t.intensity[static_cast<size_t>(k + j * 4 + i * 16 + 180)] =
                        static_cast<uint32_t>(i | j << 3 | k << 6 | 3 << 12);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 3; ++j) {
                t.intensity[static_cast<size_t>(j + i * 3 + 244)] = static_cast<uint32_t>(i | j << 3 | 4 << 12);
                t.normal[static_cast<size_t>(j + i * 3 + 500)] = static_cast<uint32_t>(i | j << 3 | 1 << 12 | 5 << 15);
            }
        return t;
    }();
    return tables;
}

// part2_length for an LSF granule: how many bits of scalefactors precede the
// Huffman-coded spectrum. Never decodes the values themselves — like MPEG1 they
// are copied through bit for bit.
static size_t lsf_scalefactor_bits(const GrChInfo& g, const bool intensity_stereo_ch1) {
    const auto& tables = lsf_slen_tables();
    uint32_t slen = intensity_stereo_ch1
        ? tables.intensity[static_cast<size_t>(g.scalefac_compress >> 1) & 0xFF]
        : tables.normal[static_cast<size_t>(g.scalefac_compress) & 0x1FF];

    int category = 0;
    if (g.window_switching_flag && g.block_type == 2) {
        category = g.mixed_block_flag ? 2 : 1;
    }
    const int* counts = lsf_band_counts[category][(slen >> 12) & 7];

    size_t bits = 0;
    for (int i = 0; i < 4; ++i) {
        bits += static_cast<size_t>(counts[i]) * (slen & 7);
        slen >>= 3;
    }
    return bits;
}

Packer::Packer() = default;

// Takes the original raw side_info bytes and sets only the main_data_begin field.
static std::vector<uint8_t> patch_mdb(const uint8_t* raw, const size_t size, const MpegVersion version, const uint16_t new_mdb) {
    std::vector<uint8_t> out(raw, raw + size);
    if (version == MpegVersion::MPEG1) {
        // main_data_begin is 9 bits (bits 0-8 MSB first)
        out[0] = static_cast<uint8_t>((new_mdb >> 1) & 0xFF);
        out[1] = static_cast<uint8_t>((out[1] & 0x7F) | ((new_mdb & 1) << 7));
    } else {
        // main_data_begin is 8 bits
        out[0] = static_cast<uint8_t>(new_mdb & 0xFF);
    }
    return out;
}

static std::vector<uint8_t> serialize_side_info(const Mp3Header& h, const SideInfo& si) {
    BitstreamWriter writer;
    const int n_ch = (h.channel_mode == ChannelMode::Mono) ? 1 : 2;
    const int n_gr = (h.version == MpegVersion::MPEG1) ? 2 : 1;

    writer.write_bits(si.main_data_begin, h.version == MpegVersion::MPEG1 ? 9 : 8);
    // private_bits is narrower for LSF, and must mirror Mp3Reader exactly: writing
    // MPEG1's widths for an MPEG2/2.5 frame overruns the block by 4 bits (mono) or
    // 1 (stereo), which the trailing byte-align then rounds up into a whole extra
    // byte — a frame one byte longer than its own header declares.
    const int private_bit_count = (h.version == MpegVersion::MPEG1)
                                  ? (h.channel_mode == ChannelMode::Mono ? 5 : 3)
                                  : (h.channel_mode == ChannelMode::Mono ? 1 : 2);
    writer.write_bits(si.private_bits, private_bit_count);
    
    if (h.version == MpegVersion::MPEG1) {
        for (int ch = 0; ch < n_ch; ++ch)
            for (int b = 0; b < 4; ++b)
                writer.write_bits(si.scfsi[ch][b], 1);
    }

    for (int gr = 0; gr < n_gr; ++gr) {
        for (int ch = 0; ch < n_ch; ++ch) {
            const GrChInfo& g = si.gr[gr][ch];
            writer.write_bits(g.part2_3_length, 12);
            writer.write_bits(g.big_values, 9);
            writer.write_bits(g.global_gain, 8);
            writer.write_bits(g.scalefac_compress, h.version == MpegVersion::MPEG1 ? 4 : 9);
            writer.write_bits(g.window_switching_flag, 1);
            if (g.window_switching_flag) {
                writer.write_bits(g.block_type, 2);
                writer.write_bits(g.mixed_block_flag, 1);
                writer.write_bits(g.table_select[0], 5);
                writer.write_bits(g.table_select[1], 5);
                for (const uint8_t i : g.subblock_gain) {
                    writer.write_bits(i, 3);
                }
            } else {
                writer.write_bits(g.table_select[0], 5);
                writer.write_bits(g.table_select[1], 5);
                writer.write_bits(g.table_select[2], 5);
                writer.write_bits(g.region0_count, 4);
                writer.write_bits(g.region1_count, 3);
            }
            // preflag, scalefac_scale, count1table_select follow for both window types
            if (h.version == MpegVersion::MPEG1) writer.write_bits(g.preflag, 1);
            writer.write_bits(g.scalefac_scale, 1);
            writer.write_bits(g.count1table_select, 1);
        }
    }
    while (writer.tell_bit() % 8 != 0) writer.write_bits(0, 1);
    return writer.data();
}

static uint32_t get_samplerate_index(const MpegVersion v, const uint32_t sr) {
    if (v == MpegVersion::MPEG1) {
        if (sr == 44100) return 0;
        if (sr == 48000) return 1;
        if (sr == 32000) return 2;
    } else if (v == MpegVersion::MPEG2) {
        if (sr == 22050) return 0;
        if (sr == 24000) return 1;
        if (sr == 16000) return 2;
    } else {
        if (sr == 11025) return 0;
        if (sr == 12000) return 1;
        if (sr == 8000)  return 2;
    }
    return 0;
}

static bool is_xing_frame(const Mp3Frame& frame, const std::vector<uint8_t>& file) {
    std::string s;
    s.append(reinterpret_cast<const char*>(frame.side_info_raw), frame.side_info_size);
    s.append(reinterpret_cast<const char*>(file.data() + frame.main_data_offset), frame.main_data_avail);
    if (s.size() < 100) return false;
    s.resize(100);
    return (s.find("Xing") != std::string::npos || s.find("Info") != std::string::npos);
}

struct ChosenBitrate {
    bool padding;
    int data_size;
    int index;
};

// The LAME extension carries its own two checksums, and they use a different
// CRC-16 from the frame one: reflected polynomial, seed 0, stored big-endian.
// Verified against files written by lame and by ffmpeg's libmp3lame.
static uint16_t crc16_lame(const uint8_t* data, const size_t size) {
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0xA001) : static_cast<uint16_t>(crc >> 1);
        }
    }
    return crc;
}

// Whether a 36-byte LAME extension starts at p. It opens with a vendor string, and
// the vendor is not always lame: ffmpeg writes "Lavc"/"Lavf" into the same block, so
// matching on "LAME" misses every file ffmpeg encoded. Presence is judged on the
// block being there and starting with printable text — a tag with no extension has
// zero padding at that offset instead.
static bool lame_extension_at(const uint8_t* p, const size_t available) {
    if (available < 36) return false;
    for (int k = 0; k < 4; ++k) {
        if (p[k] < 0x20 || p[k] > 0x7E) return false;
    }
    return true;
}

// The payload a frame holds at a bitrate index it already carries, rather than at
// the smallest index that would fit. Used to keep the xing frame the size it
// arrived at instead of shrinking it around its tag.
static ChosenBitrate bitrate_at_index(const MpegVersion version, const uint32_t samplerate,
                                      const int side_info_size, const int index,
                                      const bool padding, const int crc_size) {
    static const int mpeg1_bitrates[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int mpeg2_bitrates[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const int* bitrates = (version == MpegVersion::MPEG1) ? mpeg1_bitrates : mpeg2_bitrates;
    const int mult = (version == MpegVersion::MPEG1) ? 144 : 72;
    const int frame_size = mult * bitrates[index] * 1000 / static_cast<int>(samplerate)
                           + (padding ? 1 : 0);
    return {padding, frame_size - 4 - crc_size - side_info_size, index};
}

// The side info size is passed in rather than derived from a file-level channel
// mode: mono and stereo frames can sit in the same stream — concatenating two
// files is enough — and sizing a stereo frame's payload as if its side info were
// a mono one emits a frame whose length disagrees with its own header, which
// desynchronises everything after it. Same lesson as bug #12, one field over.
static ChosenBitrate bytes_to_bitrate(const MpegVersion version, const uint32_t samplerate,
                                      const int side_info_size, const int bytes, const int crc_size) {
    static const int mpeg1_bitrates[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int mpeg2_bitrates[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    
    const int* bitrates = (version == MpegVersion::MPEG1) ? mpeg1_bitrates : mpeg2_bitrates;
    // A kept CRC costs two bytes of every frame's payload.
    const int overhead = 4 + crc_size + side_info_size;

    for (int index = 1; index <= 14; ++index) {
        const int br = bitrates[index];
        // unpadded frame size
        int unpadded_frame_size = 0;
        if (version == MpegVersion::MPEG1) {
            unpadded_frame_size = (144 * br * 1000 / static_cast<int>(samplerate));
        } else {
            unpadded_frame_size = (72 * br * 1000 / static_cast<int>(samplerate));
        }
        
        const int unpadded_data_size = unpadded_frame_size - overhead;
        if (unpadded_data_size >= bytes) {
            return {false, unpadded_data_size, index};
        }
        if (unpadded_data_size + 1 >= bytes) {
            return {true, unpadded_data_size + 1, index};
        }
    }
    
    // fallback to max bitrate if not found
    const int max_br = bitrates[14];
    const int max_frame_size = (version == MpegVersion::MPEG1) ?
                         (144 * max_br * 1000 / static_cast<int>(samplerate)) + 1 :
                         (72 * max_br * 1000 / static_cast<int>(samplerate)) + 1;
    return {true, max_frame_size - overhead, 14};
}

// Writes the finished file through a temporary in the same directory and renames it
// into place. The rename is the only step that touches the destination, so an
// interrupted or failed run cannot leave a half-written file behind — and repacking
// a file onto itself is safe, since the input was read into memory long before.
static void write_file_atomically(const std::string& path, const std::vector<uint8_t>& bytes) {
    namespace fs = std::filesystem;
    const fs::path target(path);

    // Same directory keeps the rename on one filesystem, which is what makes it
    // atomic; a temp in /tmp could land on another and degrade to a copy.
    std::random_device entropy;
    fs::path temp = target;
    temp += ".mp3packer-" + std::to_string(entropy()) + ".tmp";

    {
        std::ofstream out(temp, std::ios::binary);
        if (!out) {
            throw std::runtime_error("Could not open output file: " + path);
        }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (!out) {
            std::error_code ignored;
            fs::remove(temp, ignored);
            throw std::runtime_error("Could not write output file: " + path);
        }
    }

    std::error_code ec;
    fs::rename(temp, target, ec);
    if (ec) {
        std::error_code ignored;
        fs::remove(temp, ignored);
        throw std::runtime_error("Could not replace output file: " + path + ": " + ec.message());
    }
}

// The frame CRC covers the last two header bytes and the whole side info — not the
// audio — so rewriting the side info means recomputing it. CRC-16 with polynomial
// 0x8005, seeded to all ones, MSB first (ISO 11172-3 2.4.3.1).
static uint16_t frame_crc16(const uint8_t* header, const uint8_t* side_info, const size_t side_info_size) {
    uint32_t crc = 0xFFFF;
    const auto feed = [&crc](const uint8_t byte) {
        uint32_t value = static_cast<uint32_t>(byte) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            value <<= 1;
            crc <<= 1;
            if ((value ^ crc) & 0x10000) crc ^= 0x8005;
        }
    };
    feed(header[2]);
    feed(header[3]);
    for (size_t i = 0; i < side_info_size; ++i) feed(side_info[i]);
    return static_cast<uint16_t>(crc & 0xFFFF);
}

// The bitrate table for a version, as indices into the frame header's 4-bit field.
// Index 0 (free) and 15 (reserved) are not usable.
static const int* bitrate_table_for(const MpegVersion version) {
    static const int mpeg1_bitrates[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int mpeg2_bitrates[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    return (version == MpegVersion::MPEG1) ? mpeg1_bitrates : mpeg2_bitrates;
}

// One frame's size in a constant-bitrate stream. The exact length is fractional —
// 144 * 128000 / 44100 is 417.96 — so a running remainder decides which frames
// carry the extra padding byte, which is the cycle a CBR encoder follows and what
// makes the stream come out at exactly the bitrate it claims.
static int cbr_frame_size(const MpegVersion version, const uint32_t samplerate,
                          const int bitrate_kbps, int& remainder, bool& padded) {
    const int mult = (version == MpegVersion::MPEG1) ? 144 : 72;
    const long long numer = static_cast<long long>(mult) * bitrate_kbps * 1000;
    const int base = static_cast<int>(numer / samplerate);
    remainder += static_cast<int>(numer % samplerate);
    padded = false;
    if (remainder >= static_cast<int>(samplerate)) {
        remainder -= static_cast<int>(samplerate);
        padded = true;
    }
    return base + (padded ? 1 : 0);
}

void Packer::process(const std::string& input_file, const std::string& output_file,
                     int* out_bitrate_kbps) const {
    DEBUG_LOG("Reading input file " << input_file << "...");
    Mp3Reader reader(input_file);

    std::vector<Mp3Frame> all_frames;
    while (auto opt_frame = reader.read_next_frame()) {
        all_frames.push_back(std::move(*opt_frame)); // a copy here duplicated every frame's payload
    }

    if (all_frames.empty()) {
        throw std::runtime_error("No valid MP3 frames found!");
    }

    bool has_xing = false;
    // removed unused xing_frame here
    const std::vector<uint8_t>& file = reader.data();
    if (is_xing_frame(all_frames[0], file)) {
        has_xing = true;
        // do not erase it! we must keep it to preserve the lame tag!
        // all_frames.erase(all_frames.begin());
        DEBUG_LOG("Detected XING header frame at beginning. Preserving it.");
    }
    
    size_t N = all_frames.size();
    DEBUG_LOG("Optimizing " << N << " audio frames...");

    std::vector<std::vector<uint8_t>> optimized_main_data(N);
    // The new side info goes into the frame itself. A worker owns its frame, and a
    // frame it gives up on keeps what it arrived with, which is what the verbatim
    // path reads — so a parallel array of side info was a copy of something each
    // frame already had, 200 bytes of it per frame.
    // plain uint8_t, not vector<bool>: worker threads write distinct indices concurrently,
    // and vector<bool>'s packed-bit storage would make that a genuine data race (RMW on a
    // shared word) even though each thread only touches its own logical element.
    std::vector<uint8_t> frame_was_optimized(N, 0);
    
    std::vector<uint8_t> global_res;
    size_t total_main = 0;
    for (const auto& frame : all_frames) total_main += frame.main_data_size;
    global_res.reserve(total_main);
    std::vector<size_t> frame_main_starts(N);
    for (size_t i = 0; i < N; ++i) {
        frame_main_starts[i] = global_res.size();
        // One copy, out of the file and into the reservoir view. A payload the file
        // does not hold in full is zero-filled to its declared length.
        const auto& fr = all_frames[i];
        global_res.insert(global_res.end(), file.data() + fr.main_data_offset,
                          file.data() + fr.main_data_offset + fr.main_data_avail);
        global_res.resize(frame_main_starts[i] + fr.main_data_size, 0x00);
    }

    std::atomic<size_t> current_frame{0};
    unsigned int active_threads = num_threads > 0 ? num_threads : std::max(1u, std::thread::hardware_concurrency());

    auto worker = [&]() {
        // One pair of spectra per worker, reused for every granule: the coder fills
        // them and zeroes only the tail it did not write.
        Spectrum coeffs{}, vcoeffs{};
        while (true) {
            size_t i = current_frame.fetch_add(1);
            if (i >= N) break;

            auto& frame = all_frames[i];
            if (has_xing && i == 0) {
                // output frames are always written without CRC, so "Xing"/"Info" must be at
                // output mdr[0..3].
                //   No-CRC file: copy mdr verbatim (extended headers like "Lavf" at byte 120+
                //                contain gapless timing info that decoders rely on).
                //   CRC file:    "Xi"/"In" is in si[-2:], "ng"/"fo" at mdr[0].
                //                Prepend si[-2:] and copy standard Xing content (through LAME
                //                extension if present), trimming trailing zeros to save space.
                const uint8_t* mdr = file.data() + all_frames[i].main_data_offset;
                const size_t mdr_size = all_frames[i].main_data_avail;
                if (!all_frames[i].header.has_crc) {
                    optimized_main_data[i].assign(mdr, mdr + mdr_size);
                } else {
                    // CRC: recover "Xi"/"In" from the last 2 bytes of the original side_info
                    const uint8_t* sir = all_frames[i].side_info_raw;
                    const size_t sir_size = all_frames[i].side_info_size;
                    uint8_t b0 = (sir_size >= 2) ? sir[sir_size - 2] : 0x58;
                    uint8_t b1 = (sir_size >= 1) ? sir[sir_size - 1] : 0x69;
                    // mdr[0..1]="ng"/"fo", content (flags, counts...) at mdr[2..]
                    size_t keep = 2;
                    if (mdr_size >= 6) {
                        uint32_t fl = (static_cast<uint32_t>(mdr[2]) << 24) |
                                      (static_cast<uint32_t>(mdr[3]) << 16) |
                                      (static_cast<uint32_t>(mdr[4]) << 8) |
                                      static_cast<uint32_t>(mdr[5]);
                        keep += 4; // flags word
                        if (fl & 1) keep += 4;   // frame count
                        if (fl & 2) keep += 4;   // byte count
                        if (fl & 4) keep += 100; // TOC
                        if (fl & 8) keep += 4;   // quality
                        if (keep <= mdr_size && lame_extension_at(mdr + keep, mdr_size - keep))
                            keep += 36;
                    }
                    // Preserving means copying the tag whole: the fields we know how
                    // to size are not necessarily all of it, and an extension we do
                    // not recognise is exactly what trimming would silently drop.
                    keep = this->preserve_xing ? mdr_size : std::min(keep, mdr_size);
                    optimized_main_data[i] = {b0, b1};
                    optimized_main_data[i].insert(optimized_main_data[i].end(), mdr, mdr + keep);
                }
                frame.side_info = SideInfo{};
                continue;
            }
        int n_ch = (frame.header.channel_mode == ChannelMode::Mono) ? 1 : 2;
        int n_gr = (frame.header.version == MpegVersion::MPEG1) ? 2 : 1;

        BitstreamReader data_reader(global_res);
        size_t frame_main_start = frame_main_starts[i];
        size_t start_byte = (frame_main_start >= static_cast<size_t>(frame.side_info.main_data_begin)) ? (frame_main_start - static_cast<size_t>(frame.side_info.main_data_begin)) : 0;
        data_reader.seek_bit(start_byte * 8);

        BitstreamWriter writer;
        SideInfo side_copy = frame.side_info; // backup in case we fallback
        bool optimization_failed = false;
        
        try {
            if (!this->recompress_huffman) {
                optimization_failed = true; // fallback to raw copy of original data
            } else {
                for (int gr = 0; gr < n_gr; ++gr) {
                for (int ch = 0; ch < n_ch; ++ch) {
                    GrChInfo& g = frame.side_info.gr[gr][ch];
                    size_t gc_orig_start = data_reader.tell_bit();

                    // read scalefactors
                    int slen1 = 0, slen2 = 0;
                    const bool is_mpeg1 = frame.header.version == MpegVersion::MPEG1;
                    if (is_mpeg1) {
                        static constexpr int slen1_tab[] = {0, 0, 0, 0, 3, 1, 1, 1, 2, 2, 2, 3, 3, 3, 4, 4};
                        static constexpr int slen2_tab[] = {0, 1, 2, 3, 0, 1, 2, 3, 1, 2, 3, 1, 2, 3, 2, 3};
                        slen1 = slen1_tab[g.scalefac_compress];
                        slen2 = slen2_tab[g.scalefac_compress];
                    }

                    // Scalefactors are re-emitted exactly as they arrived, so only
                    // their total length matters. Copying the bits is what Go's
                    // reference implementation does; reading each field into a
                    // vector and writing it back cost an allocation per granule and
                    // some seventy calls through the bit machinery.
                    size_t sf_bits = 0;
                    if (!is_mpeg1) {
                        // LSF (MPEG-2/2.5): different band-group layout and slen
                        // decomposition (ISO 13818-3 Annex B).
                        sf_bits = lsf_scalefactor_bits(g, frame.header.is_stereo && ch == 1);
                    } else if (g.window_switching_flag && g.block_type == 2) {
                        sf_bits = g.mixed_block_flag
                                  ? static_cast<size_t>(17 * slen1 + 18 * slen2)
                                  : static_cast<size_t>(18 * slen1 + 18 * slen2);
                    } else {
                        // A band group whose scfsi bit is set in granule 1 reuses
                        // granule 0's values and occupies no bits at all.
                        if (gr == 0 || !frame.side_info.scfsi[ch][0]) sf_bits += static_cast<size_t>(6 * slen1);
                        if (gr == 0 || !frame.side_info.scfsi[ch][1]) sf_bits += static_cast<size_t>(5 * slen1);
                        if (gr == 0 || !frame.side_info.scfsi[ch][2]) sf_bits += static_cast<size_t>(5 * slen2);
                        if (gr == 0 || !frame.side_info.scfsi[ch][3]) sf_bits += static_cast<size_t>(5 * slen2);
                    }

                    size_t out_start = writer.tell_bit();
                    if (data_reader.tell_bit() + sf_bits <= data_reader.size_bits()) {
                        writer.copy_from(data_reader, sf_bits);
                    } else if (!is_mpeg1) {
                        // Rare edge (reservoir truncated mid-scalefactors); the MPEG1
                        // field-by-field path below does not apply to LSF's band
                        // layout, so fall back to a verbatim copy of the frame.
                        throw std::runtime_error("LSF scalefactor span exceeds reservoir data");
                    } else {
                        // The span reaches past the end of the reservoir data, where
                        // what a read returns depends on the width it was made at, so
                        // the fields are read and written one at a time as before.
                        int scfs[36] = {};
                        if (g.window_switching_flag && g.block_type == 2) {
                            if (g.mixed_block_flag) {
                                for (int k = 0; k < 17; ++k) scfs[k] = static_cast<int>(data_reader.read_bits(slen1));
                                for (int k = 17; k < 35; ++k) scfs[k] = static_cast<int>(data_reader.read_bits(slen2));
                                for (int k = 0; k < 17; ++k) writer.write_bits(static_cast<uint32_t>(scfs[k]), slen1);
                                for (int k = 17; k < 35; ++k) writer.write_bits(static_cast<uint32_t>(scfs[k]), slen2);
                            } else {
                                for (int k = 0; k < 18; ++k) scfs[k] = static_cast<int>(data_reader.read_bits(slen1));
                                for (int k = 18; k < 36; ++k) scfs[k] = static_cast<int>(data_reader.read_bits(slen2));
                                for (int k = 0; k < 18; ++k) writer.write_bits(static_cast<uint32_t>(scfs[k]), slen1);
                                for (int k = 18; k < 36; ++k) writer.write_bits(static_cast<uint32_t>(scfs[k]), slen2);
                            }
                        } else {
                            static constexpr int group_first[4] = {0, 6, 11, 16};
                            static constexpr int group_last[4] = {6, 11, 16, 21};
                            for (int grp = 0; grp < 4; ++grp) {
                                const int slen = grp < 2 ? slen1 : slen2;
                                if (gr != 0 && frame.side_info.scfsi[ch][grp]) continue;
                                for (int k = group_first[grp]; k < group_last[grp]; ++k) {
                                    scfs[k] = static_cast<int>(data_reader.read_bits(slen));
                                }
                                for (int k = group_first[grp]; k < group_last[grp]; ++k) {
                                    writer.write_bits(static_cast<uint32_t>(scfs[k]), slen);
                                }
                            }
                        }
                    }

                    // decode & optimize huffman
                    HuffmanConfig orig_cfg = {
                        g.region0_count,
                        g.region1_count,
                        g.big_values,
                        g.table_select[0],
                        g.table_select[1],
                        g.table_select[2],
                        g.count1table_select,
                        g.window_switching_flag,
                        g.block_type,
                        g.mixed_block_flag
                    };
                    int scalefac_bits = static_cast<int>(data_reader.tell_bit() - gc_orig_start);
                    int max_huff_bits = side_copy.gr[gr][ch].part2_3_length - scalefac_bits;
                    HuffmanOptimizer::decode_quantized_coefficients(coeffs, orig_cfg, data_reader, frame.header.samplerate, max_huff_bits);
                    auto best_cfg = HuffmanOptimizer::find_best_config(coeffs, orig_cfg, frame.header.samplerate);
                    
                    size_t scalefac_end_bit = writer.tell_bit();
                    HuffmanOptimizer::encode_quantized_coefficients(coeffs, best_cfg, writer, frame.header.samplerate);

                    // round-trip verification: re-decode the bytes we just wrote and compare
                    {
                        int huff_bits = static_cast<int>(writer.tell_bit() - scalefac_end_bit);
                        BitstreamReader vrdr(writer.data());
                        vrdr.seek_bit(scalefac_end_bit);
                        HuffmanOptimizer::decode_quantized_coefficients(vcoeffs, best_cfg, vrdr, frame.header.samplerate, huff_bits);
                        for (int ci = 0; ci < 576; ++ci) {
                            if (vcoeffs[ci] != coeffs[ci]) {
                                optimization_failed = true;
                                break;
                            }
                        }
                    }

                    g.big_values = best_cfg.big_values;
                    g.region0_count = best_cfg.region0_count;
                    g.region1_count = best_cfg.region1_count;
                    g.table_select[0] = best_cfg.table0;
                    g.table_select[1] = best_cfg.table1;
                    g.table_select[2] = best_cfg.table2;
                    g.count1table_select = best_cfg.count1_table_select;
                    
                    int orig_part2_3 = side_copy.gr[gr][ch].part2_3_length;
                    g.part2_3_length = static_cast<uint16_t>(writer.tell_bit() - out_start);

                    data_reader.seek_bit(gc_orig_start + orig_part2_3);
                }
            }
            }
        } catch (const std::exception&) {
            optimization_failed = true;
        }

        int total_orig_bits = 0;
        for (int gr = 0; gr < n_gr; ++gr) {
            for (int ch = 0; ch < n_ch; ++ch) {
                total_orig_bits += side_copy.gr[gr][ch].part2_3_length;
            }
        }
        int total_orig_bytes = (total_orig_bits + 7) / 8;

        std::vector<uint8_t> new_main = writer.data();
        if (optimization_failed || new_main.size() >= static_cast<size_t>(total_orig_bytes)) {
            // fallback: no improvement or failure — preserve original side_info bits via patch_mdb.
            // The length comes from the side info's part2_3 fields, which on a
            // malformed file can claim more than the stream actually holds; what is
            // missing is zero-filled, the same answer the reservoir already gives a
            // frame whose payload the file truncates.
            const size_t want = static_cast<size_t>(total_orig_bytes);
            const size_t have = std::min(want, global_res.size() - start_byte);
            new_main.assign(global_res.begin() + static_cast<ptrdiff_t>(start_byte),
                            global_res.begin() + static_cast<ptrdiff_t>(start_byte + have));
            new_main.resize(want, 0x00);
            frame.side_info = side_copy;
        } else {
            // strictly smaller: use compressed data and re-serialize updated side_info
            frame_was_optimized[i] = true;
        }

        optimized_main_data[i] = new_main;
        }
    };

    // Spawning and joining threads costs more than the work when there is no
    // Huffman search to do: without -z a frame's worth of work is a slice copy, and
    // sixteen threads were being created to share it.
    if (active_threads == 1 || !this->recompress_huffman) {
        worker();
    } else {
        std::vector<std::thread> threads;
        threads.reserve(active_threads);
        for (unsigned int t = 0; t < active_threads; ++t) {
            threads.emplace_back(worker);
        }
        for (auto& thread : threads) {
            thread.join();
        }
    }
    
    DEBUG_LOG("Huffman optimization completed. Running reservoir constraint solver...");
    
    // backward pass (constraint solver for carry-over reservoir)
    MpegVersion version = all_frames[0].header.version;
    uint32_t samplerate = all_frames[0].header.samplerate;
    ChannelMode channel_mode = all_frames[0].header.channel_mode;
    
    DEBUG_LOG("Reservoir carryover calculated. Writing output stream...");
    
    // Nothing is opened here: the output is assembled in memory and written at the
    // end through a temporary file, so an interrupted run leaves the destination
    // exactly as it was.

    // The file is assembled in memory and written once at the end. Writing it
    // frame by frame meant three stream writes per frame, and the Xing patch-up
    // then had to seek back through them.
    std::vector<uint8_t> out_bytes;
    out_bytes.reserve(total_main + N * 40 + reader.get_start_junk().size() + reader.get_end_junk().size());
    (void)total_main;
    const auto append = [&out_bytes](const uint8_t* p, const size_t n) {
        out_bytes.insert(out_bytes.end(), p, p + n);
    };

    // write start junk (id3v2)
    const auto& start_junk = reader.get_start_junk();
    if (!start_junk.empty()) {
        append(start_junk.data(), start_junk.size());
    }
    
    // track where the xing frame starts in the file
    const size_t xing_header_file_pos = has_xing ? out_bytes.size() : SIZE_MAX;
    
    // A frame keeps its CRC only if it arrived with one and the caller asked to
    // keep it. Like the stereo mode, this is read per frame rather than from
    // frame 0 — nothing guarantees an encoder protected every frame or none.
    // The xing frame is the exception: encoders place its "Xing"/"Info" tag at
    // header + side_info, skipping the crc field, so the tag straddles the end of
    // the side info on a protected frame. The payload we rebuild normalises that to
    // start at main_data[0], which only lands where readers look if no crc follows
    // the header. It carries no audio, so it simply never gets one.
    const auto side_info_of = [&](const size_t index) {
        return static_cast<int>(all_frames[index].side_info_size);
    };

    const auto crc_size_of = [&](const size_t index) {
        if (has_xing && index == 0) return 0;
        return (this->keep_crc && all_frames[index].header.has_crc) ? 2 : 0;
    };

    // backward pass constraint solver
    std::vector<int> required_carryover(N, 0);
    int current_req = 0;
    for (int i = static_cast<int>(N) - 1; i >= 0; --i) {
        int space_needed = static_cast<int>(optimized_main_data[i].size()) + current_req;
        
        // we allow the forward pass to use up to 320 kbps if needed, so the backward pass should assume max capacity.
        ChosenBitrate max_br = bytes_to_bitrate(version, samplerate, side_info_of(static_cast<size_t>(i)), 9999, crc_size_of(static_cast<size_t>(i)));
        int max_data_per_frame = max_br.data_size;
        
        current_req = std::max(0, space_needed - max_data_per_frame);
        required_carryover[i] = current_req;
    }
    // The reservoir is described rather than built. Deciding a frame's size and its
    // reservoir offset takes lengths only, so the first pass records the pieces —
    // each frame's data, and the run of zeros left at the end where the reservoir
    // cannot be read back — and the second pass reads them straight out of the
    // frames' own buffers. Concatenating them first was a second copy of the whole
    // audio and an allocation to hold it.
    size_t reservoir_bytes = 0;
    int current_reservoir = 0; // free space in past payloads

    const int max_reservoir_bytes = (version == MpegVersion::MPEG1) ? 511 : 255;

    // --- constant bitrate ---------------------------------------------------
    //
    // Walking the layout with every frame pinned to one bitrate answers whether
    // that bitrate is usable: a frame's payload has to come out of its own slot
    // plus whatever earlier frames banked, and the back-reference only reaches
    // 511 bytes (255 for LSF). Nothing here re-runs the search — it reads the
    // sizes the repack already produced — so the whole scan costs one pass per
    // candidate bitrate.
    const int* bitrates = bitrate_table_for(version);
    const auto cbr_fits = [&](const int bitrate_kbps) {
        int remainder = 0, reservoir = 0;
        for (size_t i = 0; i < N; ++i) {
            bool padded = false;
            const int data_size = cbr_frame_size(version, samplerate, bitrate_kbps, remainder, padded)
                                  - 4 - crc_size_of(i) - side_info_of(i);
            if (data_size < 0) return false;
            const int needed = static_cast<int>(optimized_main_data[i].size());
            if (has_xing && i == 0) {
                // The Xing frame is preserved rather than rebuilt, so it has to fit
                // as it stands; if it arrived larger than the audio needs, it is what
                // raises the floor rather than being left as one odd frame.
                if (needed > data_size) return false;
                reservoir = 0;
                continue;
            }
            const int projected = reservoir + data_size - needed;
            if (projected < 0) return false;
            reservoir = std::min(projected, max_reservoir_bytes);
        }
        return true;
    };

    int cbr_bitrate = 0;          // 0 means variable bitrate
    int cbr_index = 0;
    const bool want_cbr = this->constant_bitrate || this->min_bitrate_kbps > 0;
    if (want_cbr) {
        for (int index = 1; index <= 14; ++index) {
            // The floor only raises the starting point; a bitrate the frames do not
            // fit in is never usable, so feasibility is checked either way. Forcing
            // an under-sized bitrate would produce a structurally broken file.
            if (bitrates[index] < this->min_bitrate_kbps) continue;   // floor, rounded up
            if (!cbr_fits(bitrates[index])) continue;
            cbr_bitrate = bitrates[index];
            cbr_index = index;
            break;
        }
        if (cbr_bitrate == 0) {
            throw std::runtime_error("no constant bitrate can hold this file");
        }
    }
    if (out_bitrate_kbps) *out_bitrate_kbps = cbr_bitrate;
    if (this->report_bitrate_only) return;

    std::vector<ChosenBitrate> chosen_bitrates(N);
    int cbr_remainder = 0;

    // pass 1: build global_main_data and calculate main_data_begin
    for (size_t i = 0; i < N; ++i) {
        auto& side = all_frames[i].side_info;

        int max_reservoir = max_reservoir_bytes;

        ChosenBitrate bitrate_use{};
        if (cbr_bitrate != 0) {
            bool padded = false;
            const int frame_size = cbr_frame_size(version, samplerate, cbr_bitrate, cbr_remainder, padded);
            bitrate_use = {padded, frame_size - 4 - crc_size_of(i) - side_info_of(i), cbr_index};
        } else if (has_xing && i == 0 && this->preserve_xing) {
            // Keep the frame the length the encoder gave it. Dropping the crc frees
            // exactly the two bytes the recovered "Xi"/"In" takes back, so the tag
            // lands in the same payload it came out of, byte for byte.
            bitrate_use = bitrate_at_index(version, samplerate, side_info_of(0),
                                           all_frames[0].header.bitrate.index,
                                           all_frames[0].header.bitrate.padding, crc_size_of(i));
        } else if (has_xing && i == 0) {
            // use minimum bitrate that fits the trimmed Xing tag content
            int needed = static_cast<int>(optimized_main_data[0].size());
            bitrate_use = bytes_to_bitrate(version, samplerate, side_info_of(i), needed, crc_size_of(i));
        } else {
            int req_for_next = (i + 1 < N) ? required_carryover[i + 1] : 0;
            int bytes_needed = std::max(0, static_cast<int>(optimized_main_data[i].size()) + req_for_next - current_reservoir);
            bitrate_use = bytes_to_bitrate(version, samplerate, side_info_of(i), bytes_needed, crc_size_of(i));
        }

        int data_size = bitrate_use.data_size;

        // if this frame would push reservoir past 511, absorb excess as ancillary zeros
        // (guaranteed to fit: excess = proj - max <= data_size - main_size since current_reservoir <= max)
        int projected_reservoir = current_reservoir + data_size - static_cast<int>(optimized_main_data[i].size());
        if (projected_reservoir > max_reservoir) {
            int excess = projected_reservoir - max_reservoir;
            optimized_main_data[i].resize(optimized_main_data[i].size() + excess, 0x00);
            projected_reservoir = max_reservoir;
        }

        // for the Xing frame, pad to data_size before pushing so pass 2
        // doesn't read audio bytes to fill the remainder of the payload
        if (has_xing && i == 0 && static_cast<int>(optimized_main_data[0].size()) < data_size)
            optimized_main_data[0].resize(static_cast<size_t>(data_size), 0x00);

        side.main_data_begin = static_cast<uint16_t>(current_reservoir);
        reservoir_bytes += optimized_main_data[i].size();
        current_reservoir = projected_reservoir;
        chosen_bitrates[i] = bitrate_use;

        if (has_xing && i == 0)
            current_reservoir = 0;
    }
    
    // Whatever the last frame leaves unread is emitted as zeros.
    const size_t reservoir_tail_zeros = static_cast<size_t>(std::max(0, current_reservoir));
    reservoir_bytes += reservoir_tail_zeros;
    
    // pass 2: write physical file
    //
    // Every entry in all_frames is a frame the source actually declared, each
    // worth a fixed number of samples; omitting one shortens the decoded audio
    // by exactly that much regardless of where in the file it falls. A frame
    // whose own optimized_main_data is empty isn't redundant — genuinely quiet
    // material can compress a granule down to a reservoir back-reference with
    // nothing new of its own — so every frame is written, unconditionally.
    const size_t N_out = N;


    // A frame's slot is a window over that sequence and generally spans more than one
    // piece — that is what a bit reservoir is for — so the read side is a cursor.
    size_t cursor_frame = 0, cursor_offset = 0, cursor_read = 0;
    const auto emit_reservoir = [&](size_t want) {
        while (want > 0 && cursor_read < reservoir_bytes) {
            if (cursor_frame < N) {
                const auto& piece = optimized_main_data[cursor_frame];
                if (cursor_offset >= piece.size()) {
                    ++cursor_frame;
                    cursor_offset = 0;
                    continue;
                }
                const size_t take = std::min(want, piece.size() - cursor_offset);
                append(piece.data() + cursor_offset, take);
                cursor_offset += take;
                cursor_read += take;
                want -= take;
                continue;
            }
            // Past the last frame's data: the tail the reservoir never gives back.
            const size_t take = std::min(want, reservoir_bytes - cursor_read);
            out_bytes.resize(out_bytes.size() + take, 0x00);
            cursor_read += take;
            want -= take;
        }
        return want; // whatever the reservoir could not supply
    };

    // audio frame byte positions in the output file (index 0 = first audio frame, i.e. frame 1 if has_xing)
    std::vector<size_t> audio_frame_file_pos;

    for (size_t i = 0; i < N_out; ++i) {
        const auto& frame = all_frames[i];
        const auto& side = frame.side_info;

        if (has_xing && i > 0)
            audio_frame_file_pos.push_back(out_bytes.size());

        ChosenBitrate bitrate_use = chosen_bitrates[i];

        uint32_t h = 0xFFE00000;
        uint32_t version_bits = 2; // MPEG2
        if (version == MpegVersion::MPEG1) version_bits = 3;
        else if (version == MpegVersion::MPEG25) version_bits = 0;
        h |= (version_bits << 19);
        // bit 17 marks Layer III; bit 16 is the protection bit, which is *clear*
        // when a CRC follows the header.
        const int frame_crc_size = crc_size_of(i);
        h |= (1 << 17);
        if (frame_crc_size == 0) h |= (1 << 16);
        h |= (static_cast<uint32_t>(bitrate_use.index) << 12) | (get_samplerate_index(version, samplerate) << 10);
        // Mode and mode_extension come from this frame, not from frame 0: an encoder
        // may switch a stereo pair between plain L/R and joint stereo per frame, and
        // the main_data we are copying through is coded accordingly. Writing frame 0's
        // mode for the whole file silently mislabels every frame that differs — a
        // joint/MS frame emitted as plain stereo leaves the decoder with mid/side
        // values it never un-rotates, which destroys the audio while staying
        // structurally valid.
        h |= static_cast<uint32_t>(bitrate_use.padding ? 1 : 0) << 9
           | (static_cast<uint32_t>(frame.header.channel_mode) << 6);
        uint32_t mode_ext = 0;
        if (frame.header.channel_mode == ChannelMode::JointStereo) {
            mode_ext = (frame.header.ms_stereo ? 2 : 0) | (frame.header.is_stereo ? 1 : 0);
        }
        h |= mode_ext << 4;
        h |= static_cast<uint32_t>(frame.header.copyright ? 1 : 0) << 3;
        h |= static_cast<uint32_t>(frame.header.original ? 1 : 0) << 2;
        h |= static_cast<uint32_t>(frame.header.emphasis);

        uint8_t head[4];
        head[0] = static_cast<uint8_t>(h >> 24); head[1] = static_cast<uint8_t>(h >> 16); head[2] = static_cast<uint8_t>(h >> 8); head[3] = static_cast<uint8_t>(h);

        std::vector<uint8_t> new_side;
        if (has_xing && i == 0) {
            new_side.assign(frame.side_info_size, 0);
        } else if (frame_was_optimized[i]) {
            new_side = serialize_side_info(frame.header, side);
        } else {
            new_side = patch_mdb(frame.side_info_raw, frame.side_info_size, version, side.main_data_begin);
        }

        append(head, 4);
        if (frame_crc_size == 2) {
            // Computed over the header and side info we are about to emit, not the
            // ones that arrived: the side info has been rewritten.
            const uint16_t crc = frame_crc16(head, new_side.data(), new_side.size());
            const uint8_t crc_bytes[2] = {static_cast<uint8_t>(crc >> 8), static_cast<uint8_t>(crc & 0xFF)};
            append(crc_bytes, 2);
        }
        append(new_side.data(), new_side.size());

        const int data_size = bitrate_use.data_size;
        const size_t short_by = emit_reservoir(static_cast<size_t>(data_size));
        if (short_by > 0) {
            out_bytes.resize(out_bytes.size() + short_by, 0x00);
        }
    }
    
    // save position before end_junk to calculate total mp3 stream size
    size_t mp3_end_pos = out_bytes.size();
    
    // write end junk (id3v1)
    const auto& end_junk = reader.get_end_junk();
    if (!end_junk.empty()) {
        append(end_junk.data(), end_junk.size());
    }
    
    // patch xing header fields (bytes count + TOC) if present
    const auto patch = [&out_bytes](const size_t at, const uint8_t* p, const size_t n) {
        if (at + n <= out_bytes.size()) std::memcpy(out_bytes.data() + at, p, n);
    };
    if (has_xing) {
        // optimized_main_data[0] always has "Xing"/"Info" at bytes 0-3, flags at 4-7
        // (regardless of whether the source was CRC or no-CRC)
        const auto& xing_out = optimized_main_data[0];
        if (xing_out.size() >= 8) {
            uint32_t flags = (static_cast<uint32_t>(xing_out[4]) << 24) |
                             (static_cast<uint32_t>(xing_out[5]) << 16) |
                             (static_cast<uint32_t>(xing_out[6]) << 8) |
                             static_cast<uint32_t>(xing_out[7]);

            int side_info_size = (version == MpegVersion::MPEG1) ?
                                 (channel_mode == ChannelMode::Mono ? 17 : 32) :
                                 (channel_mode == ChannelMode::Mono ? 9 : 17);
            // base = start of output main_data = after header(4) + crc + side_info
            size_t main_data_base = xing_header_file_pos + 4
                                    + static_cast<size_t>(crc_size_of(0))
                                    + static_cast<size_t>(side_info_size);
            // in output: 'Xing'(4) + flags(4) = offset 8
            size_t field_offset = 8;

            size_t total_mp3_bytes = mp3_end_pos - xing_header_file_pos;

            if (flags & 1) field_offset += 4; // frames field: skip it
            if (flags & 2) {
                uint8_t buf[4];
                buf[0] = (total_mp3_bytes >> 24) & 0xFF;
                buf[1] = (total_mp3_bytes >> 16) & 0xFF;
                buf[2] = (total_mp3_bytes >> 8) & 0xFF;
                buf[3] = (total_mp3_bytes >> 0) & 0xFF;
                patch(main_data_base + field_offset, buf, 4);
                field_offset += 4;
            }

            if ((flags & 4) && !audio_frame_file_pos.empty()) {
                // recalculate 100-entry TOC; toc[0] is always 0 (LAME convention)
                size_t n_audio = audio_frame_file_pos.size();
                uint8_t toc[100];
                toc[0] = 0;
                for (int k = 1; k < 100; ++k) {
                    size_t fi = (static_cast<size_t>(k) * n_audio) / 100;
                    if (fi >= n_audio) fi = n_audio - 1;
                    size_t pos = audio_frame_file_pos[fi] - xing_header_file_pos;
                    int v = static_cast<int>(256.0 * static_cast<double>(pos) / static_cast<double>(total_mp3_bytes));
                    toc[k] = static_cast<uint8_t>(std::min(v, 255));
                }
                patch(main_data_base + field_offset, toc, 100);
            }

            // The LAME extension ends with a checksum of the audio and a checksum of
            // the tag itself. Both describe what we have just rewritten — the audio
            // bytes are new, and the tag checksum covers the header, the side info
            // and the byte count and seek table patched above — so copying them
            // through would leave the frame asserting two things that are no longer
            // true. Order matters: the tag checksum covers the audio one.
            size_t lame_off = 8;
            if (flags & 1) lame_off += 4;
            if (flags & 2) lame_off += 4;
            if (flags & 4) lame_off += 100;
            if (flags & 8) lame_off += 4;
            const size_t lame_pos = main_data_base + lame_off;
            if (lame_pos < mp3_end_pos &&
                lame_extension_at(out_bytes.data() + lame_pos, mp3_end_pos - lame_pos)) {
                if (!audio_frame_file_pos.empty() && audio_frame_file_pos[0] < mp3_end_pos) {
                    const size_t audio_start = audio_frame_file_pos[0];
                    const uint16_t music = crc16_lame(out_bytes.data() + audio_start,
                                                      mp3_end_pos - audio_start);
                    const uint8_t mb[2] = {static_cast<uint8_t>(music >> 8), static_cast<uint8_t>(music & 0xFF)};
                    patch(lame_pos + 32, mb, 2);
                }
                const size_t tag_crc_pos = lame_pos + 34;
                const uint16_t tag = crc16_lame(out_bytes.data() + xing_header_file_pos,
                                                tag_crc_pos - xing_header_file_pos);
                const uint8_t tb[2] = {static_cast<uint8_t>(tag >> 8), static_cast<uint8_t>(tag & 0xFF)};
                patch(tag_crc_pos, tb, 2);
            }
        }
    }

    write_file_atomically(output_file, out_bytes);
}

} // namespace mp3packer
