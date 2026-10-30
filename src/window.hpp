#pragma once
// Frame-window search for ONE input.
//
// The game runs trials asynchronously (many frames per trial), so this is a
// resumable state machine instead of a loop with a callback:
//
//     WindowSearch s(cfg);
//     while (auto off = s.next()) s.submit(*off, runTrialAt(*off));
//     WindowResult r = s.result();
//
// Offsets are in physics ticks relative to the recorded input (0 = original).
// Every decision is derived from the cache of results, so a re-run that
// disagrees with an earlier one automatically moves the boundary.

#include <algorithm>
#include <optional>
#include <vector>

namespace fw {

enum class Verdict : unsigned char { Pass, Fail, Invalid };

// Why a window edge is NOT a real FAIL boundary.
enum class Limit : unsigned char { None, Radius, Neighbor, Plateau, RefEnd, Start, Cap };

inline char limitChar(Limit l) {
    switch (l) {
        case Limit::Radius: return 'R';
        case Limit::Neighbor: return 'N';
        case Limit::Plateau: return 'P';
        case Limit::RefEnd: return 'E';
        case Limit::Start: return 'S';
        case Limit::Cap: return 'C';
        default: return ' ';
    }
}

struct SearchConfig {
    int rangeLo = -8;  // inclusive, <= 0
    int rangeHi = 8;   // inclusive, >= 0
    Limit limitLo = Limit::Radius;
    Limit limitHi = Limit::Radius;
    bool thorough = false;  // test every offset in range, not just outward from 0
    int probeBeyond = 1;    // extra offsets tested past the first FAIL (finds islands)
    bool verify = true;     // run each boundary offset twice
    int maxWidth = 0;       // > 0: stop as soon as the window is at least this wide (reported as "N+")
};

struct WindowResult {
    bool baseFailed = false;  // offset 0 did not pass twice: reference is not reproducible
    int lo = 0, hi = 0;       // inclusive
    Limit clipLo = Limit::None;  // != None: edge hit a limit before any FAIL
    Limit clipHi = Limit::None;
    bool island = false;      // a PASS exists beyond the first FAIL
    bool unstable = false;    // same offset gave different results
    bool drift = false;       // a trial fired the input on a different tick than planned
    bool capped = false;      // window reached SearchConfig::maxWidth: reported as capWidth+ and not measured further
    int capWidth = 0;
    int trials = 0;
    int width() const { return baseFailed ? 0 : (capped ? capWidth : hi - lo + 1); }
};

class WindowSearch {
public:
    explicit WindowSearch(SearchConfig c) : cfg_(c) {
        cfg_.rangeLo = std::min(cfg_.rangeLo, 0);
        cfg_.rangeHi = std::max(cfg_.rangeHi, 0);
        runs_.resize(static_cast<size_t>(cfg_.rangeHi - cfg_.rangeLo + 1));
    }

    // Next offset to run, or nullopt when the search is finished.
    // Returns the same offset again until submit() is called for it.
    std::optional<int> next() {
        if (pending_) return pending_;
        pending_ = pick();
        return pending_;
    }

    void submit(int offset, Verdict v) {
        if (offset < cfg_.rangeLo || offset > cfg_.rangeHi) return;
        runs_[idx(offset)].push_back(v);
        ++trials_;
        if (pending_ && *pending_ == offset) pending_.reset();
    }

    WindowResult result() const {
        WindowResult r;
        r.trials = trials_;
        for (const auto& rs : runs_) {
            bool anyPass = false, anyOther = false;
            for (Verdict v : rs) {
                if (v == Verdict::Pass) anyPass = true;
                else anyOther = true;
                if (v == Verdict::Invalid) r.drift = true;
            }
            if (anyPass && anyOther) r.unstable = true;
        }
        if (!passes(0)) {
            r.baseFailed = true;
            return r;
        }
        Scan s = scan();
        r.lo = s.lo;
        r.hi = s.hi;
        if (s.capped) {
            r.capped = true;
            r.capWidth = cfg_.maxWidth;
            r.clipHi = Limit::Cap;
            return r;
        }
        if (!s.hiFail && s.hi == cfg_.rangeHi) r.clipHi = cfg_.limitHi;
        if (!s.loFail && s.lo == cfg_.rangeLo) r.clipLo = cfg_.limitLo;
        for (int o = cfg_.rangeLo; o <= cfg_.rangeHi; ++o) {
            if ((o < r.lo || o > r.hi) && passes(o)) r.island = true;
        }
        return r;
    }

private:
    struct Scan {
        int lo = 0, hi = 0;
        std::optional<int> loFail, hiFail;
        std::optional<int> need;  // first untested offset the scan ran into
        bool capped = false;
    };

    size_t idx(int o) const { return static_cast<size_t>(o - cfg_.rangeLo); }
    const std::vector<Verdict>& runs(int o) const { return runs_[idx(o)]; }
    bool tested(int o) const { return !runs(o).empty(); }
    bool passes(int o) const {
        const auto& r = runs(o);
        if (r.empty()) return false;
        for (Verdict v : r) if (v != Verdict::Pass) return false;
        return true;
    }

    // Expands +1, -1, +2, -2 ... so both sides grow together; stops a side at its first FAIL,
    // and everything as soon as the window is wide enough (maxWidth).
    Scan scan() const {
        Scan s;
        int dr = 1, dl = -1;
        bool right = cfg_.rangeHi >= 1, left = cfg_.rangeLo <= -1;
        auto wideEnough = [&] { return cfg_.maxWidth > 0 && (s.hi - s.lo + 1) >= cfg_.maxWidth; };
        while (right || left) {
            if (right) {
                if (!tested(dr)) { s.need = dr; return s; }
                if (!passes(dr)) { s.hiFail = dr; right = false; }
                else { s.hi = dr; if (++dr > cfg_.rangeHi) right = false; }
                if (wideEnough()) { s.capped = true; return s; }
            }
            if (left) {
                if (!tested(dl)) { s.need = dl; return s; }
                if (!passes(dl)) { s.loFail = dl; left = false; }
                else { s.lo = dl; if (--dl < cfg_.rangeLo) left = false; }
                if (wideEnough()) { s.capped = true; return s; }
            }
        }
        return s;
    }

    std::optional<int> pick() const {
        // 1. the original timing must pass, and a failure is always confirmed once
        if (!tested(0)) return 0;
        if (!passes(0)) {
            if (runs(0).size() < 2) return 0;
            return std::nullopt;
        }
        // 2. expand outward until the first FAIL on each side
        Scan s = scan();
        if (s.need) return s.need;
        if (s.capped) return std::nullopt;  // wide enough: label it N+ and move on
        // 3. probe a few offsets past each FAIL: a PASS there is an "island"
        for (int k = 1; k <= cfg_.probeBeyond; ++k) {
            if (s.hiFail && *s.hiFail + k <= cfg_.rangeHi && !tested(*s.hiFail + k)) return *s.hiFail + k;
            if (s.loFail && *s.loFail - k >= cfg_.rangeLo && !tested(*s.loFail - k)) return *s.loFail - k;
        }
        // 4. thorough mode: everything in range
        if (cfg_.thorough) {
            for (int o = cfg_.rangeLo; o <= cfg_.rangeHi; ++o) if (!tested(o)) return o;
        }
        // 5. run each boundary a second time; a disagreement moves the boundary
        //    and the next call re-expands from the cache.
        if (cfg_.verify) {
            int cand[4] = {s.hi, s.lo, 0, 0};
            int n = 2;
            if (s.hiFail) cand[n++] = *s.hiFail;
            if (s.loFail) cand[n++] = *s.loFail;
            for (int i = 0; i < n; ++i) {
                if (cand[i] >= cfg_.rangeLo && cand[i] <= cfg_.rangeHi && runs(cand[i]).size() < 2) return cand[i];
            }
        }
        return std::nullopt;
    }

    SearchConfig cfg_;
    std::vector<std::vector<Verdict>> runs_;
    std::optional<int> pending_;
    int trials_ = 0;
};

}  // namespace fw
