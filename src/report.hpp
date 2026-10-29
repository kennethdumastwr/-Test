#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "analyzer.hpp"

namespace fw {

inline std::string fmt(const char* f, double a) {
    char buf[64];
    std::snprintf(buf, sizeof buf, f, a);
    return buf;
}

// "[-2,+2] 5F"  /  "[-8R,+3] 12F"   (letter after a number = edge was NOT a FAIL, see legend)
inline std::string windowText(const WindowResult& w) {
    auto side = [](int v, Limit l, bool plus) {
        std::string s = (plus && v >= 0) ? "+" : "";
        s += std::to_string(v);
        if (l != Limit::None) s += limitChar(l);
        return s;
    };
    return "[" + side(w.lo, w.clipLo, false) + "," + side(w.hi, w.clipHi, true) + "] " + std::to_string(w.width()) + "F";
}

inline std::string flagsText(const GroupReport& g) {
    std::vector<std::string> f;
    if (g.w.baseFailed) f.push_back("BASE-FAIL");
    if (g.w.unstable) f.push_back("UNSTABLE");
    if (g.w.drift) f.push_back("DRIFT");
    if (g.w.island) f.push_back("ISLAND");
    if (g.horizonCapped) f.push_back("HORIZON-CAPPED");
    if (f.empty()) return "stable";
    std::string s;
    for (size_t i = 0; i < f.size(); ++i) s += (i ? " " : "") + f[i];
    return s;
}

inline std::string numbersText(const MacroFile& m, const Group& g) {
    (void)m;
    if (g.actions.size() == 1) return "#" + std::to_string(g.actions[0] + 1);
    return "#" + std::to_string(g.actions.front() + 1) + "-" + std::to_string(g.actions.back() + 1);
}

struct ReportStyle {
    bool tags = false;  // GD rich-text colour tags (<cg> <cr> <co> <cy>)
    double tps = 240.0;
};

inline std::string colorWrap(bool tags, const char* tag, const std::string& s) {
    if (!tags) return s;
    return std::string("<") + tag + ">" + s + "</c>";
}

inline std::string formatLine(const MacroFile& m, const GroupSet& gs, const GroupReport& g, const ReportStyle& st) {
    const Group& grp = gs.groups[g.group];
    std::string head = numbersText(m, grp) + (grp.player ? " P2 " : " ") + kindName(g.kind) + " x=" + fmt("%.3f", g.x);
    switch (g.state) {
        case GroupState::NotSelected: return head + "  (not selected)";
        case GroupState::Redundant: return head + "  no state change (input repeats the current state) - skipped";
        case GroupState::Unreached: return colorWrap(st.tags, "cr", head + "  never reached in the reference run");
        case GroupState::Unmappable: return colorWrap(st.tags, "co", head + "  x does not grow every tick here - cannot place on single ticks");
        case GroupState::Analyzed: break;
    }
    if (g.w.baseFailed) return colorWrap(st.tags, "cr", head + "  original timing does not pass - not reproducible");
    const double ms = g.w.width() * 1000.0 / st.tps;
    std::string line = head + "  " + windowText(g.w) + " " + fmt("%.1f", ms) + "ms  " + flagsText(g);
    bool clean = !g.w.unstable && !g.w.drift && !g.w.island && g.w.clipLo == Limit::None && g.w.clipHi == Limit::None;
    if (!clean) return colorWrap(st.tags, (g.w.unstable || g.w.drift) ? "cr" : "co", line);
    return colorWrap(st.tags, "cg", line);
}

inline std::string formatReport(const Analyzer& a, const ReportStyle& st, const std::string& macroName) {
    const Report& r = a.report();
    const MacroFile& m = a.macro();
    std::string out;
    out += "Frame windows @" + fmt("%.0f", st.tps) + " TPS  (1F = " + fmt("%.2f", 1000.0 / st.tps) + "ms)\n";
    out += "macro: " + macroName + (m.levelName.empty() ? "" : "   level: " + m.levelName) + "\n";
    out += "actions: " + std::to_string(m.actions.size()) + "   inputs (same-x grouped): " + std::to_string(a.groups().groups.size()) +
           "   trials: " + std::to_string(r.totalTrials) + "\n";
    if (r.cancelled) out += colorWrap(st.tags, "cr", "CANCELLED: " + r.cancelReason + " (partial results)") + "\n";
    if (!r.note.empty()) out += colorWrap(st.tags, "cr", r.note) + "\n";
    if (r.referenceRan) {
        if (r.refDied)
            out += colorWrap(st.tags, "cr",
                             "WARNING: the untouched macro dies at tick " + std::to_string(r.refDeathTick) + " (x=" + fmt("%.2f", r.refDeathX) +
                                 "). Inputs after that are unreachable; the macro does not replay cleanly from the start.") + "\n";
        else if (r.refCompleted) out += "reference run: completed the level\n";
        else if (r.refTimedOut) out += colorWrap(st.tags, "co", "reference run hit the time limit") + "\n";
        else out += "reference run: survived " + std::to_string(r.refTicks) + " ticks\n";
    }
    for (const std::string& w : m.warnings) out += colorWrap(st.tags, "co", "note: " + w) + "\n";
    out += "edge letters: R=scan radius N=neighbour input S=level start E=end of reference P=x not growing\n";
    out += "(a letter means the window may be wider: no FAIL was found before that limit)\n\n";
    for (const GroupReport& g : r.groups) {
        if (g.state == GroupState::NotSelected) continue;
        out += formatLine(m, a.groups(), g, st) + "\n";
    }
    return out;
}

inline std::string limitCsv(Limit l) { return l == Limit::None ? std::string() : std::string(1, limitChar(l)); }

inline std::string formatCsv(const Analyzer& a, double tps) {
    std::string out = "numbers,player,kind,x,state,lo,hi,width_ticks,width_ms,clip_lo,clip_hi,flags,trials\n";
    const MacroFile& m = a.macro();
    for (const GroupReport& g : a.report().groups) {
        if (g.state == GroupState::NotSelected) continue;
        const Group& grp = a.groups().groups[g.group];
        const char* st = g.state == GroupState::Analyzed ? "analyzed" : g.state == GroupState::Redundant ? "no-op"
                         : g.state == GroupState::Unreached ? "unreached" : "unmappable";
        out += numbersText(m, grp) + "," + (grp.player ? "P2" : "P1") + "," + kindName(g.kind) + "," + fmt("%.6f", g.x) + "," + st + ",";
        if (g.state == GroupState::Analyzed && !g.w.baseFailed) {
            out += std::to_string(g.w.lo) + "," + std::to_string(g.w.hi) + "," + std::to_string(g.w.width()) + "," +
                   fmt("%.2f", g.w.width() * 1000.0 / tps) + "," + limitCsv(g.w.clipLo) + "," +
                   limitCsv(g.w.clipHi) + "," + flagsText(g) + "," + std::to_string(g.w.trials);
        } else {
            out += ",,,,,," + (g.w.baseFailed ? std::string("BASE-FAIL") : std::string()) + ",";
        }
        out += "\n";
    }
    return out;
}

}  // namespace fw
