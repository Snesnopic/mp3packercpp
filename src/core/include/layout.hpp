#ifndef MP3PACKERCPP_LAYOUT_HPP
#define MP3PACKERCPP_LAYOUT_HPP

#include "types.hpp"
#include <cstdint>
#include <vector>

namespace mp3packer {

// One frame as the layout sees it.
struct LayoutFrame {
    int need = 0;              // main data bytes it brings
    int overhead = 0;          // header, crc and side info bytes
    bool fixed = false;        // keeps fixed_index/fixed_padding, and the reservoir restarts empty after it
    int fixed_index = 0;
    bool fixed_padding = false;
};

// The last frame before a stream break: the frame after it reaches back `extra`
// bytes into glue appended to it, and inherits whatever the layout leaves unread
// around that glue, which must stay within the reservoir cap. has_prev: there is a
// frame before it to move slack into.
struct LayoutSeam {
    int extra = 0;
    bool has_prev = false;
};

struct LayoutStep {
    int index = 0;
    bool padding = false;
    int data_size = 0;         // payload bytes the frame offers
};

// Bitrate and padding for every frame such that the stream is as short as it can
// be. A frame's main data has to fit in its own payload plus the reservoir earlier
// frames left, which holds at most `cap` bytes (511, 255 for LSF); what would
// overflow it is lost as padding. Exact: a dynamic program over the reservoir.
class FrameLayout {
public:
    FrameLayout(MpegVersion version, uint32_t samplerate, int cap);

    std::vector<LayoutStep> solve(const std::vector<LayoutFrame>& frames, const LayoutSeam* seam = nullptr) const;

    // The cheapest cost of reaching each reservoir state before frame `first`, and
    // before every frame after it up to frames.size(), for checking seams.
    std::vector<std::vector<int32_t>> costs_from(const std::vector<LayoutFrame>& frames, size_t first) const;

    // Whether `last` can follow a prefix that ends in `before` under `seam`.
    bool seam_fits(const std::vector<int32_t>& before, const LayoutFrame& last, const LayoutSeam& seam) const;

private:
    struct Option {
        int size;
        int index;
        bool padding;
    };
    // reservoir states lo..hi, reached with w bytes of padding thrown away beyond
    // the least any state needed (see layout.cpp)
    struct Run {
        int lo;
        int hi;
        int32_t w;
    };
    struct Work {
        std::vector<size_t> order;
        std::vector<Run> reach, covered, merged;
    };
    std::vector<Option> options_;   // every bitrate and padding, by size
    int cap_;

    // One frame on from `before`; returns the padding subtracted to keep the least at zero.
    int32_t advance(const std::vector<Run>& before, const LayoutFrame& frame, std::vector<Run>& after,
                    bool& rescued, Work& work) const;
    // The same across a seam, over every state: costs (state plus padding) in, costs out.
    int64_t seam_step(const std::vector<int32_t>& before, const LayoutFrame& frame, const LayoutSeam& seam,
                      std::vector<int32_t>& next) const;
    std::vector<int32_t> to_costs(const std::vector<Run>& runs) const;
    std::vector<Run> from_costs(const std::vector<int32_t>& costs) const;
    bool transition_ok(int r, int data, const LayoutFrame& frame, const LayoutSeam* seam) const;
};

}  // namespace mp3packer

#endif
