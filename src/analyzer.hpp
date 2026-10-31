#pragma once
// Frame-window analyzer for position-based macros. No Geode includes: the game
// side only has to run a TrialSpec and report a TrialResult.
//
//   Analyzer a(macro, settings);
//   while (auto spec = a.nextTrial()) a.submit(runInGame(*spec));
//   const Report& r = a.report();
//
// Flow
//   1. REFERENCE: replay the untouched macro from tick 0 and record the player
//      x at every tick plus the tick each input fired on.
//   2. For every input group: for offset d, take the x the player had at tick
//      (T + d) in the reference, move the group to that x, replay from tick 0,
//      PASS if the player survives until a fixed end tick.
//   3. WindowSearch decides which offsets to try and builds [lo, hi].

#include <algorithm>
#include <climits>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <vector>

#include "macro.hpp"
#include "overlay.hpp"
#include "window.hpp"

namespace fw {

struct AnalyzerSettings {
    int radius = 10;            // max ticks scanned each side
    bool thorough = false;
    int probeBeyond = 0;        // 0 = stop at the first FAIL on each side
    bool verify = false;        // true = run each boundary twice
    // PASS = survive until the next input(s) of the ORIGINAL macro: a click must survive until its release,
    // a release until the next click. (Defaults: 1 following input, no extra ticks.)
    int horizonGroups = 1;      // survive until this many following inputs (same player) fired...
    int horizonMinTicks = 0;    // ...and at least this many ticks after the tested input
    int horizonMargin = 0;      // extra ticks survived after the following input fired
    bool skipFreeReleases = true;  // a release that survives -N and +N ticks is "free" and not analysed
    int freeReleaseTicks = 10;
    bool horizonToEnd = false;  // survive to the end of the reference run
    double epsilon = 0.001;     // x tolerance used by playback
    int onlyAction = 0;         // 1-based action number, 0 = every input
    int maxRunTicks = 240 * 180;  // hard stop for one run
    int maxWidth = 10;          // a window this wide is reported as "10+" and the next input is analysed (0 = off)
    bool useCheckpoints = true; // start each trial from a checkpoint taken at the previous input (needs the game side to support it)
};

struct TrialSpec {
    std::vector<MacroAction> actions;  // sorted by x; `id` identifies the original action
    int endTick = -1;                  // run until this tick index has been executed (-1 = no limit)
    int stopAfterLastFire = -1;        // reference: stop N ticks after the last input fired (-1 = off)
    int maxTick = 0;                   // safety stop
    bool recordX = false;              // record the pre-step x of every tick
    bool captureCheckpoints = false;   // reference: take a checkpoint at the END of every tick in which inputs fired
    int checkpointTick = -1;           // trial: start from the checkpoint taken at the end of this tick (ticks stay absolute)
};

struct TrialResult {
    bool died = false;
    int deathTick = -1;       // tick index during which the player died
    double deathX = 0.0;
    std::string deathNote;    // what killed the player (filled by the game side, shown in the report)
    bool completed = false;   // level finished
    bool timedOut = false;    // hit maxTick
    bool aborted = false;     // cancelled / unexpected reset: result is meaningless
    int ticksRun = 0;         // number of ticks executed (tick indices 0..ticksRun-1)
    std::vector<int> firedTick;  // per position in TrialSpec::actions, -1 = never fired
    std::vector<double> xs;      // only if recordX: xs[t] = x before tick t
    std::vector<int> checkpointTicks;  // ticks a checkpoint was actually captured for
    std::vector<double> ys;      // only if recordX: ys[t] = y before tick t (used to place the on-screen markers)
};

enum class GroupState { NotSelected, Analyzed, Redundant, Unreached, Unmappable, Free };

struct GroupReport {
    int group = 0;
    int player = 0;
    double x = 0.0;
    double y = 0.0;      // player y when the input fired in the reference run
    GroupKind kind = GroupKind::NoOp;
    int number = 0;      // 1-based number of the first action
    int actionCount = 0;
    GroupState state = GroupState::NotSelected;
    int refTick = -1;
    int endTick = -1;
    bool horizonCapped = false;
    bool usedCheckpoint = false;  // trials started from a checkpoint
    bool cpFallback = false;      // the checkpoint run did not reproduce the original timing, re-measured from the level start
    WindowResult w;
};

struct Report {
    std::vector<GroupReport> groups;
    bool referenceRan = false;
    int refTicks = 0;
    bool refDied = false;
    int refDeathTick = -1;
    double refDeathX = 0.0;
    std::string refDeathNote;
    bool refCompleted = false;
    bool refTimedOut = false;
    bool usedCheckpoints = false;
    bool complete = false;      // every selected input was finished (not cancelled)
    int resumed = 0;            // inputs taken over from an earlier, interrupted analysis
    int cpCaptured = 0;
    std::string note;  // top-level problem, empty if none
    int totalTrials = 0;
    bool cancelled = false;
    std::string cancelReason;
};

struct Progress {
    int groupsDone = 0;
    int groupsTotal = 0;
    int trials = 0;
    int currentNumber = 0;  // action number being analysed (0 = reference)
    int currentOffset = 0;
};

class Analyzer {
public:
    Analyzer(MacroFile macro, AnalyzerSettings s) : macro_(std::move(macro)), s_(s) {
        gs_ = buildGroups(macro_.actions, std::max(1e-6, s_.epsilon));
        report_.groups.resize(gs_.groups.size());
        for (size_t i = 0; i < gs_.groups.size(); ++i) {
            const Group& g = gs_.groups[i];
            GroupReport& r = report_.groups[i];
            r.group = static_cast<int>(i);
            r.player = g.player;
            r.x = g.x;
            r.kind = g.kind;
            r.number = g.number;
            r.actionCount = static_cast<int>(g.actions.size());
        }
        refFire_.assign(gs_.groups.size(), -1);
    }

    // Results of an interrupted analysis of the SAME macro: finished inputs are taken over instead of re-measured.
    void usePrior(const std::vector<PriorRow>& rows) { prior_ = rows; }

    const Report& report() const { return report_; }
    const GroupSet& groups() const { return gs_; }
    const MacroFile& macro() const { return macro_; }
    bool finished() const { return stage_ == Stage::Done; }

    Progress progress() const {
        Progress p;
        p.groupsTotal = static_cast<int>(selected_.size());
        p.groupsDone = std::min<int>(nextSel_ - (cur_ ? 1 : 0), p.groupsTotal);
        p.trials = report_.totalTrials;
        if (cur_) {
            p.currentNumber = gs_.groups[cur_->group].number;
            p.currentOffset = pendingOffset_;
        }
        return p;
    }

    void cancel(const std::string& why) {
        report_.cancelled = true;
        report_.cancelReason = why;
        if (cur_) {  // an unfinished search is NOT a result: leave that input for a later resume
            report_.groups[cur_->group].state = GroupState::NotSelected;
            cur_.reset();
        }
        stage_ = Stage::Done;
        pendingSpec_.reset();
    }

    std::optional<TrialSpec> nextTrial() {
        switch (stage_) {
            case Stage::Done: return std::nullopt;
            case Stage::NeedReference: {
                if (macro_.actions.empty()) { stage_ = Stage::Done; return std::nullopt; }
                TrialSpec t;
                t.actions = macro_.actions;
                t.recordX = true;
                t.captureCheckpoints = s_.useCheckpoints;
                t.stopAfterLastFire = std::max(s_.horizonMinTicks, s_.radius) + 12;
                t.maxTick = s_.maxRunTicks;
                pendingSpec_ = t;
                stage_ = Stage::WaitReference;
                return pendingSpec_;
            }
            case Stage::WaitReference:
            case Stage::WaitTrial:
                return pendingSpec_;  // submit() not called yet: same trial again
            case Stage::Search: break;
        }

        while (true) {
            if (!cur_) {
                if (nextSel_ >= selected_.size()) { report_.complete = !report_.cancelled; stage_ = Stage::Done; return std::nullopt; }
                beginGroup(selected_[nextSel_++]);
                continue;
            }
            if (cur_->freeIdx < cur_->freeOffsets.size()) {  // release filter probes first
                pendingOffset_ = cur_->freeOffsets[cur_->freeIdx];
                pendingSpec_ = buildTrial(pendingOffset_);
                stage_ = Stage::WaitTrial;
                return pendingSpec_;
            }
            if (cur_->freeAllPass && !cur_->freeOffsets.empty()) {
                GroupReport& rep = report_.groups[cur_->group];
                rep.state = GroupState::Free;
                rep.w = WindowResult{};
                rep.w.trials = static_cast<int>(cur_->freeOffsets.size());
                cur_.reset();
                continue;
            }
            std::optional<int> off = cur_->search.next();
            if (!off) {
                if (cur_->usingCp && cur_->search.result().baseFailed) {
                    // the checkpoint run did not reproduce the original timing: measure this input from the level start instead
                    cur_->usingCp = false;
                    cur_->fellBack = true;
                    cur_->search = WindowSearch(cur_->cfg);
                    continue;
                }
                finalizeGroup();
                continue;
            }
            pendingOffset_ = *off;
            pendingSpec_ = buildTrial(*off);
            stage_ = Stage::WaitTrial;
            return pendingSpec_;
        }
    }

    void submit(const TrialResult& r) {
        if (r.aborted) { cancel("run aborted (level reset, paused or left)"); return; }
        if (stage_ == Stage::WaitReference) { processReference(r); return; }
        if (stage_ != Stage::WaitTrial || !cur_) return;
        ++report_.totalTrials;
        const Verdict v = classify(r);
        cur_->search.submit(pendingOffset_, v);  // also seeds the real search with the probe results
        if (cur_->freeIdx < cur_->freeOffsets.size() && pendingOffset_ == cur_->freeOffsets[cur_->freeIdx]) {
            ++cur_->freeIdx;
            if (v != Verdict::Pass) {  // not free: stop probing, run the normal search
                cur_->freeAllPass = false;
                cur_->freeIdx = cur_->freeOffsets.size();
            }
        }
        pendingSpec_.reset();
        stage_ = Stage::Search;
    }

private:
    static constexpr int kHorizonTail = 60;  // ticks to survive after the last inputs of the macro
    enum class Stage { NeedReference, WaitReference, Search, WaitTrial, Done };

    struct Current {
        int group = 0;
        int refTick = 0;
        int endTick = 0;
        bool capped = false;
        int cpTick = -1;
        bool usingCp = false;
        bool fellBack = false;
        std::vector<int> freeOffsets;  // release filter: +N then -N
        size_t freeIdx = 0;
        bool freeAllPass = true;
        SearchConfig cfg;
        WindowSearch search;
        Current(int g, int t, int e, bool c, int cp, const SearchConfig& config)
            : group(g), refTick(t), endTick(e), capped(c), cpTick(cp), usingCp(cp >= 0), cfg(config), search(config) {}
    };

    bool isNoOp(int gi) const { return gs_.groups[gi].kind == GroupKind::NoOp; }

    void processReference(const TrialResult& r) {
        report_.referenceRan = true;
        report_.refTicks = r.ticksRun;
        report_.refDied = r.died;
        report_.refDeathTick = r.deathTick;
        report_.refDeathX = r.deathX;
        report_.refDeathNote = r.deathNote;
        report_.refCompleted = r.completed;
        report_.refTimedOut = r.timedOut;
        cpTicks_.clear();
        for (int t : r.checkpointTicks) cpTicks_.insert(t);
        report_.cpCaptured = static_cast<int>(cpTicks_.size());
        report_.usedCheckpoints = s_.useCheckpoints && !cpTicks_.empty();
        xs_ = r.xs;
        ys_ = r.ys;
        refDeath_ = r.died ? r.deathTick : -1;
        pendingSpec_.reset();

        if (xs_.empty() || r.ticksRun <= 0) {
            report_.note = "the reference run produced no physics ticks (is the tick hook firing?)";
            stage_ = Stage::Done;
            return;
        }
        for (size_t gi = 0; gi < gs_.groups.size(); ++gi) {
            int id = gs_.groups[gi].actions[0];
            refFire_[gi] = (id < static_cast<int>(r.firedTick.size())) ? r.firedTick[id] : -1;
        }

        // Which groups to analyse.
        for (size_t gi = 0; gi < gs_.groups.size(); ++gi) {
            bool pick = true;
            if (s_.onlyAction > 0) {
                pick = false;
                for (int id : gs_.groups[gi].actions) if (id + 1 == s_.onlyAction) pick = true;
            }
            if (pick) selected_.push_back(static_cast<int>(gi));
        }
        if (selected_.empty()) {
            report_.note = "action #" + std::to_string(s_.onlyAction) + " does not exist in this macro";
            stage_ = Stage::Done;
            return;
        }
        stage_ = Stage::Search;
    }

    bool monotoneAt(int t) const {
        if (t <= 0) return true;
        if (t >= static_cast<int>(xs_.size())) return false;
        return xs_[t] - xs_[t - 1] > 2.0 * s_.epsilon;
    }

    // Neighbour = previous/next group of the SAME player that actually changes state.
    int neighbourTick(int gi, int dir) const {
        const auto& list = gs_.byPlayer[gs_.groups[gi].player];
        int at = -1;
        for (size_t i = 0; i < list.size(); ++i) if (list[i] == gi) at = static_cast<int>(i);
        for (int i = at + dir; i >= 0 && i < static_cast<int>(list.size()); i += dir) {
            if (isNoOp(list[i])) continue;
            int t = refFire_[list[i]];
            if (t >= 0 && (refDeath_ < 0 || t < refDeath_)) return t;
            if (dir > 0) return -1;  // unreached: nothing to collide with
        }
        return -1;
    }

    // Take over a finished input of an interrupted analysis (same number AND same x).
    bool takePrior(int gi) {
        const Group& g = gs_.groups[gi];
        for (const PriorRow& pr : prior_) {
            if (pr.number != g.number || std::fabs(pr.x - g.x) > 1e-3) continue;
            GroupReport& rep = report_.groups[gi];
            if (pr.state == "free") {
                rep.state = GroupState::Free;
            } else if (pr.state == "analyzed" && pr.width > 0 && pr.flags.find("BASE-FAIL") == std::string::npos) {
                rep.state = GroupState::Analyzed;
                rep.w.lo = pr.lo;
                rep.w.hi = pr.hi;
                auto lim = [](char c) {
                    switch (c) {
                        case 'R': return Limit::Radius;
                        case 'N': return Limit::Neighbor;
                        case 'P': return Limit::Plateau;
                        case 'E': return Limit::RefEnd;
                        case 'S': return Limit::Start;
                        case 'C': return Limit::Cap;
                        default: return Limit::None;
                    }
                };
                rep.w.clipLo = lim(pr.clipLo);
                rep.w.clipHi = lim(pr.clipHi);
                if (rep.w.clipHi == Limit::Cap) { rep.w.capped = true; rep.w.capWidth = pr.width; }
                rep.w.unstable = pr.flags.find("UNSTABLE") != std::string::npos;
                rep.w.drift = pr.flags.find("DRIFT") != std::string::npos;
                rep.w.island = pr.flags.find("ISLAND") != std::string::npos;
                rep.cpFallback = pr.flags.find("CP-FALLBACK") != std::string::npos;
                rep.horizonCapped = pr.flags.find("HORIZON-CAPPED") != std::string::npos;
            } else {
                return false;
            }
            rep.y = pr.y;
            rep.refTick = refFire_[gi];
            ++report_.resumed;
            return true;
        }
        return false;
    }

    void beginGroup(int gi) {
        GroupReport& rep = report_.groups[gi];
        if (isNoOp(gi)) { rep.state = GroupState::Redundant; return; }
        if (takePrior(gi)) return;
        const int T = refFire_[gi];
        rep.refTick = T;
        if (T < 0 || (refDeath_ >= 0 && T >= refDeath_)) { rep.state = GroupState::Unreached; return; }

        const int last = static_cast<int>(xs_.size()) - 1;  // last tick whose x is known
        const int R = std::max(1, s_.radius);

        SearchConfig cfg;
        cfg.thorough = s_.thorough;
        cfg.probeBeyond = std::max(0, s_.probeBeyond);
        cfg.verify = s_.verify;
        cfg.maxWidth = s_.thorough ? 0 : std::max(0, s_.maxWidth);

        // ---- range, clipped by neighbours / start / end of data
        const int prevT = neighbourTick(gi, -1);
        const int nextT = neighbourTick(gi, +1);
        const int minTick = (prevT >= 0) ? prevT + 1 : 0;
        const Limit minReason = (prevT >= 0) ? Limit::Neighbor : Limit::Start;
        int maxTick = last;
        Limit maxReason = Limit::RefEnd;
        if (nextT >= 0 && nextT - 1 < maxTick) { maxTick = nextT - 1; maxReason = Limit::Neighbor; }

        cfg.rangeLo = -R; cfg.limitLo = Limit::Radius;
        if (minTick - T > -R) { cfg.rangeLo = minTick - T; cfg.limitLo = minReason; }
        cfg.rangeHi = R; cfg.limitHi = Limit::Radius;
        if (maxTick - T < R) { cfg.rangeHi = maxTick - T; cfg.limitHi = maxReason; }
        cfg.rangeLo = std::min(cfg.rangeLo, 0);
        cfg.rangeHi = std::max(cfg.rangeHi, 0);

        // ---- the position-based macro can only place an input on a tick if x grows every tick
        for (int d = 1; d <= cfg.rangeHi; ++d) {
            if (!monotoneAt(T + d)) { cfg.rangeHi = d - 1; cfg.limitHi = Limit::Plateau; break; }
        }
        for (int d = 0; d >= cfg.rangeLo; --d) {
            if (!monotoneAt(T + d)) { cfg.rangeLo = d + 1; cfg.limitLo = Limit::Plateau; break; }
        }
        if (cfg.rangeLo > 0 || cfg.rangeHi < 0) {  // not even the original tick is unique
            rep.state = GroupState::Unmappable;
            return;
        }

        // ---- end tick: identical for every offset, so results are comparable.
        // Default: the next input of the ORIGINAL macro (a click must survive until its release, a release until the next click).
        int end;
        bool capped = false;
        if (s_.horizonToEnd) {
            end = last;
        } else {
            end = T + std::max(0, s_.horizonMinTicks);
            const auto& list = gs_.byPlayer[gs_.groups[gi].player];
            int at = -1;
            for (size_t i = 0; i < list.size(); ++i) if (list[i] == gi) at = static_cast<int>(i);
            int seen = 0;
            for (int i = at + 1; i < static_cast<int>(list.size()) && seen < s_.horizonGroups; ++i) {
                if (isNoOp(list[i])) continue;
                ++seen;
                int t = refFire_[list[i]];
                if (t < 0) break;
                end = std::max(end, t + std::max(0, s_.horizonMargin));
            }
            if (seen < s_.horizonGroups) end = std::max(end, T + kHorizonTail);  // fewer inputs left than asked for
        }
        int cap = last;
        if (refDeath_ >= 0) cap = std::min(cap, refDeath_ - 1);
        if (end > cap) { end = cap; capped = true; }
        if (end < T) { end = T; capped = true; }

        rep.state = GroupState::Analyzed;
        rep.y = (T < static_cast<int>(ys_.size())) ? ys_[T] : 0.0;
        rep.endTick = end;
        rep.horizonCapped = capped;
        const int cpTick = (s_.useCheckpoints && prevT >= 0 && cpTicks_.count(prevT)) ? prevT : -1;
        cur_ = std::make_unique<Current>(gi, T, end, capped, cpTick, cfg);
        if (s_.skipFreeReleases && gs_.groups[gi].kind == GroupKind::Release) {
            const int n = std::max(1, s_.freeReleaseTicks);
            if (cfg.rangeHi >= n && cfg.rangeLo <= -n) cur_->freeOffsets = {n, -n};  // can only conclude if both are reachable
        }
    }

    void finalizeGroup() {
        GroupReport& rep = report_.groups[cur_->group];
        rep.w = cur_->search.result();
        rep.refTick = cur_->refTick;
        rep.endTick = cur_->endTick;
        rep.horizonCapped = cur_->capped;
        rep.usedCheckpoint = cur_->cpTick >= 0 && !cur_->fellBack;
        rep.cpFallback = cur_->fellBack;
        cur_.reset();
    }

    TrialSpec buildTrial(int off) const {
        const Group& g = gs_.groups[cur_->group];
        TrialSpec t;
        t.actions = macro_.actions;
        if (off != 0) {
            const double x = xs_[cur_->refTick + off];
            for (int id : g.actions) t.actions[id].x = x;
            std::stable_sort(t.actions.begin(), t.actions.end(),
                             [](const MacroAction& a, const MacroAction& b) { return a.x < b.x; });
        }
        t.endTick = cur_->endTick;
        t.maxTick = cur_->endTick + 16;
        t.checkpointTick = cur_->usingCp ? cur_->cpTick : -1;
        t.recordX = false;
        return t;
    }

    Verdict classify(const TrialResult& r) const {
        const Group& g = gs_.groups[cur_->group];
        int pos = -1;
        for (size_t i = 0; i < pendingSpec_->actions.size(); ++i) {
            if (pendingSpec_->actions[i].id == g.actions[0]) { pos = static_cast<int>(i); break; }
        }
        const int expect = cur_->refTick + pendingOffset_;
        const int fired = (pos >= 0 && pos < static_cast<int>(r.firedTick.size())) ? r.firedTick[pos] : -1;
        const int end = cur_->endTick;

        if (fired < 0) return r.died ? Verdict::Fail : Verdict::Invalid;
        if (fired != expect) return Verdict::Invalid;  // x->tick mapping drifted (speed change, other path)
        if (r.died && r.deathTick <= end) return Verdict::Fail;
        if (!r.completed && r.ticksRun <= end) return Verdict::Invalid;  // stopped before the end tick
        return Verdict::Pass;
    }

    MacroFile macro_;
    AnalyzerSettings s_;
    GroupSet gs_;
    Report report_;
    Stage stage_ = Stage::NeedReference;
    std::vector<PriorRow> prior_;
    std::set<int> cpTicks_;
    std::vector<double> xs_, ys_;
    std::vector<int> refFire_;
    int refDeath_ = -1;
    std::vector<int> selected_;
    size_t nextSel_ = 0;
    std::unique_ptr<Current> cur_;
    std::optional<TrialSpec> pendingSpec_;
    int pendingOffset_ = 0;
};

}  // namespace fw
