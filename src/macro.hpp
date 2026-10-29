#pragma once
// Pure C++ (no Geode includes) so it can be unit-tested on a PC.
//
// percent-macro v2 line format:  x percent down p2
//   x       player X position when the input happened  (what playback really uses)
//   percent level percent at that moment               (ignored)
//   down    1 = press, 0 = release
//   p2      0 = player 1, 1 = player 2

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fw {

struct MacroAction {
    double x = 0.0;
    double percent = 0.0;
    bool down = false;
    bool p2 = false;
    int line = 0;  // 1-based line in the source file (for error messages)
    int id = 0;    // 0-based position after sorting; stable identity of the action
};

struct ParseIssue {
    int line = 0;
    std::string message;
};

struct MacroFile {
    std::string levelName;
    std::string formatLine;
    std::vector<MacroAction> actions;  // sorted by x (stable)
    std::vector<ParseIssue> errors;    // any error => analysis refuses to run
    std::vector<std::string> warnings;
    int softHeaderLines = 0;           // header lines that had no leading '#'
    bool wasUnsorted = false;
    int inversions = 0;

    bool ok() const { return errors.empty() && !actions.empty(); }
};

namespace detail {

inline bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; }

inline std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && isSpace(s[a])) ++a;
    while (b > a && isSpace(s[b - 1])) --b;
    return std::string(s.substr(a, b - a));
}

inline std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isSpace(s[i])) ++i;
        size_t j = i;
        while (j < s.size() && !isSpace(s[j])) ++j;
        if (j > i) out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

// Locale-independent strict number parse ("1,5" must NOT silently become 1).
inline bool parseDouble(const std::string& tok, double& out) {
    if (tok.empty()) return false;
    std::istringstream ss(tok);
    ss.imbue(std::locale::classic());
    double v = 0.0;
    ss >> v;
    if (ss.fail()) return false;
    ss >> std::ws;
    if (!ss.eof()) return false;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

inline bool startsWithWord(const std::string& s, const char* word) {
    size_t n = 0;
    while (word[n]) ++n;
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        char a = s[i], b = word[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != b) return false;
    }
    return s.size() == n || isSpace(s[n]);
}

inline void readHeader(const std::string& body, MacroFile& m) {
    if (startsWithWord(body, "level")) {
        m.levelName = trim(std::string_view(body).substr(5));
    } else if (body.rfind("percent-macro", 0) == 0) {
        m.formatLine = body;
    }
}

}  // namespace detail

// Parse a macro file. Never throws. Lines are trimmed, '#' lines are headers,
// everything else must be exactly 4 numbers. Header lines whose '#' got lost
// (copy/paste through markdown) are accepted as long as they appear before the
// first data line and do not start with a number.
inline MacroFile parseMacro(std::string_view text) {
    using namespace detail;
    MacroFile m;
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.remove_prefix(3);  // UTF-8 BOM
    }

    int lineNo = 0;
    size_t pos = 0;
    bool seenData = false;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view raw = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;
        ++lineNo;

        std::string line = trim(raw);
        if (line.empty()) continue;

        if (line[0] == '#') {
            readHeader(trim(std::string_view(line).substr(1)), m);
            continue;
        }

        std::vector<std::string> tok = splitWs(line);
        double first = 0.0;
        const bool looksLikeData = parseDouble(tok[0], first);

        if (!looksLikeData && !seenData) {
            readHeader(line, m);
            ++m.softHeaderLines;
            continue;
        }
        if (tok.size() != 4) {
            m.errors.push_back({lineNo, "expected exactly 4 values, found " + std::to_string(tok.size())});
            continue;
        }

        double v[4];
        bool good = true;
        for (int i = 0; i < 4; ++i) {
            if (!parseDouble(tok[i], v[i])) {
                m.errors.push_back({lineNo, "value " + std::to_string(i + 1) + " is not a number: '" + tok[i] + "'"});
                good = false;
                break;
            }
        }
        if (!good) continue;
        if (v[0] < 0.0) {
            m.errors.push_back({lineNo, "negative x"});
            continue;
        }
        if (v[2] != 0.0 && v[2] != 1.0) {
            m.errors.push_back({lineNo, "down must be 0 or 1"});
            continue;
        }
        if (v[3] != 0.0 && v[3] != 1.0) {
            m.errors.push_back({lineNo, "p2 must be 0 or 1"});
            continue;
        }

        seenData = true;
        MacroAction a;
        a.x = v[0];
        a.percent = v[1];
        a.down = v[2] == 1.0;
        a.p2 = v[3] == 1.0;
        a.line = lineNo;
        m.actions.push_back(a);
    }

    if (m.actions.empty() && m.errors.empty()) m.errors.push_back({0, "no actions found"});

    for (size_t i = 1; i < m.actions.size(); ++i) {
        if (m.actions[i].x < m.actions[i - 1].x) ++m.inversions;
    }
    if (m.inversions > 0) {
        m.wasUnsorted = true;
        m.warnings.push_back("file was not sorted by x (" + std::to_string(m.inversions) +
                             " out-of-order lines); sorted in memory, file untouched");
        std::stable_sort(m.actions.begin(), m.actions.end(),
                         [](const MacroAction& a, const MacroAction& b) { return a.x < b.x; });
    }
    for (size_t i = 0; i < m.actions.size(); ++i) m.actions[i].id = static_cast<int>(i);
    return m;
}

// ---------------------------------------------------------------------------
// Grouping: inputs of the same player at the same x are ONE event.
// (release + press at the same x = a "tap"; analysing them one by one would
// create sequences the player never performed.)
// ---------------------------------------------------------------------------

enum class GroupKind { Press, Release, Tap, Flick, NoOp, Multi };

inline const char* kindName(GroupKind k) {
    switch (k) {
        case GroupKind::Press: return "press";
        case GroupKind::Release: return "release";
        case GroupKind::Tap: return "tap(R+P)";
        case GroupKind::Flick: return "flick(P+R)";
        case GroupKind::NoOp: return "no-op";
        case GroupKind::Multi: return "multi";
    }
    return "?";
}

struct Group {
    int player = 0;                 // 0 = P1, 1 = P2
    double x = 0.0;                 // x of the first action
    std::vector<int> actions;       // action ids, file order
    std::vector<bool> redundant;    // parallel to `actions`: input that changes nothing
    GroupKind kind = GroupKind::NoOp;
    int number = 0;                 // 1-based action number of the first action
};

struct GroupSet {
    std::vector<Group> groups;      // ordered by first action
    std::vector<int> byPlayer[2];   // indices into `groups`, ordered by x
    int redundantActions = 0;
};

inline GroupSet buildGroups(const std::vector<MacroAction>& acts, double eps = 1e-3) {
    GroupSet gs;
    bool held[2] = {false, false};
    int last[2] = {-1, -1};

    for (const MacroAction& a : acts) {
        const int p = a.p2 ? 1 : 0;
        const bool redundant = (a.down == held[p]);  // press while held / release while released
        held[p] = a.down;
        if (redundant) ++gs.redundantActions;

        if (last[p] >= 0 && a.x - gs.groups[last[p]].x <= eps) {
            Group& g = gs.groups[last[p]];
            g.actions.push_back(a.id);
            g.redundant.push_back(redundant);
        } else {
            Group g;
            g.player = p;
            g.x = a.x;
            g.number = a.id + 1;
            g.actions.push_back(a.id);
            g.redundant.push_back(redundant);
            gs.groups.push_back(std::move(g));
            last[p] = static_cast<int>(gs.groups.size()) - 1;
            gs.byPlayer[p].push_back(last[p]);
        }
    }

    for (Group& g : gs.groups) {
        std::vector<bool> eff;  // direction of every input that actually changes state
        for (size_t i = 0; i < g.actions.size(); ++i) {
            if (!g.redundant[i]) eff.push_back(acts[g.actions[i]].down);
        }
        if (eff.empty()) g.kind = GroupKind::NoOp;
        else if (eff.size() == 1) g.kind = eff[0] ? GroupKind::Press : GroupKind::Release;
        else if (eff.size() == 2 && !eff[0] && eff[1]) g.kind = GroupKind::Tap;
        else if (eff.size() == 2 && eff[0] && !eff[1]) g.kind = GroupKind::Flick;
        else g.kind = GroupKind::Multi;
    }
    return gs;
}

}  // namespace fw
