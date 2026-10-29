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

private:
    const std::vector<MacroAction>* acts_ = nullptr;
    double eps_ = 0.001;
    size_t next_ = 0;
};

}  // namespace fw
