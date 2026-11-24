#include "layout.hpp"

#include <algorithm>
#include <iterator>
#include <numeric>

namespace mp3packer {
namespace {

// unreachable; costs are kept small by normalising, so adding a frame never wraps
constexpr int32_t kInf = 1 << 29;

int32_t lowest(const std::vector<int32_t>& v) {
    int32_t m = kInf;
    for (const int32_t x : v) m = std::min(m, x);
    return m;
}

size_t first_lowest(const std::vector<int32_t>& v) {
    return static_cast<size_t>(std::find(v.begin(), v.end(), lowest(v)) - v.begin());
}

}  // namespace

// Why runs. A stream's total size is its overheads, its data, the bytes padding
// throws away when the reservoir would overflow, and what is left unread at the
// end. So the cheapest way to reach reservoir state r costs r + w(r) plus a constant
// that is the same for every state, w being the least padding any path to r has
// thrown away; and a step that does not overflow the reservoir adds the same
// constant whichever bitrate it takes. w is therefore the whole state, and it is
// constant over long stretches of r — zero almost everywhere until the reservoir
// first fills — so it is kept as a few runs rather than 512 cells: a step is a
// merge of shifted runs, and the runs of every frame are cheap enough to keep, so
// walking back needs no recomputation.

FrameLayout::FrameLayout(const MpegVersion version, const uint32_t samplerate, const int cap) : cap_(cap) {
    static const int mpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int mpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const int* table = version == MpegVersion::MPEG1 ? mpeg1 : mpeg2;
    const int mult = version == MpegVersion::MPEG1 ? 144 : 72;
    for (int index = 1; index <= 14; ++index) {
        const int base = mult * table[index] * 1000 / static_cast<int>(samplerate);
        options_.push_back({base, index, false});
        options_.push_back({base + 1, index, true});
    }
}

bool FrameLayout::transition_ok(const int r, const int data, const LayoutFrame& frame, const LayoutSeam* seam) const {
    const int left = r + data - frame.need;
    if (left < 0) return false;
    if (!seam) return true;
    // what the frame after the seam inherits: anything clamped off as padding, the
    // slack the last frame's own back-reference cannot absorb, and the glue
    const int excess = std::max(0, left - cap_);
    const int end = std::min(cap_, left);
    const int moved = seam->has_prev ? std::min(end, r) : 0;
    return excess + end - moved + seam->extra <= cap_;
}

std::vector<int32_t> FrameLayout::to_costs(const std::vector<Run>& runs) const {
    std::vector<int32_t> costs(static_cast<size_t>(cap_) + 1, kInf);
    for (const Run& run : runs)
        for (int r = run.lo; r <= run.hi; ++r) costs[static_cast<size_t>(r)] = run.w + r;
    return costs;
}

std::vector<FrameLayout::Run> FrameLayout::from_costs(const std::vector<int32_t>& costs) const {
    std::vector<Run> runs;
    int32_t base = kInf;
    for (int r = 0; r <= cap_; ++r) {
        const int32_t c = costs[static_cast<size_t>(r)];
        if (c >= kInf) continue;
        const int32_t w = c - r;
        base = std::min(base, w);
        if (!runs.empty() && runs.back().hi + 1 == r && runs.back().w == w) runs.back().hi = r;
        else runs.push_back({r, r, w});
    }
    for (Run& run : runs) run.w -= base;
    return runs;
}

int64_t FrameLayout::seam_step(const std::vector<int32_t>& before, const LayoutFrame& frame, const LayoutSeam& seam,
                               std::vector<int32_t>& next) const {
    std::fill(next.begin(), next.end(), kInf);
    for (const Option& o : options_) {
        const int data = o.size - frame.overhead;
        if (data < 0) continue;
        for (int r = 0; r <= cap_; ++r) {
            if (before[static_cast<size_t>(r)] >= kInf || !transition_ok(r, data, frame, &seam)) continue;
            const size_t t = static_cast<size_t>(std::min(cap_, r + data - frame.need));
            next[t] = std::min(next[t], before[static_cast<size_t>(r)] + o.size);
        }
    }
    const int32_t m = lowest(next);
    if (m >= kInf) return 0;
    for (int32_t& v : next) v = v >= kInf ? kInf : v - m;
    return m;
}

int32_t FrameLayout::advance(const std::vector<Run>& before, const LayoutFrame& frame, std::vector<Run>& after,
                             bool& rescued, Work& work) const {
    const int cap = cap_;
    after.clear();
    rescued = false;
    if (frame.fixed) {
        // a tag frame: its own size, and nothing of it is left for the audio
        after.push_back({0, 0, 0});
        return 0;
    }
    // runs by padding thrown away, least first, so each state takes the least
    work.order.resize(before.size());
    std::iota(work.order.begin(), work.order.end(), size_t{0});
    std::stable_sort(work.order.begin(), work.order.end(),
                     [&before](const size_t a, const size_t b) { return before[a].w < before[b].w; });
    work.covered.clear();
    int32_t full = kInf;   // the cheapest way to a full reservoir
    for (const size_t j : work.order) {
        const Run& run = before[j];
        // where this run's states land below the cap, over every bitrate: the shifts
        // grow with the frame size, so the landing intervals arrive in order
        work.reach.clear();
        for (const Option& o : options_) {
            const int data = o.size - frame.overhead;
            if (data < 0) continue;
            const int s = data - frame.need;
            const int a = std::max(run.lo, -s), b = std::min(run.hi, cap - 1 - s);
            if (a <= b) {
                if (!work.reach.empty() && a + s <= work.reach.back().hi + 1)
                    work.reach.back().hi = std::max(work.reach.back().hi, b + s);
                else
                    work.reach.push_back({a + s, b + s, run.w});
            }
            // overflowing the cap throws away the excess; least from the run's lowest state
            const int from = std::max({run.lo, -s, cap - s});
            if (from <= run.hi) full = std::min(full, run.w + from + s - cap);
        }
        // what no run with less padding reaches already
        size_t c = 0;
        for (const Run& in : work.reach) {
            int cur = in.lo;
            while (c < work.covered.size() && work.covered[c].hi < cur) ++c;
            for (size_t k = c; k < work.covered.size() && work.covered[k].lo <= in.hi && cur <= in.hi; ++k) {
                if (work.covered[k].lo > cur) after.push_back({cur, work.covered[k].lo - 1, run.w});
                cur = std::max(cur, work.covered[k].hi + 1);
            }
            if (cur <= in.hi) after.push_back({cur, in.hi, run.w});
        }
        work.merged.clear();
        std::merge(work.covered.begin(), work.covered.end(), work.reach.begin(), work.reach.end(),
                   std::back_inserter(work.merged), [](const Run& x, const Run& y) { return x.lo < y.lo; });
        work.covered.clear();
        for (const Run& r : work.merged) {
            if (!work.covered.empty() && r.lo <= work.covered.back().hi + 1)
                work.covered.back().hi = std::max(work.covered.back().hi, r.hi);
            else
                work.covered.push_back(r);
        }
    }
    std::sort(after.begin(), after.end(), [](const Run& x, const Run& y) { return x.lo < y.lo; });
    if (full < kInf) after.push_back({cap, cap, full});
    if (after.empty()) {
        // data that fits no frame at all (only a malformed stream has it): the
        // largest frame, and the layout carries on from an empty reservoir
        rescued = true;
        after.push_back({0, 0, 0});
        return 0;
    }
    // adjacent runs of equal padding become one, and the least becomes zero
    size_t kept = 0;
    for (size_t k = 1; k < after.size(); ++k) {
        if (after[k].lo == after[kept].hi + 1 && after[k].w == after[kept].w) after[kept].hi = after[k].hi;
        else after[++kept] = after[k];
    }
    after.resize(kept + 1);
    int32_t m = kInf;
    for (const Run& r : after) m = std::min(m, r.w);
    for (Run& r : after) r.w -= m;
    return m;
}

std::vector<std::vector<int32_t>> FrameLayout::costs_from(const std::vector<LayoutFrame>& frames, const size_t first) const {
    std::vector<Run> cur{{0, 0, 0}}, next;
    Work work;
    std::vector<std::vector<int32_t>> kept;
    for (size_t i = 0; i < frames.size(); ++i) {
        if (i >= first) kept.push_back(to_costs(cur));
        bool rescued = false;
        advance(cur, frames[i], next, rescued, work);
        cur.swap(next);
    }
    kept.push_back(to_costs(cur));
    return kept;
}

bool FrameLayout::seam_fits(const std::vector<int32_t>& before, const LayoutFrame& last, const LayoutSeam& seam) const {
    std::vector<int32_t> next(static_cast<size_t>(cap_) + 1);
    seam_step(before, last, seam, next);
    return lowest(next) < kInf;
}

std::vector<LayoutStep> FrameLayout::solve(const std::vector<LayoutFrame>& frames, const LayoutSeam* seam) const {
    const size_t n = frames.size();
    std::vector<LayoutStep> steps(n);
    if (n == 0) return steps;
    const int cap = cap_;

    // the runs before every frame and after the last, end to end
    std::vector<Run> all;
    std::vector<size_t> start(n + 2);
    std::vector<int32_t> shifted(n, 0);
    std::vector<uint8_t> rescued(n, 0);
    std::vector<Run> cur{{0, 0, 0}}, next;   // the stream starts with an empty reservoir
    Work work;
    std::vector<int32_t> seam_before, seam_after(static_cast<size_t>(cap) + 1);
    int64_t seam_offset = 0;
    for (size_t i = 0; i < n; ++i) {
        start[i] = all.size();
        all.insert(all.end(), cur.begin(), cur.end());
        if (seam && i + 1 == n) {
            seam_before = to_costs(cur);
            seam_offset = seam_step(seam_before, frames[i], *seam, seam_after);
            next = from_costs(seam_after);
        } else {
            bool r = false;
            shifted[i] = advance(cur, frames[i], next, r, work);
            rescued[i] = r;
        }
        cur.swap(next);
    }
    start[n] = all.size();
    all.insert(all.end(), cur.begin(), cur.end());
    start[n + 1] = all.size();

    const auto runs_at = [&](const size_t i) { return std::make_pair(all.begin() + static_cast<ptrdiff_t>(start[i]),
                                                                     all.begin() + static_cast<ptrdiff_t>(start[i + 1])); };
    // the padding thrown away on the way to state r before frame i, or -1
    const auto waste_at = [&](const size_t i, const int r) -> int32_t {
        const auto range = runs_at(i);
        auto it = std::upper_bound(range.first, range.second, r, [](const int v, const Run& x) { return v < x.lo; });
        if (it == range.first) return -1;
        --it;
        return r <= it->hi ? it->w : -1;
    };
    // the cheapest state, the one with least left in the reservoir on a tie
    const auto cheapest = [&](const size_t i) {
        int best_r = 0;
        int64_t best = -1;
        const auto range = runs_at(i);
        for (auto it = range.first; it != range.second; ++it) {
            const int64_t c = static_cast<int64_t>(it->w) + it->lo;
            if (best < 0 || c < best) { best = c; best_r = it->lo; }
        }
        return best_r;
    };

    int t = cheapest(n);
    for (size_t i = n; i-- > 0;) {
        const LayoutFrame& frame = frames[i];
        const Option* chosen = nullptr;
        int from_state = -1;
        if (frame.fixed) {
            chosen = &options_[static_cast<size_t>(2 * (frame.fixed_index - 1) + (frame.fixed_padding ? 1 : 0))];
            from_state = cheapest(i);
        } else if (seam && i + 1 == n) {
            const int64_t target = static_cast<int64_t>(seam_after[static_cast<size_t>(t)]) + seam_offset;
            for (const Option& o : options_) {
                const int data = o.size - frame.overhead;
                if (data < 0) continue;
                for (int r = 0; r <= cap; ++r) {
                    if (seam_before[static_cast<size_t>(r)] >= kInf || !transition_ok(r, data, frame, seam)) continue;
                    if (std::min(cap, r + data - frame.need) != t) continue;
                    if (static_cast<int64_t>(seam_before[static_cast<size_t>(r)]) + o.size != target) continue;
                    from_state = r;
                    chosen = &o;
                    break;
                }
                if (chosen) break;
            }
        } else if (!rescued[i]) {
            const int32_t target = waste_at(i + 1, t) + shifted[i];
            // smallest frame first, then the smallest reservoir it can come from
            for (const Option& o : options_) {
                const int data = o.size - frame.overhead;
                if (data < 0) continue;
                const int s = data - frame.need;
                if (t < cap) {
                    const int r = t - s;
                    if (r < 0 || r > cap) continue;
                    if (waste_at(i, r) == target) { from_state = r; chosen = &o; break; }
                } else {
                    const auto range = runs_at(i);
                    for (auto it = range.first; it != range.second; ++it) {
                        const int from = std::max({it->lo, -s, cap - s});
                        if (from > it->hi) continue;
                        if (it->w + from + s - cap == target) { from_state = from; chosen = &o; break; }
                    }
                    if (chosen) break;
                }
            }
        }
        if (!chosen) {
            // the malformed-stream fallback advance() took
            chosen = &options_.back();
            from_state = cheapest(i);
        }
        steps[i] = {chosen->index, chosen->padding, chosen->size - frame.overhead};
        t = from_state;
    }
    return steps;
}

}  // namespace mp3packer
