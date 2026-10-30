#pragma once
// Position-based playback, exactly as the percent-macro format defines it:
// each physics tick compare the player's CURRENT x with the x of the next
// action; as soon as the player reached it, send the input and move on.
// Never looks at a clock or at a frame number.

#include <vector>

#include "macro.hpp"

namespace fw {

struct Fire {
    int pos = 0;       // index in the action vector that was bound
    bool down = false;
    bool p2 = false;
};

class Cursor {
public:
    void bind(const std::vector<MacroAction>* actions, double eps) {
        acts_ = actions;
        eps_ = eps;
        next_ = 0;
    }
    void reset() { next_ = 0; }
    size_t next() const { return next_; }
    bool done() const { return !acts_ || next_ >= acts_->size(); }

    // Call once per physics tick, BEFORE the tick runs, with the player's x.
    // `x >= action.x - eps` (not ==): float positions are never identical, and
    // a position jump must not leave an action stuck forever.
    // Several actions at the same x fire in file order in the same tick.
    void poll(double x, std::vector<Fire>& out) {
        if (!acts_) return;
        while (next_ < acts_->size() && (*acts_)[next_].x <= x + eps_) {
            const MacroAction& a = (*acts_)[next_];
            out.push_back({static_cast<int>(next_), a.down, a.p2});
            ++next_;
        }
    }

    // After a respawn / checkpoint / late start: continue from the first action at or after `x`.
    // If the player was holding at that point, press again right away (appended to `out` with pos = -1).
    void seekTo(double x, std::vector<Fire>& out) {
        if (!acts_) return;
        size_t i = 0;
        while (i < acts_->size() && (*acts_)[i].x < x - eps_) ++i;
        next_ = i;
        bool held[2] = {false, false};
        for (size_t k = 0; k < i; ++k) held[(*acts_)[k].p2 ? 1 : 0] = (*acts_)[k].down;
        for (int p = 0; p < 2; ++p) {
            if (held[p]) out.push_back({-1, true, p == 1});
        }
    }

private:
    const std::vector<MacroAction>* acts_ = nullptr;
    double eps_ = 0.001;
    size_t next_ = 0;
};

}  // namespace fw
