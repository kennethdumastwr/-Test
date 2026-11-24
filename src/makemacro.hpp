#pragma once
// Turning the measured windows into a NEW macro: every analysed click is moved to a chosen tick inside its window.
// Pure C++ (tested on a PC).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "macro.hpp"

namespace fw {

enum class PickMode { Center = 0, Earliest = 1, Latest = 2 };

inline const char* pickName(PickMode m) {
    switch (m) {
        case PickMode::Center: return "center";
        case PickMode::Earliest: return "earliest";
        case PickMode::Latest: return "latest";
    }
    return "center";
}

inline PickMode pickFromInt(int v) { return v == 1 ? PickMode::Earliest : (v == 2 ? PickMode::Latest : PickMode::Center); }

// Offset (in ticks, relative to the recorded click) to use for a measured window [lo, hi].
// Center = the middle of the window; when two ticks are equally central the one closer to the original timing wins.
inline int pickOffset(int lo, int hi, PickMode mode) {
    if (mode == PickMode::Earliest) return lo;
    if (mode == PickMode::Latest) return hi;
    const double c = (lo + hi) / 2.0;
    const int a = static_cast<int>(std::floor(c)), b = static_cast<int>(std::ceil(c));
    return std::abs(a) <= std::abs(b) ? a : b;
}

// One row per action id: x as recorded, and x with every click at the center / earliest / latest tick of its window.
using PickRow = std::array<double, 4>;  // [0] original, [1] center, [2] earliest, [3] latest

inline std::string formatPickTable(const std::vector<PickRow>& rows) {
    std::string out = "id,x_original,x_center,x_earliest,x_latest\n";
    char buf[256];
    for (size_t i = 0; i < rows.size(); ++i) {
        std::snprintf(buf, sizeof buf, "%zu,%.17g,%.17g,%.17g,%.17g\n", i, rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
        out += buf;
    }
    return out;
}

inline bool parsePickTable(const std::string& text, std::vector<PickRow>& rows) {
    rows.clear();
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line.rfind("id,", 0) == 0) continue;
        std::vector<std::string> f;
        std::string cur;
        std::istringstream ls(line);
        while (std::getline(ls, cur, ',')) f.push_back(cur);
        if (f.size() != 5) return false;
        PickRow r;
        for (int k = 0; k < 4; ++k) r[k] = std::atof(f[k + 1].c_str());
        if (static_cast<size_t>(std::atoi(f[0].c_str())) != rows.size()) return false;  // ids must be 0,1,2...
        rows.push_back(r);
    }
    return !rows.empty();
}

// The macro's actions with x taken from the chosen column (sorted by x like every macro).
inline bool applyPicks(const MacroFile& m, const std::vector<PickRow>& rows, PickMode mode, std::vector<MacroAction>& out) {
    if (rows.size() != m.actions.size()) return false;  // the macro changed since the analysis
    out = m.actions;
    const int col = mode == PickMode::Center ? 1 : (mode == PickMode::Earliest ? 2 : 3);
    for (size_t i = 0; i < out.size(); ++i) {
        if (std::fabs(rows[i][0] - m.actions[i].x) > 1e-3) return false;  // a different macro
        out[i].x = rows[i][col];
    }
    std::stable_sort(out.begin(), out.end(), [](const MacroAction& a, const MacroAction& b) { return a.x < b.x; });
    return true;
}

// percent-macro v2 text. The percent column is cosmetic (playback ignores it) but is kept consistent: moved by the x change.
inline std::string formatMacroText(const MacroFile& orig, const std::vector<MacroAction>& acts, const std::string& note) {
    std::vector<double> slopes;  // x units per percent, estimated from the original file
    for (size_t i = 1; i < orig.actions.size(); ++i) {
        const double dp = orig.actions[i].percent - orig.actions[i - 1].percent;
        const double dx = orig.actions[i].x - orig.actions[i - 1].x;
        if (dp > 0.05 && dx > 0.0) slopes.push_back(dx / dp);
    }
    double unitsPerPercent = 0.0;
    if (!slopes.empty()) {
        std::sort(slopes.begin(), slopes.end());
        unitsPerPercent = slopes[slopes.size() / 2];
    }
    std::string out = "# percent-macro v2: x percent down p2 (25 decimals)\n";
    if (!orig.levelName.empty()) out += "# level " + orig.levelName + "\n";
    if (!note.empty()) out += "# " + note + "\n";
    char buf[512];
    for (const MacroAction& a : acts) {
        const MacroAction& o = orig.actions[a.id];
        double percent = o.percent;
        if (unitsPerPercent > 0.0) percent += (a.x - o.x) / unitsPerPercent;
        std::snprintf(buf, sizeof buf, "%.25f %.25f %d %d\n", a.x, percent, a.down ? 1 : 0, a.p2 ? 1 : 0);
        out += buf;
    }
    return out;
}

}  // namespace fw
