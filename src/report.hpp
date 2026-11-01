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

// Word-wrap to `width` characters; continuation lines are indented by two spaces. width <= 0 = no wrapping.
inline std::vector<std::string> wrapLine(const std::string& text, int width) {
    std::vector<std::string> out;
    if (width <= 0 || static_cast<int>(text.size()) <= width) { out.push_back(text); return out; }
    std::string cur;
    size_t i = 0;
    while (i < text.size()) {
        size_t j = text.find(' ', i);
        std::string word = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = (j == std::string::npos) ? text.size() : j + 1;
        if (word.empty()) continue;
        const std::string sep = cur.empty() ? "" : " ";
        if (!cur.empty() && static_cast<int>(cur.size() + sep.size() + word.size()) > width) {
            out.push_back(cur);
            cur = "  " + word;
        } else {
            cur += sep + word;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct ReportStyle {
    bool tags = false;  // GD rich-text colour tags (<cg> <cr> <co>)
    double tps = 240.0;
    int wrap = 0;       // wrap lines at this many characters (0 = off)
};

// Append `text` as one or more lines; every line gets its own colour tag so wrapping never breaks a tag.
inline void addLine(std::string& out, const ReportStyle& st, const char* tag, const std::string& text) {
    for (const std::string& l : wrapLine(text, st.wrap)) {
        if (st.tags && tag) out += std::string("<") + tag + ">" + l + "</c>\n";
        else out += l + "\n";
    }
}

// "[-2,+2] 5F"  /  "[-8R,+3] 12F"   (letter after a number = edge was NOT a FAIL, see legend)
inline std::string windowText(const WindowResult& w) {
    if (w.capped) return std::to_string(w.capWidth) + "+F";
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
    if (g.cpFallback) f.push_back("CP-FALLBACK");
    if (g.fastAssumed) f.push_back("FAST-ASSUMED");
    if (f.empty()) return "stable";
    std::string s;
    for (size_t i = 0; i < f.size(); ++i) s += (i ? " " : "") + f[i];
    return s;
}

inline std::string numbersText(const Group& g) {
    if (g.actions.size() == 1) return "#" + std::to_string(g.actions[0] + 1);
    return "#" + std::to_string(g.actions.front() + 1) + "-" + std::to_string(g.actions.back() + 1);
}

// Returns the text of one input's line and the colour tag to use (nullptr = none).
inline std::string formatLine(const GroupSet& gs, const GroupReport& g, const ReportStyle& st, const char*& tag) {
    const Group& grp = gs.groups[g.group];
    tag = nullptr;
    std::string head = numbersText(grp) + (grp.player ? " P2 " : " ") + kindName(g.kind) + " x=" + fmt("%.3f", g.x);
    switch (g.state) {
        case GroupState::NotSelected: return head + "  (not selected)";
        case GroupState::Redundant: return head + "  no state change - skipped";
        case GroupState::Free: return head + "  free release (survives the +- probe) - not analysed";
        case GroupState::Unreached: tag = "cr"; return head + "  never reached in the reference run";
        case GroupState::Unmappable: tag = "co"; return head + "  x not growing here, cannot place on ticks";
        case GroupState::Analyzed: break;
    }
    if (g.w.baseFailed) { tag = "cr"; return head + "  original timing does not pass"; }
    const double ms = g.w.width() * 1000.0 / st.tps;
    std::string line = head + "  " + windowText(g.w) + " " + fmt("%.1f", ms) + (g.w.capped ? "ms+  " : "ms  ") + flagsText(g);
    const bool clean = !g.w.unstable && !g.w.drift && !g.w.island && g.w.clipLo == Limit::None && (g.w.clipHi == Limit::None || g.w.capped);
    tag = clean ? "cg" : ((g.w.unstable || g.w.drift) ? "cr" : "co");
    return line;
}

inline std::string formatReport(const Analyzer& a, const ReportStyle& st, const std::string& macroName) {
    const Report& r = a.report();
    const MacroFile& m = a.macro();
    std::string out;
    addLine(out, st, nullptr, "Frame windows @" + fmt("%.0f", st.tps) + " TPS (1F = " + fmt("%.2f", 1000.0 / st.tps) + "ms)");
    addLine(out, st, nullptr, "macro: " + macroName + (m.levelName.empty() ? "" : "  level: " + m.levelName));
    addLine(out, st, nullptr, "actions: " + std::to_string(m.actions.size()) + "  inputs: " + std::to_string(a.groups().groups.size()) +
                                  "  trials: " + std::to_string(r.totalTrials));
    if (r.cancelled) addLine(out, st, "cr", "CANCELLED: " + r.cancelReason + ". Partial results are saved: press Resume in the menu to continue.");
    if (!r.note.empty()) addLine(out, st, "cr", r.note);
    if (r.referenceRan) {
        if (r.refDied) {
            addLine(out, st, "cr",
                    "WARNING: the untouched macro dies at tick " + std::to_string(r.refDeathTick) + " (x=" + fmt("%.2f", r.refDeathX) +
                        "). Inputs after that are unreachable.");
            if (!r.refDeathNote.empty()) addLine(out, st, "cr", "killed by: " + r.refDeathNote);
        } else if (r.refCompleted) addLine(out, st, nullptr, "reference run: completed the level");
        else if (r.refTimedOut) addLine(out, st, "co", "reference run hit the time limit");
        else addLine(out, st, nullptr, "reference run: survived " + std::to_string(r.refTicks) + " ticks");
        addLine(out, st, nullptr, std::string("checkpoints: ") + (r.usedCheckpoints ? "captured " + std::to_string(r.cpCaptured) + ", ok on " + std::to_string(r.cpOk) + " inputs, fell back on " + std::to_string(r.cpFallbacks) + (r.cpDisabled ? " (given up: the restore does not reproduce the original timing)" : "") : "not used (every trial restarts from the level start)"));
    }
    for (const std::string& w : m.warnings) addLine(out, st, "co", "note: " + w);
    {
        int analyzed = 0, unreached = 0, unmappable = 0, noop = 0, basefail = 0, drift = 0, unstable = 0, clipped = 0, freeRel = 0;
        for (const GroupReport& g : r.groups) {
            switch (g.state) {
                case GroupState::Analyzed:
                    ++analyzed;
                    basefail += g.w.baseFailed; drift += g.w.drift; unstable += g.w.unstable;
                    clipped += (g.w.clipLo != Limit::None || g.w.clipHi != Limit::None);
                    break;
                case GroupState::Unreached: ++unreached; break;
                case GroupState::Free: ++freeRel; break;
                case GroupState::Unmappable: ++unmappable; break;
                case GroupState::Redundant: ++noop; break;
                default: break;
            }
        }
        std::string sum = "summary: " + std::to_string(analyzed) + " analysed";
        if (unreached) sum += ", " + std::to_string(unreached) + " unreached";
        if (basefail) sum += ", " + std::to_string(basefail) + " base-fail";
        if (drift) sum += ", " + std::to_string(drift) + " drift";
        if (unstable) sum += ", " + std::to_string(unstable) + " unstable";
        if (unmappable) sum += ", " + std::to_string(unmappable) + " unmappable";
        if (clipped) sum += ", " + std::to_string(clipped) + " clipped";
        if (freeRel) sum += ", " + std::to_string(freeRel) + " free releases";
        if (noop) sum += ", " + std::to_string(noop) + " no-op";
        if (r.resumed) sum += ", " + std::to_string(r.resumed) + " resumed";
        addLine(out, st, (unreached || basefail || drift || unstable || unmappable) ? "co" : "cg", sum);
    }
    addLine(out, st, nullptr, "Edge letters: R radius, N neighbour input, S start, E end of reference, P x flat. A letter means no FAIL was found before that limit. 10+ = at least that wide, not measured further.");
    out += "\n";
    for (const GroupReport& g : r.groups) {
        if (g.state == GroupState::NotSelected) continue;
        const char* tag = nullptr;
        std::string line = formatLine(a.groups(), g, st, tag);
        addLine(out, st, tag, line);
    }
    return out;
}

inline std::string limitCsv(Limit l) { return l == Limit::None ? std::string() : std::string(1, limitChar(l)); }

inline std::string formatCsv(const Analyzer& a, double tps) {
    std::string out;
    if (!a.macro().levelName.empty()) out += "# level " + a.macro().levelName + "\n";
    {
        int total = 0;
        for (const Group& g : a.groups().groups) total += g.kind != GroupKind::NoOp;
        out += std::string("# complete ") + (a.report().complete ? "1" : "0") + "\n# total " + std::to_string(total) + "\n";
    }
    out += "numbers,player,kind,x,y,state,lo,hi,width_ticks,width_ms,clip_lo,clip_hi,flags,trials\n";
    for (const GroupReport& g : a.report().groups) {
        if (g.state == GroupState::NotSelected) continue;
        const Group& grp = a.groups().groups[g.group];
        const char* st = g.state == GroupState::Analyzed ? "analyzed" : g.state == GroupState::Redundant ? "no-op"
                         : g.state == GroupState::Free ? "free" : g.state == GroupState::Unreached ? "unreached" : "unmappable";
        out += numbersText(grp).substr(1) + "," + (grp.player ? "P2" : "P1") + "," + kindName(g.kind) + "," + fmt("%.6f", g.x) + "," + fmt("%.3f", g.y) + "," + st + ",";
        if (g.state == GroupState::Analyzed && !g.w.baseFailed) {
            out += std::to_string(g.w.lo) + "," + std::to_string(g.w.hi) + "," + std::to_string(g.w.width()) + "," +
                   fmt("%.2f", g.w.width() * 1000.0 / tps) + "," + limitCsv(g.w.clipLo) + "," + limitCsv(g.w.clipHi) + "," +
                   flagsText(g) + "," + std::to_string(g.w.trials);
        } else {
            out += ",,,,,," + (g.state == GroupState::Free ? std::string("free") : g.w.baseFailed ? std::string("BASE-FAIL") : std::string()) + ",";
        }
        out += "\n";
    }
    return out;
}

}  // namespace fw
