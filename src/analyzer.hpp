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
#include "makemacro.hpp"
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
    bool pairClicks = true;     // a click and its release are ONE unit and get the same offset in every trial
    bool skipFreeReleases = true;  // a release that survives -N and +N ticks is "free" and not analysed
    int freeReleaseTicks = 10;
    bool horizonToEnd = false;  // survive to the end of the reference run
    double epsilon = 0.001;     // x tolerance used by playback
    int onlyAction = 0;         // 1-based action number, 0 = every input
    int maxRunTicks = 240 * 180;  // hard stop for one run
    int maxWidth = 10;          // a window this wide is reported as "10+" and the next input is analysed (0 = off)
    bool useCheckpoints = true; // start each trial from a checkpoint taken at the previous input (needs the game side to support it)
    // Fast mode (beta): once a few checkpoint runs reproduced the original timing, the original timing is trusted without a
    // test; and if -4 and +5 (for maxWidth 10) both pass, the window is assumed to be 10+ without testing in between.
    bool fastMode = false;
};

struct TrialSpec {
    std::vector<MacroAction> actions;  // sorted by x; `id` identifies the original action
    int endTick = -1;                  // run until this tick index has been executed (-1 = no limit)
    int stopAfterLastFire = -1;        // reference: stop N ticks after the last input fired (-1 = off)
    int maxTick = 0;                   // safety stop
    bool recordX = false;              // record the pre-step x of every tick
    bool captureCheckpoints = false;   // reference: take a checkpoint at the level start and after every tick in which inputs fired
    int startAt = -1;                  // trial: start from the checkpoint taken after this many ticks had run (0 = the level start); -1 = no checkpoint. Ticks stay absolute
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
    std::vector<int> checkpointAt;  // 'ticks already run' values a checkpoint exists for (0 = level start)
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
    int holdTicks = 0;            // click: ticks between the press and its release in the original macro
    bool horizonCapped = false;
    bool fastAssumed = false;     // fast mode: width assumed from the two extreme offsets
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
    bool cpDisabled = false;    // checkpoint starts were abandoned after repeated mismatches
    int cpOk = 0;               // inputs whose checkpoint start reproduced the original timing
    int cpFallbacks = 0;        // inputs that had to be re-measured from the level start
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
        us_ = buildUnits(gs_, macro_.actions, s_.pairClicks);
        report_.groups.resize(us_.units.size());
        for (size_t i = 0; i < us_.units.size(); ++i) {
            const Unit& u = us_.units[i];
            GroupReport& r = report_.groups[i];
            r.group = static_cast<int>(i);  // index of the UNIT
            r.player = u.player;
            r.x = u.x;
            r.kind = u.kind == UnitKind::Single ? u.single : GroupKind::Press;
            r.number = u.number;
            int n = 0;
            for (int gi : u.groups) n += static_cast<int>(gs_.groups[gi].actions.size());
            r.actionCount = n;
        }
        refFire_.assign(gs_.groups.size(), -1);
    }

    // Results of an interrupted analysis of the SAME macro: finished inputs are taken over instead of re-measured.
    void usePrior(const std::vector<PriorRow>& rows) { prior_ = rows; }

    const Report& report() const { return report_; }
    const GroupSet& groups() const { return gs_; }
    const UnitSet& unitSet() const { return us_; }

    // The macro with every analysed click moved to a tick inside its measured window (see makemacro.hpp).
    // Clicks without a trustworthy window (10+, unstable, drifted, not analysed) keep their recorded timing.
    std::vector<MacroAction> pickedActions(PickMode mode) const {
        std::vector<MacroAction> out = macro_.actions;
        for (size_t ui = 0; ui < us_.units.size(); ++ui) {
            const GroupReport& r = report_.groups[ui];
            if (r.state != GroupState::Analyzed || r.w.baseFailed || r.w.capped || r.w.unstable || r.w.drift) continue;
            const int d = pickOffset(r.w.lo, r.w.hi, mode);
            if (d == 0) continue;
            bool inRange = true;  // all-or-nothing: a click is never half moved
            for (int gi : us_.units[ui].groups) {
                const int t = refFire_[gi] + d;
                if (t < 0 || t >= static_cast<int>(xs_.size())) inRange = false;
            }
            if (!inRange) continue;
            for (int gi : us_.units[ui].groups)
                for (int id : gs_.groups[gi].actions) out[id].x = xs_[refFire_[gi] + d];
        }
        std::stable_sort(out.begin(), out.end(), [](const MacroAction& a, const MacroAction& b) { return a.x < b.x; });
        return out;
    }

    // x of every action id for original / center / earliest / latest (saved so a macro can be made later without analysing again)
    std::vector<PickRow> pickTable() const {
        std::vector<PickRow> rows(macro_.actions.size());
        const PickMode modes[3] = {PickMode::Center, PickMode::Earliest, PickMode::Latest};
        for (size_t i = 0; i < rows.size(); ++i) rows[i][0] = macro_.actions[i].x;
        for (int k = 0; k < 3; ++k) {
            for (const MacroAction& a : pickedActions(modes[k])) rows[a.id][k + 1] = a.x;
        }
        return rows;
    }
    const MacroFile& macro() const { return macro_; }
    bool finished() const { return stage_ == Stage::Done; }

    Progress progress() const {
        Progress p;
        p.groupsTotal = static_cast<int>(selected_.size());
        p.groupsDone = std::min<int>(nextSel_ - (cur_ ? 1 : 0), p.groupsTotal);
        p.trials = report_.totalTrials;
        if (cur_) {
            p.currentNumber = us_.units[cur_->group].number;
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
                noteCheckpointOk();
                cur_.reset();
                continue;
            }
            if (cur_->extremeIdx < cur_->extremeOffsets.size()) {  // fast mode: the two extreme offsets first
                pendingOffset_ = cur_->extremeOffsets[cur_->extremeIdx];
                pendingSpec_ = buildTrial(pendingOffset_);
                stage_ = Stage::WaitTrial;
                return pendingSpec_;
            }
            if (cur_->extremeAllPass && !cur_->extremeOffsets.empty()) {  // both pass: assume everything in between does too
                GroupReport& rep = report_.groups[cur_->group];
                rep.w = WindowResult{};
                rep.w.lo = cur_->extremeOffsets[1];
                rep.w.hi = cur_->extremeOffsets[0];
                rep.w.capped = true;
                rep.w.capWidth = s_.maxWidth;
                rep.w.clipHi = Limit::Cap;
                rep.w.trials = static_cast<int>(cur_->extremeOffsets.size());
                rep.fastAssumed = true;
                rep.endTick = cur_->endTick;
                rep.horizonCapped = cur_->capped;
                rep.usedCheckpoint = cur_->cpAt >= 0 && !cur_->fellBack;
                noteCheckpointOk();
                cur_.reset();
                continue;
            }
            std::optional<int> off = cur_->search.next();
            if (!off) {
                if (cur_->usingCp && cur_->search.result().baseFailed) {
                    // the checkpoint run did not reproduce the original timing: measure this input from the level start instead
                    ++report_.cpFallbacks;
                    ++cpFailStreak_;
                    {  // a from-start run wipes the practice checkpoints, so one failed checkpoint start ends checkpoint use for this run
                        cpDisabled_ = true;
                        report_.cpDisabled = true;
                    }
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
        } else if (cur_->extremeIdx < cur_->extremeOffsets.size() && pendingOffset_ == cur_->extremeOffsets[cur_->extremeIdx]) {
            ++cur_->extremeIdx;
            if (v != Verdict::Pass) {  // narrower than maxWidth: measure it normally (the result so far is kept)
                cur_->extremeAllPass = false;
                cur_->extremeIdx = cur_->extremeOffsets.size();
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
        int cpAt = -1;
        bool usingCp = false;
        bool fellBack = false;
        std::vector<int> freeOffsets;  // release filter: +N then -N
        size_t freeIdx = 0;
        bool freeAllPass = true;
        std::vector<int> extremeOffsets;  // fast mode: +b then -a, together >= maxWidth wide
        size_t extremeIdx = 0;
        bool extremeAllPass = true;
        bool baseSeeded = false;
        SearchConfig cfg;
        WindowSearch search;
        Current(int g, int t, int e, bool c, int cp, const SearchConfig& config)
            : group(g), refTick(t), endTick(e), capped(c), cpAt(cp), usingCp(cp >= 0), cfg(config), search(config) {}
    };

    bool isNoOp(int ui) const { return us_.units[ui].kind == UnitKind::NoOp; }
    int firstTick(int ui) const { return refFire_[us_.units[ui].groups.front()]; }
    int lastTick(int ui) const { return refFire_[us_.units[ui].groups.back()]; }
    bool reached(int ui) const {
        for (int gi : us_.units[ui].groups) {
            const int t = refFire_[gi];
            if (t < 0 || (refDeath_ >= 0 && t >= refDeath_)) return false;
        }
        return true;
    }

    void processReference(const TrialResult& r) {
        report_.referenceRan = true;
        report_.refTicks = r.ticksRun;
        report_.refDied = r.died;
        report_.refDeathTick = r.deathTick;
        report_.refDeathX = r.deathX;
        report_.refDeathNote = r.deathNote;
        report_.refCompleted = r.completed;
        report_.refTimedOut = r.timedOut;
        cpAts_.clear();
        for (int t : r.checkpointAt) cpAts_.insert(t);
        report_.cpCaptured = static_cast<int>(cpAts_.size());
        report_.usedCheckpoints = s_.useCheckpoints && !cpAts_.empty();
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

        // Which units to analyse.
        for (size_t ui = 0; ui < us_.units.size(); ++ui) {
            bool pick = true;
            if (s_.onlyAction > 0) {
                pick = false;
                for (int gi : us_.units[ui].groups)
                    for (int id : gs_.groups[gi].actions) if (id + 1 == s_.onlyAction) pick = true;
            }
            if (pick) selected_.push_back(static_cast<int>(ui));
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

    // Neighbour = previous/next UNIT of the SAME player that actually changes state.
    // Previous: the tick its LAST input fired on. Next: the tick its FIRST input fired on.
    int neighbourTick(int ui, int dir) const {
        const auto& list = us_.byPlayer[us_.units[ui].player];
        int at = -1;
        for (size_t i = 0; i < list.size(); ++i) if (list[i] == ui) at = static_cast<int>(i);
        for (int i = at + dir; i >= 0 && i < static_cast<int>(list.size()); i += dir) {
            if (isNoOp(list[i])) continue;
            if (reached(list[i])) return dir < 0 ? lastTick(list[i]) : firstTick(list[i]);
            if (dir > 0) return -1;  // unreached: nothing to collide with
        }
        return -1;
    }

    void noteCheckpointOk() {
        if (!cur_ || !cur_->usingCp) return;
        cpFailStreak_ = 0;
        ++report_.cpOk;
        if (++cpOkCount_ >= 3) cpTrusted_ = true;
    }

    // Take over a finished input of an interrupted analysis (same number AND same x).
    bool takePrior(int gi) {
        const Unit& g = us_.units[gi];
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
            rep.refTick = firstTick(gi);
            rep.holdTicks = lastTick(gi) - firstTick(gi);
            ++report_.resumed;
            return true;
        }
        return false;
    }

    void beginGroup(int gi) {
        GroupReport& rep = report_.groups[gi];
        if (isNoOp(gi)) { rep.state = GroupState::Redundant; return; }
        if (takePrior(gi)) return;
        const Unit& unit = us_.units[gi];
        if (!reached(gi)) { rep.state = GroupState::Unreached; rep.refTick = firstTick(gi); return; }
        const int Tf = firstTick(gi);   // first input of the unit (the click)
        const int Tl = lastTick(gi);    // last input of the unit (the release, or the last release of a chain)
        rep.refTick = Tf;
        rep.holdTicks = Tl - Tf;

        const int last = static_cast<int>(xs_.size()) - 1;  // last tick whose x is known
        const int R = std::max(1, s_.radius);

        SearchConfig cfg;
        cfg.thorough = s_.thorough;
        cfg.probeBeyond = std::max(0, s_.probeBeyond);
        cfg.verify = s_.verify;
        cfg.maxWidth = s_.thorough ? 0 : std::max(0, s_.maxWidth);

        // ---- range: EVERY input of the unit moves by the same d, so the click must stay after the previous unit
        // and the release before the next one
        const int prevT = neighbourTick(gi, -1);  // tick the previous unit ended on
        const int nextT = neighbourTick(gi, +1);  // tick the next unit starts on
        const int minTick = (prevT >= 0) ? prevT + 1 : 0;
        const Limit minReason = (prevT >= 0) ? Limit::Neighbor : Limit::Start;
        int maxTick = last;
        Limit maxReason = Limit::RefEnd;
        if (nextT >= 0 && nextT - 1 < maxTick) { maxTick = nextT - 1; maxReason = Limit::Neighbor; }

        cfg.rangeLo = -R; cfg.limitLo = Limit::Radius;
        if (minTick - Tf > -R) { cfg.rangeLo = minTick - Tf; cfg.limitLo = minReason; }
        cfg.rangeHi = R; cfg.limitHi = Limit::Radius;
        if (maxTick - Tl < R) { cfg.rangeHi = maxTick - Tl; cfg.limitHi = maxReason; }
        cfg.rangeLo = std::min(cfg.rangeLo, 0);
        cfg.rangeHi = std::max(cfg.rangeHi, 0);

        // ---- the position-based macro can only place an input on a tick if x grows every tick (checked for every member)
        for (int d = 1; d <= cfg.rangeHi; ++d) {
            bool ok = true;
            for (int g : unit.groups) ok = ok && monotoneAt(refFire_[g] + d);
            if (!ok) { cfg.rangeHi = d - 1; cfg.limitHi = Limit::Plateau; break; }
        }
        for (int d = 0; d >= cfg.rangeLo; --d) {
            bool ok = true;
            for (int g : unit.groups) ok = ok && monotoneAt(refFire_[g] + d);
            if (!ok) { cfg.rangeLo = d + 1; cfg.limitLo = Limit::Plateau; break; }
        }
        if (cfg.rangeLo > 0 || cfg.rangeHi < 0) {  // not even the original ticks are unique
            rep.state = GroupState::Unmappable;
            return;
        }

        // ---- end tick: identical for every offset, so results are comparable.
        // Default: the next unit of the ORIGINAL macro: a click (press AND release) must survive until the next click.
        int end;
        bool capped = false;
        if (s_.horizonToEnd) {
            end = last;
        } else {
            end = Tl + std::max(0, s_.horizonMinTicks);
            const auto& list = us_.byPlayer[unit.player];
            int at = -1;
            for (size_t i = 0; i < list.size(); ++i) if (list[i] == gi) at = static_cast<int>(i);
            int seen = 0;
            for (int i = at + 1; i < static_cast<int>(list.size()) && seen < s_.horizonGroups; ++i) {
                if (isNoOp(list[i])) continue;
                ++seen;
                const int t = firstTick(list[i]);
                if (t < 0) break;
                end = std::max(end, t + std::max(0, s_.horizonMargin));
            }
            if (seen < s_.horizonGroups) end = std::max(end, Tl + kHorizonTail);  // fewer units left than asked for
        }
        end = std::max(end, Tl + cfg.rangeHi);  // the shifted release must have executed before the trial may end
        int cap = last;
        if (refDeath_ >= 0) cap = std::min(cap, refDeath_ - 1);
        if (end > cap) { end = cap; capped = true; }
        if (end < Tl) { end = Tl; capped = true; }

        rep.state = GroupState::Analyzed;
        rep.y = (Tf < static_cast<int>(ys_.size())) ? ys_[Tf] : 0.0;
        rep.endTick = end;
        rep.horizonCapped = capped;
        // The checkpoint taken at the previous input of the original macro, or the closest earlier one that exists.
        // A checkpoint with N ticks already run is valid for every candidate tick >= N; candidates are >= prevT + 1.
        // (No previous input: the checkpoint of the level start, N = 0.)
        int cpAt = -1;
        if (s_.useCheckpoints && !cpDisabled_) {
            auto it = cpAts_.upper_bound(prevT + 1);
            if (it != cpAts_.begin()) cpAt = *--it;
        }
        cur_ = std::make_unique<Current>(gi, Tf, end, capped, cpAt, cfg);
        if (s_.skipFreeReleases && unit.kind == UnitKind::Single && unit.single == GroupKind::Release) {
            const int n = std::max(1, s_.freeReleaseTicks);
            if (cfg.rangeHi >= n && cfg.rangeLo <= -n) cur_->freeOffsets = {n, -n};  // can only conclude if both are reachable
        }
        if (s_.fastMode) {
            if (cur_->usingCp && cpTrusted_) {  // calibrated: the checkpoint start has reproduced the original timing before
                cur_->search.submit(0, Verdict::Pass);
                cur_->baseSeeded = true;
            }
            if (s_.maxWidth > 1 && !s_.thorough) {
                const int b = s_.maxWidth / 2, a = s_.maxWidth - 1 - b;
                if (cfg.rangeHi >= b && cfg.rangeLo <= -a) cur_->extremeOffsets = {b, -a};
            }
        }
    }

    void finalizeGroup() {
        GroupReport& rep = report_.groups[cur_->group];
        rep.w = cur_->search.result();
        rep.refTick = cur_->refTick;
        rep.endTick = cur_->endTick;
        rep.horizonCapped = cur_->capped;
        rep.usedCheckpoint = cur_->cpAt >= 0 && !cur_->fellBack;
        rep.cpFallback = cur_->fellBack;
        if (cur_->usingCp && !cur_->baseSeeded) noteCheckpointOk();
        cur_.reset();
    }

    TrialSpec buildTrial(int off) const {
        const Unit& u = us_.units[cur_->group];
        TrialSpec t;
        t.actions = macro_.actions;
        if (off != 0) {
            // every input of the unit moves by the SAME number of ticks: each goes to the x the player had at (its own tick + off)
            for (int gi : u.groups) {
                const double x = xs_[refFire_[gi] + off];
                for (int id : gs_.groups[gi].actions) t.actions[id].x = x;
            }
            std::stable_sort(t.actions.begin(), t.actions.end(),
                             [](const MacroAction& a, const MacroAction& b) { return a.x < b.x; });
        }
        t.endTick = cur_->endTick;
        t.maxTick = cur_->endTick + 16;
        t.startAt = cur_->usingCp ? cur_->cpAt : -1;
        t.recordX = false;
        return t;
    }

    Verdict classify(const TrialResult& r) const {
        const Unit& u = us_.units[cur_->group];
        const int end = cur_->endTick;
        bool anyMissing = false, anyDrift = false;
        for (int gi : u.groups) {
            int pos = -1;
            for (size_t i = 0; i < pendingSpec_->actions.size(); ++i) {
                if (pendingSpec_->actions[i].id == gs_.groups[gi].actions[0]) { pos = static_cast<int>(i); break; }
            }
            const int expect = refFire_[gi] + pendingOffset_;
            const int fired = (pos >= 0 && pos < static_cast<int>(r.firedTick.size())) ? r.firedTick[pos] : -1;
            if (fired < 0) anyMissing = true;
            else if (fired != expect) anyDrift = true;  // x->tick mapping drifted (speed change, other path)
        }
        if (anyMissing) return r.died ? Verdict::Fail : Verdict::Invalid;
        if (anyDrift) return Verdict::Invalid;
        if (r.died && r.deathTick <= end) return Verdict::Fail;
        if (!r.completed && r.ticksRun <= end) return Verdict::Invalid;  // stopped before the end tick
        return Verdict::Pass;
    }

    MacroFile macro_;
    AnalyzerSettings s_;
    GroupSet gs_;
    UnitSet us_;
    Report report_;
    Stage stage_ = Stage::NeedReference;
    std::vector<PriorRow> prior_;
    std::set<int> cpAts_;
    bool cpDisabled_ = false;
    bool cpTrusted_ = false;
    int cpFailStreak_ = 0;
    int cpOkCount_ = 0;
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
