// FrameLayout against two references that share none of its tricks: every sequence
// of bitrates and paddings for short streams, and for long ones (across the
// reconstruction's segment boundaries) a plain dynamic program in 64-bit costs that
// keeps every row. Every layout returned is also replayed to check it is feasible.
#include "layout.hpp"
#include <algorithm>
#include <cstdio>
#include <random>

using namespace mp3packer;

namespace {

struct Spec {
    MpegVersion version;
    uint32_t samplerate;
    int cap;
};

std::vector<std::pair<int, std::pair<int, bool>>> sizes(const Spec& s) {
    static const int mpeg1[] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320};
    static const int mpeg2[] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160};
    const int* t = s.version == MpegVersion::MPEG1 ? mpeg1 : mpeg2;
    const int mult = s.version == MpegVersion::MPEG1 ? 144 : 72;
    std::vector<std::pair<int, std::pair<int, bool>>> out;
    for (int i = 1; i <= 14; ++i)
        for (int p = 0; p < 2; ++p) out.push_back({mult * t[i] * 1000 / static_cast<int>(s.samplerate) + p, {i, p != 0}});
    return out;
}

// total size of a layout, or -1 if some frame's data does not fit
long long replay(const Spec& s, const std::vector<LayoutFrame>& f, const std::vector<std::pair<int, bool>>& choice,
                 const LayoutSeam* seam) {
    const auto table = sizes(s);
    long long total = 0;
    int r = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        int size = -1;
        for (const auto& e : table)
            if (e.second == choice[i]) size = e.first;
        total += size;
        if (f[i].fixed) { r = 0; continue; }
        const int data = size - f[i].overhead;
        const int left = r + data - f[i].need;
        if (data < 0 || left < 0) return -1;
        if (seam && i + 1 == f.size()) {
            const int excess = std::max(0, left - s.cap), end = std::min(s.cap, left);
            const int moved = seam->has_prev ? std::min(end, r) : 0;
            if (excess + end - moved + seam->extra > s.cap) return -1;
        }
        r = std::min(s.cap, left);
    }
    return total;
}

long long brute(const Spec& s, const std::vector<LayoutFrame>& f, const LayoutSeam* seam) {
    const auto table = sizes(s);
    long long best = -1;
    std::vector<size_t> idx(f.size(), 0);
    for (size_t i = 0; i < f.size(); ++i)
        if (f[i].fixed) idx[i] = static_cast<size_t>(2 * (f[i].fixed_index - 1) + (f[i].fixed_padding ? 1 : 0));
    for (;;) {
        long long total = 0;
        int r = 0;
        bool ok = true;
        for (size_t i = 0; i < f.size() && ok; ++i) {
            const int size = table[idx[i]].first;
            total += size;
            if (f[i].fixed) { r = 0; continue; }
            const int data = size - f[i].overhead, left = r + data - f[i].need;
            ok = data >= 0 && left >= 0;
            if (ok && seam && i + 1 == f.size()) {
                const int excess = std::max(0, left - s.cap), end = std::min(s.cap, left);
                const int moved = seam->has_prev ? std::min(end, r) : 0;
                ok = excess + end - moved + seam->extra <= s.cap;
            }
            r = std::min(s.cap, left);
        }
        if (ok && (best < 0 || total < best)) best = total;
        size_t k = 0;
        while (k < f.size() && (f[k].fixed || ++idx[k] == table.size())) {
            if (!f[k].fixed) idx[k] = 0;
            ++k;
        }
        if (k == f.size()) break;
    }
    return best;
}

// a dynamic program written the obvious way: 64-bit costs, every row kept
long long plain(const Spec& s, const std::vector<LayoutFrame>& f) {
    const auto table = sizes(s);
    const long long inf = 1LL << 60;
    std::vector<long long> cur(static_cast<size_t>(s.cap) + 1, inf);
    cur[0] = 0;
    for (const LayoutFrame& fr : f) {
        std::vector<long long> next(cur.size(), inf);
        for (int r = 0; r <= s.cap; ++r) {
            if (cur[static_cast<size_t>(r)] >= inf) continue;
            for (const auto& e : table) {
                if (fr.fixed) {
                    if (e.second != std::make_pair(fr.fixed_index, fr.fixed_padding)) continue;
                    next[0] = std::min(next[0], cur[static_cast<size_t>(r)] + e.first);
                    continue;
                }
                const int left = r + e.first - fr.overhead - fr.need;
                if (e.first - fr.overhead < 0 || left < 0) continue;
                const size_t t = static_cast<size_t>(std::min(s.cap, left));
                next[t] = std::min(next[t], cur[static_cast<size_t>(r)] + e.first);
            }
        }
        cur = next;
    }
    const long long best = *std::min_element(cur.begin(), cur.end());
    return best >= inf ? -1 : best;
}

}  // namespace

int main() {
    std::mt19937 rng(2026);
    const Spec specs[] = {{MpegVersion::MPEG1, 44100, 511}, {MpegVersion::MPEG1, 32000, 511},
                          {MpegVersion::MPEG2, 22050, 255}, {MpegVersion::MPEG25, 8000, 255},
                          {MpegVersion::MPEG1, 48000, 40}};   // a tiny cap makes clamping common
    int fails = 0, checked = 0;

    for (int t = 0; t < 1500 && fails < 10; ++t) {
        const Spec& s = specs[rng() % 5];
        const FrameLayout layout(s.version, s.samplerate, s.cap);
        const size_t n = 1 + rng() % (t % 10 == 0 ? 4 : 3);
        std::vector<LayoutFrame> f(n);
        const int scale = s.version == MpegVersion::MPEG1 ? 900 : 300;
        for (size_t i = 0; i < n; ++i) {
            f[i].overhead = 4 + (rng() % 2 ? 32 : 17) + (rng() % 4 == 0 ? 2 : 0);
            f[i].need = static_cast<int>(rng() % static_cast<unsigned>(scale));
            if (rng() % 6 == 0) f[i].need = 0;
            if (i == 0 && rng() % 4 == 0) {
                f[i].fixed = true;
                f[i].fixed_index = 1 + static_cast<int>(rng() % 14);
                f[i].fixed_padding = rng() & 1;
            }
        }
        LayoutSeam seam{static_cast<int>(rng() % static_cast<unsigned>(s.cap + 1)), n >= 2};
        const LayoutSeam* rule = (n >= 2 && rng() % 3 == 0) ? &seam : nullptr;
        const long long want = brute(s, f, rule);
        if (want < 0) continue;   // no layout at all: only a malformed stream gets here
        ++checked;
        const auto steps = layout.solve(f, rule);
        std::vector<std::pair<int, bool>> choice;
        for (const auto& st : steps) choice.push_back({st.index, st.padding});
        const long long got = replay(s, f, choice, rule);
        if (got != want) {
            printf("trial %d (%zu frames, cap %d, seam %d): layout costs %lld, the best is %lld\n", t, n, s.cap, rule != nullptr, got, want);
            ++fails;
        }
    }

    for (int t = 0; t < 12 && fails < 10; ++t) {
        const Spec& s = specs[t % 5];
        const FrameLayout layout(s.version, s.samplerate, s.cap);
        const size_t n = 300 + rng() % 500;
        std::vector<LayoutFrame> f(n);
        // music-like: a slowly wandering demand with bursts, so the reservoir matters
        // the largest payload a frame can carry bounds the demand, or nothing fits
        const int most = sizes(s).back().first - 36;
        double level = most / 3.0;
        for (size_t i = 0; i < n; ++i) {
            level = std::min(std::max(0.0, level + static_cast<double>(static_cast<int>(rng() % 81) - 40)), most * 0.8);
            f[i].overhead = 4 + 32;
            f[i].need = static_cast<int>(level) + (rng() % 20 == 0 ? static_cast<int>(rng() % static_cast<unsigned>(most / 2)) : 0);
            f[i].need = std::min(f[i].need, most);
        }
        f[0] = {0, 36, true, 9, false};
        const long long want = plain(s, f);
        const auto steps = layout.solve(f);
        std::vector<std::pair<int, bool>> choice;
        for (const auto& st : steps) choice.push_back({st.index, st.padding});
        const long long got = replay(s, f, choice, nullptr);
        ++checked;
        if (want < 0 || got != want) {
            printf("long trial %d (%zu frames): layout costs %lld, plain program says %lld\n", t, n, got, want);
            ++fails;
        }
    }
    printf("%d layouts checked\n", checked);
    printf(fails ? "FAILURES: %d\n" : "ok\n", fails);
    return fails != 0;
}
