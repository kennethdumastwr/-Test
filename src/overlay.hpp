#pragma once
// Saved results -> on-screen markers. Pure C++ (tested on a PC).
// The .windows.csv written after an analysis is the persistent store.

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace fw {

struct OverlayItem {
    double x = 0.0, y = 0.0;
    int width = 0;          // window width in ticks
    bool clipped = false;   // an edge hit a limit: real window may be wider
    int lo = 0, hi = 0;
};

// One saved row, used to RESUME an interrupted analysis.
struct PriorRow {
    int number = 0;         // 1-based number of the first action of the input
    double x = 0.0, y = 0.0;
    std::string state;      // analyzed | free | no-op | unreached | unmappable
    int lo = 0, hi = 0, width = 0;
    char clipLo = ' ', clipHi = ' ';
    std::string flags;
};

struct SavedResults {
    std::string level;      // level name stored in the file ("" = unknown)
    std::string macro;
    bool oldFormat = false;  // written by a version without the y column: analyse again
    bool complete = true;    // "# complete 0" = the analysis was interrupted (older files count as complete)
    int total = 0;           // "# total N" = inputs that change state
    int done = 0;            // analysed + free rows
    std::vector<OverlayItem> items;
    std::vector<PriorRow> rows;
};

inline std::vector<std::string> splitCsv(const std::string& line) {
    std::vector<std::string> f;
    std::string cur;
    std::istringstream ss(line);
    while (std::getline(ss, cur, ',')) f.push_back(cur);
    if (!line.empty() && line.back() == ',') f.push_back("");
    return f;
}

// Columns: numbers,player,kind,x,y,state,lo,hi,width_ticks,width_ms,clip_lo,clip_hi,flags,trials
inline SavedResults parseResultsCsv(const std::string& text) {
    SavedResults r;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (line.rfind("# level ", 0) == 0) r.level = line.substr(8);
            else if (line.rfind("# complete ", 0) == 0) r.complete = line.substr(11) == "1";
            else if (line.rfind("# total ", 0) == 0) r.total = std::atoi(line.c_str() + 8);
            else if (line.rfind("# macro ", 0) == 0) r.macro = line.substr(8);
            continue;
        }
        if (line.rfind("numbers,", 0) == 0) continue;
        auto f = splitCsv(line);
        if (f.size() >= 5 && f[4] == "analyzed") { r.oldFormat = true; continue; }
        if (f.size() < 12) continue;
        {
            PriorRow pr;
            pr.number = std::atoi(f[0].c_str());
            pr.x = std::atof(f[3].c_str());
            pr.y = std::atof(f[4].c_str());
            pr.state = f[5];
            pr.lo = std::atoi(f[6].c_str());
            pr.hi = std::atoi(f[7].c_str());
            pr.width = std::atoi(f[8].c_str());
            pr.clipLo = f[10].empty() ? ' ' : f[10][0];
            pr.clipHi = f[11].empty() ? ' ' : f[11][0];
            pr.flags = f.size() > 12 ? f[12] : "";
            r.rows.push_back(pr);
            if (pr.state == "free") ++r.done;
        }
        if (f[5] != "analyzed" || f[8].empty()) continue;
        OverlayItem it;
        it.x = std::atof(f[3].c_str());
        it.y = std::atof(f[4].c_str());
        it.lo = std::atoi(f[6].c_str());
        it.hi = std::atoi(f[7].c_str());
        it.width = std::atoi(f[8].c_str());
        it.clipped = !f[10].empty() || !f[11].empty();
        if (it.width > 0) { r.items.push_back(it); ++r.done; }
    }
    return r;
}

struct Rgb { unsigned char r, g, b; };

// Same scale as the counter in your screenshot: 1 red, 2 orange, 3 yellow, 4 white, 5+ green
inline Rgb widthColor(int w) {
    switch (w) {
        case 1: return {255, 70, 70};
        case 2: return {255, 150, 40};
        case 3: return {255, 220, 40};
        case 4: return {255, 255, 255};
        default: return {110, 255, 110};
    }
}

inline int widthBucket(int w) { return w < 1 ? 1 : (w > 5 ? 5 : w); }

inline void histogram(const std::vector<OverlayItem>& items, int out[5]) {
    for (int i = 0; i < 5; ++i) out[i] = 0;
    for (const auto& it : items) ++out[widthBucket(it.width) - 1];
}

inline double averageWidth(const std::vector<OverlayItem>& items) {
    if (items.empty()) return 0.0;
    double sum = 0.0;
    for (const auto& it : items) sum += it.width;
    return sum / static_cast<double>(items.size());
}

// Pitch of the click sound: 5+ = normal, then -0.1 per frame of window down to 1 (0.6)
inline float pitchForWidth(int w) {
    if (w >= 5) return 1.0f;
    if (w < 1) w = 1;
    return 1.0f - 0.1f * static_cast<float>(5 - w);
}

inline std::string markerText(const OverlayItem& it) { return std::to_string(it.width) + (it.clipped ? "+" : ""); }

}  // namespace fw
