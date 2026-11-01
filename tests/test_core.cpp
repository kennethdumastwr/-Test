// Host tests for the pure-C++ core.  Build & run:  ./tests/run_tests.sh
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "../src/analyzer.hpp"
#include "../src/playback.hpp"
#include "../src/overlay.hpp"
#include "../src/report.hpp"

using namespace fw;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (cond) ++g_pass;                                                           \
        else { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define SECTION(name) std::printf("[%s]\n", name)

static std::string slurp(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ---------------------------------------------------------------- parser
static void testParser() {
    SECTION("parser: real macro (pasted, headers without '#')");
    std::string txt = slurp("tests/spamy_wVs.txt");
    CHECK(!txt.empty());
    MacroFile m = parseMacro(txt);
    CHECK(m.ok());
    CHECK(m.errors.empty());
    CHECK(m.actions.size() == 138);
    CHECK(m.levelName == "spamy_wVs");
    CHECK(m.softHeaderLines == 2);
    CHECK(!m.wasUnsorted);
    CHECK(m.actions[0].x == 0.0 && !m.actions[0].down);
    CHECK(m.actions[1].down && std::fabs(m.actions[1].x - 94.77227020263671875) < 1e-12);
    int presses = 0;
    for (auto& a : m.actions) presses += a.down;
    CHECK(presses == 66);

    SECTION("parser: same file with '#' headers");
    std::string withHash = "# percent-macro v2: x percent down p2 (25 decimals)\n# level spamy_wVs\n";
    withHash += txt.substr(txt.find("0.0000000000000000000000000"));
    MacroFile m2 = parseMacro(withHash);
    CHECK(m2.ok() && m2.actions.size() == 138 && m2.levelName == "spamy_wVs" && m2.softHeaderLines == 0);

    SECTION("parser: awkward input");
    MacroFile c = parseMacro("\xEF\xBB\xBF# level My Level Name\r\n\r\n  10.5\t0.1   1   0  \r\n20.5 0.2 0 0\r\n");
    CHECK(c.ok() && c.levelName == "My Level Name" && c.actions.size() == 2 && c.actions[0].down);
    MacroFile e1 = parseMacro("1 2 3\n");
    CHECK(!e1.ok());                                   // 3 values
    MacroFile e2 = parseMacro("1 2 1 0 9\n");
    CHECK(!e2.ok());                                   // 5 values
    MacroFile e3 = parseMacro("1,5 2 1 0\n");
    CHECK(!e3.ok());                                   // comma decimal must not become 1
    MacroFile e4 = parseMacro("-3 2 1 0\n");
    CHECK(!e4.ok());
    MacroFile e5 = parseMacro("3 2 2 0\n");
    CHECK(!e5.ok());
    MacroFile e6 = parseMacro("3 2 1 0\nabc def\n");
    CHECK(!e6.ok() && e6.errors[0].line == 2);         // junk after data starts is an error, not a header
    MacroFile e7 = parseMacro("# only a header\n");
    CHECK(!e7.ok());
    MacroFile e8 = parseMacro("1 2 nan 0\n");
    CHECK(!e8.ok());
    MacroFile u = parseMacro("30 0 0 0\n10 0 1 0\n20 0 0 0\n");
    CHECK(u.ok() && u.wasUnsorted && u.actions[0].x == 10 && u.actions[2].x == 30 && u.actions[1].id == 1);
    MacroFile s = parseMacro("10 0 0 0\n10 0 1 0\n5 0 1 0\n");  // stable: same-x order preserved
    CHECK(s.actions[1].x == 10 && !s.actions[1].down && s.actions[2].down);
}

// ---------------------------------------------------------------- grouping
static void testGroups() {
    SECTION("groups: real macro");
    MacroFile m = parseMacro(slurp("tests/spamy_wVs.txt"));
    GroupSet gs = buildGroups(m.actions);
    CHECK(gs.redundantActions == 6);                   // #1, #60, #77, #114, #127, #134 (independent python check)
    CHECK(gs.groups.size() == 137);                    // 138 actions, one same-x pair
    int taps = 0, noops = 0;
    for (auto& g : gs.groups) { taps += g.kind == GroupKind::Tap; noops += g.kind == GroupKind::NoOp; }
    CHECK(taps == 1);
    CHECK(noops == 6);
    const Group* tap = nullptr;
    for (auto& g : gs.groups) if (g.kind == GroupKind::Tap) tap = &g;
    CHECK(tap && tap->actions.size() == 2 && tap->actions[0] == 40 && tap->actions[1] == 41 && tap->number == 41);

    SECTION("groups: players are separate, redundant+press collapses to press");
    MacroFile p = parseMacro("10 0 0 0\n10 0 1 0\n10 0 1 1\n50 0 0 0\n50 0 0 1\n");
    GroupSet g2 = buildGroups(p.actions);
    CHECK(g2.groups.size() == 4);
    CHECK(g2.groups[0].kind == GroupKind::Press && g2.groups[0].redundant[0]);  // release was already released
    CHECK(g2.groups[1].player == 1 && g2.groups[1].kind == GroupKind::Press);
    CHECK(g2.byPlayer[0].size() == 2 && g2.byPlayer[1].size() == 2);
}

// ---------------------------------------------------------------- cursor
static void testCursor() {
    SECTION("cursor");
    std::vector<MacroAction> v(3);
    v[0].x = 100.0; v[0].down = true;
    v[1].x = 100.0; v[1].down = false;   // same x: must fire right after, same tick, in order
    v[2].x = 200.0; v[2].down = true; v[2].p2 = true;
    Cursor c;
    c.bind(&v, 0.001);
    std::vector<Fire> out;
    c.poll(99.9, out);  CHECK(out.empty());
    c.poll(99.9995, out); CHECK(out.size() == 2 && out[0].down && !out[1].down);   // within eps counts
    out.clear();
    c.poll(150.0, out); CHECK(out.empty());
    c.poll(5000.0, out); CHECK(out.size() == 1 && out[0].p2);                        // jump past: still fires once
    CHECK(c.done());
    c.reset(); out.clear();
    c.poll(99.998, out); CHECK(out.empty());                                         // 0.002 short > eps
}

// ---------------------------------------------------------------- window search
using Oracle = std::function<Verdict(int)>;
static WindowResult runSearch(const SearchConfig& cfg, Oracle o, std::vector<int>* order = nullptr) {
    WindowSearch s(cfg);
    int guard = 0;
    while (auto off = s.next()) {
        if (order) order->push_back(*off);
        s.submit(*off, o(*off));
        if (++guard > 1000) { std::printf("  runaway search\n"); std::abort(); }
    }
    return s.result();
}
static Oracle window(int lo, int hi) {
    return [=](int d) { return (d >= lo && d <= hi) ? Verdict::Pass : Verdict::Fail; };
}

static void testWindowSearch() {
    SearchConfig cfg;  // radius 8, verify on, probe 1
    SECTION("window: contiguous [-2,+2]");
    {
        auto r = runSearch(cfg, window(-2, 2));
        CHECK(!r.baseFailed && r.lo == -2 && r.hi == 2 && r.width() == 5);
        CHECK(r.clipLo == Limit::None && r.clipHi == Limit::None && !r.island && !r.unstable);
        std::printf("  trials with verify+probe: %d (brute force over +-8 = 17)\n", r.trials);
        CHECK(r.trials < 17);
    }
    SECTION("window: minimal search (no verify, no probe) = 7 trials like the spec");
    {
        SearchConfig c = cfg; c.verify = false; c.probeBeyond = 0;
        auto r = runSearch(c, window(-2, 2));
        CHECK(r.lo == -2 && r.hi == 2 && r.trials == 7);
    }
    SECTION("window: one sided / single tick");
    {
        auto a = runSearch(cfg, window(0, 3));   CHECK(a.lo == 0 && a.hi == 3 && a.width() == 4);
        auto b = runSearch(cfg, window(-5, 0));  CHECK(b.lo == -5 && b.hi == 0);
        auto c = runSearch(cfg, window(0, 0));   CHECK(c.lo == 0 && c.hi == 0 && c.width() == 1);
    }
    SECTION("window: radius clip is reported, not silently treated as a boundary");
    {
        auto r = runSearch(cfg, [](int) { return Verdict::Pass; });
        CHECK(r.lo == -8 && r.hi == 8 && r.clipLo == Limit::Radius && r.clipHi == Limit::Radius);
    }
    SECTION("window: neighbour clip");
    {
        SearchConfig c = cfg; c.rangeLo = -3; c.limitLo = Limit::Neighbor; c.rangeHi = 1; c.limitHi = Limit::Neighbor;
        auto r = runSearch(c, [](int) { return Verdict::Pass; });
        CHECK(r.lo == -3 && r.hi == 1 && r.clipLo == Limit::Neighbor && r.clipHi == Limit::Neighbor);
        SearchConfig z = cfg; z.rangeHi = 0; z.limitHi = Limit::Neighbor;   // next input in the very next tick
        auto r2 = runSearch(z, window(-1, 5));
        CHECK(r2.hi == 0 && r2.clipHi == Limit::Neighbor && r2.lo == -1);
    }
    SECTION("window: original timing fails -> BASE-FAIL after a confirming run");
    {
        std::vector<int> order;
        auto r = runSearch(cfg, [](int) { return Verdict::Fail; }, &order);
        CHECK(r.baseFailed && r.width() == 0 && order.size() == 2);
    }
    SECTION("window: island beyond the first FAIL");
    {
        SearchConfig c = cfg; c.probeBeyond = 2;
        auto r = runSearch(c, [](int d) { return (d >= -1 && d <= 2) || d == 4 ? Verdict::Pass : Verdict::Fail; });
        CHECK(r.lo == -1 && r.hi == 2 && r.island);       // island flagged, NOT merged into the window
        SearchConfig t = cfg; t.thorough = true;
        auto r2 = runSearch(t, [](int d) { return (d >= -1 && d <= 2) || d == 7 ? Verdict::Pass : Verdict::Fail; });
        CHECK(r2.hi == 2 && r2.island);                   // thorough finds islands the probe cannot
    }
    SECTION("window: flaky boundary shrinks the window and is flagged");
    {
        int calls2 = 0;
        auto r = runSearch(cfg, [&](int d) {
            if (d == 2) return (calls2++ == 0) ? Verdict::Pass : Verdict::Fail;  // passes once, then fails
            return (d >= -2 && d <= 2) ? Verdict::Pass : Verdict::Fail;
        });
        CHECK(r.unstable && r.hi == 1 && r.lo == -2);
    }
    SECTION("window: Invalid verdict (drift) counts as not-pass and is flagged");
    {
        auto r = runSearch(cfg, [](int d) { return d == 3 ? Verdict::Invalid : (d >= -2 && d <= 2 ? Verdict::Pass : Verdict::Fail); });
        CHECK(r.drift && r.hi == 2);
    }
}

// ---------------------------------------------------------------- fake game
// A tiny "level": the player moves speed units per tick; every constrained input
// must fire inside [A,B] (absolute ticks) or the player dies `delay` ticks after
// the mistake.
struct Rule { int A, B, delay; };
struct FakeLevel {
    double speed = 1.298245;
    std::map<int, Rule> rules;      // action id (first action of the input) -> rule
    int fireBiasId = -1;            // drift test: report this input firing 1 tick late
    int flakeId = -1;               // flaky test: first FAIL for this id is reported as PASS once
    mutable int flakeBudget = 1;
    int killAtTick = -1;            // reference dies here regardless
    int trials = 0;
    int captureStride = 1;          // only ticks divisible by this get a checkpoint (simulates frames that end mid-input)
    bool cpArtifact = false;        // simulate a checkpoint restore that does NOT reproduce the original timing
    mutable long long ticksSimulated = 0;
};

static TrialResult runFake(const FakeLevel& lv, const TrialSpec& spec) {
    TrialResult r;
    r.firedTick.assign(spec.actions.size(), -1);
    Cursor cur;
    cur.bind(&spec.actions, 0.001);

    int maxId = 0;
    for (auto& a : spec.actions) maxId = std::max(maxId, a.id);
    std::vector<int> posOfId(maxId + 1, -1);
    for (size_t i = 0; i < spec.actions.size(); ++i) posOfId[spec.actions[i].id] = static_cast<int>(i);
    const int t0 = spec.checkpointTick >= 0 ? spec.checkpointTick + 1 : 0;   // state restored at the end of that tick
    std::multimap<int, int> lateCheck;  // tick -> rule id that must have fired by then
    for (auto& kv : lv.rules) {
        const int pos = kv.first <= maxId ? posOfId[kv.first] : -1;
        // an input that already fired before the restored checkpoint was satisfied in the run that made the checkpoint
        const bool before = pos >= 0 && spec.actions[pos].x < t0 * lv.speed - 0.5 * lv.speed;
        if (!before && kv.second.B + 1 > t0) lateCheck.insert({kv.second.B + 1, kv.first});
    }

    int deathAt = lv.cpArtifact && spec.checkpointTick >= 0 ? t0 : INT_MAX;
    std::vector<Fire> fires;
    int lastFire = 0;
    if (t0 > 0) cur.seekTo(t0 * lv.speed, fires);
    fires.clear();
    for (int t = t0;; ++t) {
        ++lv.ticksSimulated;
        if (t >= spec.maxTick) { r.timedOut = true; r.ticksRun = t; break; }
        const double x = t * lv.speed;
        if (spec.recordX) { r.xs.push_back(x); r.ys.push_back(105.0); }
        fires.clear();
        cur.poll(x, fires);
        for (const Fire& f : fires) {
            r.firedTick[f.pos] = t;
            lastFire = t;
            auto it = lv.rules.find(spec.actions[f.pos].id);
            if (it != lv.rules.end() && t < it->second.A) deathAt = std::min(deathAt, t + it->second.delay);
        }
        auto range = lateCheck.equal_range(t);
        for (auto it = range.first; it != range.second; ++it) {
            const int pos = it->second <= maxId ? posOfId[it->second] : -1;
            const bool fired = pos >= 0 && r.firedTick[pos] >= 0 && r.firedTick[pos] <= lv.rules.at(it->second).B;
            if (!fired) deathAt = std::min(deathAt, t + lv.rules.at(it->second).delay);
        }
        if (lv.killAtTick >= 0) deathAt = std::min(deathAt, lv.killAtTick);
        if (t >= deathAt) {
            r.died = true; r.deathTick = t; r.deathX = x; r.ticksRun = t + 1;
            break;
        }
        r.ticksRun = t + 1;
        if (spec.captureCheckpoints && !fires.empty() && t % lv.captureStride == 0) r.checkpointTicks.push_back(t);
        if (spec.endTick >= 0 && t >= spec.endTick) break;
        if (spec.stopAfterLastFire >= 0 && cur.done() && t >= lastFire + spec.stopAfterLastFire) break;
    }
    if (lv.fireBiasId >= 0 && !spec.recordX) {
        for (size_t i = 0; i < spec.actions.size(); ++i)
            if (spec.actions[i].id == lv.fireBiasId && r.firedTick[i] >= 0) r.firedTick[i] += 1;
    }
    return r;
}

static std::string macroText(const std::vector<std::tuple<int, int, int>>& evs, double speed = 1.298245) {
    // (tick, down, p2)  ->  x = tick * speed, exactly what the fake player has at that tick
    std::ostringstream o;
    o.precision(17);
    for (auto& [tick, down, p2] : evs) o << tick * speed << " 0 " << down << " " << p2 << "\n";
    return o.str();
}

static Analyzer* drive(Analyzer* a, const FakeLevel& lv, int* trialCount = nullptr) {
    int n = 0;
    while (auto spec = a->nextTrial()) {
        a->submit(runFake(lv, *spec));
        if (++n > 100000) { std::printf("  runaway analyzer\n"); std::abort(); }
    }
    if (trialCount) *trialCount = n;
    return a;
}

static const GroupReport* byNumber(const Analyzer& a, int number) {
    for (auto& g : a.report().groups) if (g.number == number) return &g;
    return nullptr;
}

static void testAnalyzer() {
    // ids:  0 R@0 (no-op) | 1 P@100 | 2 R@130 | 3 R@150 (no-op) | 4 P@170
    //       5 R@200 + 6 P@200 (tap) | 7 R@204 | 8 P@300
    MacroFile m = parseMacro(macroText({{0, 0, 0}, {100, 1, 0}, {130, 0, 0}, {150, 0, 0}, {170, 1, 0},
                                        {200, 0, 0}, {200, 1, 0}, {204, 0, 0}, {300, 1, 0}}));
    CHECK(m.ok() && m.actions.size() == 9);
    FakeLevel lv;
    lv.rules[1] = {98, 103, 0};     // P@100 -> [-2,+3]
    lv.rules[2] = {129, 140, 0};    // R@130 -> [-1,+10]  (radius 8 clips the right side)
    lv.rules[4] = {169, 172, 0};    // P@170 -> [-1,+2]
    lv.rules[5] = {197, 215, 0};    // tap@200 -> [-3,+15] but the next input (tick 204) clips it to +3
    lv.rules[7] = {190, 210, 0};    // R@204 -> [-14,+6] but the tap at tick 200 clips the left to -3
    lv.rules[8] = {299, 301, 0};    // P@300 -> [-1,+1]

    SECTION("analyzer end-to-end on the fake game");
    AnalyzerSettings st;
    st.radius = 8;
    st.maxWidth = 0;          // these older checks measure the full window
    st.useCheckpoints = false;
    Analyzer a(m, st);
    int trials = 0;
    drive(&a, lv, &trials);
    const Report& rep = a.report();
    CHECK(rep.referenceRan && !rep.refDied && rep.note.empty());
    auto g1 = byNumber(a, 2);   // P@100
    CHECK(g1 && g1->state == GroupState::Analyzed && g1->refTick == 100 && g1->w.lo == -2 && g1->w.hi == 3 && g1->w.width() == 6);
    auto g2 = byNumber(a, 3);   // R@130
    CHECK(g2 && g2->w.lo == -1 && g2->w.hi == 8 && g2->w.clipHi == Limit::Radius && g2->w.clipLo == Limit::None);
    auto gnoop = byNumber(a, 4);  // double release
    CHECK(gnoop && gnoop->state == GroupState::Redundant);
    auto g4 = byNumber(a, 5);   // P@170
    CHECK(g4 && g4->w.lo == -1 && g4->w.hi == 2);
    auto gtap = byNumber(a, 6);   // tap (actions 6-7)
    CHECK(gtap && gtap->kind == GroupKind::Tap && gtap->w.lo == -3 && gtap->w.hi == 3 && gtap->w.clipHi == Limit::Neighbor);
    auto gnb = byNumber(a, 8);    // R@204: the tap at 200 limits the left side to -3
    CHECK(gnb && gnb->state == GroupState::Analyzed && gnb->w.lo == -3 && gnb->w.clipLo == Limit::Neighbor && gnb->w.hi == 6);
    auto gl = byNumber(a, 9);
    CHECK(gl && gl->w.lo == -1 && gl->w.hi == 1);
    auto g0 = byNumber(a, 1);     // release at x=0, nothing held
    CHECK(g0 && g0->state == GroupState::Redundant);
    std::printf("  total trials: %d for %zu inputs\n", trials, a.groups().groups.size());
    {
        ReportStyle rs;
        std::string txt = formatReport(a, rs, "fake.txt");
        std::printf("%s", txt.c_str());
        CHECK(txt.find("[-2,+3] 6F") != std::string::npos);
        CHECK(formatCsv(a, 240.0).find("analyzed") != std::string::npos);
        CHECK(txt.find("summary: 6 analysed") != std::string::npos);
    }

    SECTION("overlay: saved csv -> markers (persistence round trip)");
    {
        std::string csv = formatCsv(a, 240.0);
        SavedResults sr = parseResultsCsv(csv);
        CHECK(sr.items.size() == 6);
        CHECK(sr.items[0].width == 6 && sr.items[0].lo == -2 && sr.items[0].hi == 3 && !sr.items[0].clipped);
        CHECK(std::fabs(sr.items[0].y - 105.0) < 1e-6 && std::fabs(sr.items[0].x - 129.8245) < 0.01);
        CHECK(sr.items[1].clipped);                       // R@130 hit the radius on the right
        int h[5]; histogram(sr.items, h);
        CHECK(h[0] + h[1] + h[2] + h[3] + h[4] == 6);
        CHECK(widthBucket(12) == 5 && widthBucket(1) == 1 && widthBucket(4) == 4);
        CHECK(markerText(sr.items[1]).back() == '+');
        SavedResults lv = parseResultsCsv("# level My Level\n# macro a.txt\nnumbers,player\n3,P1,press,1.0,2.0,analyzed,-1,1,3,12.5,,,stable,7\n4,P1,press,1.0,2.0,unreached,,,,,,,,\n");
        CHECK(lv.level == "My Level" && lv.macro == "a.txt" && lv.items.size() == 1 && lv.items[0].width == 3);
        CHECK(parseResultsCsv("garbage\n\n,,,\n").items.empty());
    }

    SECTION("analyzer: horizon - a late death needs a long enough horizon");
    {
        MacroFile m2 = parseMacro(macroText({{100, 1, 0}, {400, 0, 0}}));
        FakeLevel lv2;
        lv2.rules[0] = {98, 100, 30};  // pressing early kills 30 ticks later
        AnalyzerSettings sShort; sShort.radius = 8; sShort.maxWidth = 0; sShort.useCheckpoints = false; sShort.horizonMinTicks = 10; sShort.horizonGroups = 0;
        Analyzer a1(m2, sShort); drive(&a1, lv2);
        AnalyzerSettings sLong; sLong.radius = 8; sLong.maxWidth = 0; sLong.useCheckpoints = false; sLong.horizonMinTicks = 60; sLong.horizonGroups = 0;
        Analyzer a2(m2, sLong); drive(&a2, lv2);
        const GroupReport* s1 = byNumber(a1, 1); const GroupReport* s2 = byNumber(a2, 1);
        CHECK(s1 && s2);
        CHECK(s1->w.lo == -8 && s1->w.clipLo == Limit::Radius);   // short horizon misses the death: window looks wider than it is
        CHECK(s2->w.lo == -2 && s2->w.hi == 0);                   // long horizon sees it
    }

    SECTION("analyzer: reference that dies - later inputs unreachable, horizon capped");
    {
        FakeLevel lv3 = lv;
        lv3.killAtTick = 250;
        Analyzer a3(m, st); drive(&a3, lv3);
        CHECK(a3.report().refDied && a3.report().refDeathTick == 250);
        auto late = byNumber(a3, 9);   // press@300 never reached
        CHECK(late && late->state == GroupState::Unreached);
        auto tap = byNumber(a3, 5);   // P@170 is reached
        CHECK(tap && tap->state == GroupState::Analyzed && tap->endTick <= 249);
    }

    SECTION("analyzer: drift (input fired on a different tick than planned)");
    {
        FakeLevel lv4 = lv;
        lv4.fireBiasId = 1;
        Analyzer a4(m, st); drive(&a4, lv4);
        auto g = byNumber(a4, 2);
        CHECK(g && g->w.drift);
    }

    SECTION("analyzer: only one action");
    {
        AnalyzerSettings so = st; so.onlyAction = 9;
        Analyzer a5(m, so); int n = 0; drive(&a5, lv, &n);
        CHECK(byNumber(a5, 9)->state == GroupState::Analyzed && byNumber(a5, 2)->state == GroupState::NotSelected);
        AnalyzerSettings sb = st; sb.onlyAction = 99;
        Analyzer a6(m, sb); drive(&a6, lv);
        CHECK(!a6.report().note.empty());
    }

    SECTION("analyzer: players are independent streams");
    {
        // P1 press@100, P2 press@101 (1 tick later): P2 must not clip P1's right side
        MacroFile mp = parseMacro(macroText({{100, 1, 0}, {101, 1, 1}, {200, 0, 0}, {201, 0, 1}}));
        FakeLevel lvp; lvp.rules[0] = {98, 104, 0};
        Analyzer ap(mp, st); drive(&ap, lvp);
        auto gp = byNumber(ap, 1);
        CHECK(gp && gp->w.lo == -2 && gp->w.hi == 4);
    }

    SECTION("analyzer: x that does not grow (plateau) cannot be placed on single ticks");
    {
        FakeLevel flat = lv; flat.speed = 0.0;   // player never moves
        MacroFile mf = parseMacro("0 0 1 0\n0 0 0 0\n");
        Analyzer af(mf, st);
        int n = 0;
        while (auto spec = af.nextTrial()) { af.submit(runFake(flat, *spec)); if (++n > 50) break; }
        CHECK(n < 50);
    }

    SECTION("analyzer: cancel yields partial results without crashing");
    {
        Analyzer ac(m, st);
        auto s0 = ac.nextTrial(); ac.submit(runFake(lv, *s0));
        auto s1 = ac.nextTrial(); (void)s1;
        ac.cancel("test");
        CHECK(ac.finished() && ac.report().cancelled);
        CHECK(!ac.nextTrial());
        std::string txt = formatReport(ac, ReportStyle{}, "x");
        CHECK(txt.find("CANCELLED") != std::string::npos);
    }
}

// ---------------------------------------------------------------- real macro through the fake game
static void testRealMacroSmoke() {
    SECTION("real macro: fake level that allows +-2 ticks everywhere");
    MacroFile m = parseMacro(slurp("tests/spamy_wVs.txt"));
    FakeLevel lv;
    lv.speed = 1.2982;  // refX never equals recorded x exactly here; eps handles it
    // constrain every real input to [T-2, T+2] where T is the tick the reference fires it on
    Analyzer probe(m, AnalyzerSettings{});
    // first, learn reference fire ticks with an unconstrained level
    TrialSpec s0 = *probe.nextTrial();
    TrialResult ref = runFake(lv, s0);
    for (auto& g : probe.groups().groups) {
        if (g.kind == GroupKind::NoOp) continue;
        int T = ref.firedTick[g.actions[0]];
        lv.rules[g.actions[0]] = {T - 2, T + 2, 0};
    }
    Analyzer a(m, AnalyzerSettings{});
    int trials = 0;
    drive(&a, lv, &trials);
    int analysed = 0, wrong = 0;
    for (auto& g : a.report().groups) {
        if (g.state != GroupState::Analyzed) continue;
        ++analysed;
        // neighbours that sit closer than 3 ticks clip the window; everything else must be exactly [-2,+2]
        bool clipped = g.w.clipLo == Limit::Neighbor || g.w.clipHi == Limit::Neighbor;
        if (!clipped && !(g.w.lo == -2 && g.w.hi == 2)) ++wrong;
    }
    std::printf("  analysed %d inputs, %d trials (%.1f per input), unexpected windows: %d\n", analysed, trials,
                analysed ? double(trials) / analysed : 0.0, wrong);
    CHECK(analysed == 131);   // 137 groups - 6 no-ops
    CHECK(wrong == 0);
    CHECK(!a.report().refDied);
}

static void testHorizonFreeResume() {
    SECTION("horizon: default = survive until the next input of the original macro");
    {
        MacroFile m = parseMacro(macroText({{100, 1, 0}, {130, 0, 0}}));
        FakeLevel lv;
        lv.rules[0] = {98, 100, 40};  // pressing early kills 40 ticks later, i.e. AFTER the release at tick 130
        AnalyzerSettings d; d.radius = 8; d.maxWidth = 0; d.useCheckpoints = false; d.skipFreeReleases = false;
        Analyzer a(m, d); drive(&a, lv);
        const GroupReport* g = byNumber(a, 1);
        CHECK(g && g->endTick == 130);
        CHECK(g->w.lo == -8 && g->w.clipLo == Limit::Radius);   // the late death is outside "until the release"
        AnalyzerSettings far = d; far.horizonMinTicks = 60;
        Analyzer b(m, far); drive(&b, lv);
        CHECK(byNumber(b, 1)->w.lo == -2 && byNumber(b, 1)->w.hi == 0);   // asking for a longer horizon sees it
        // a release must survive until the next click
        MacroFile m2 = parseMacro(macroText({{100, 1, 0}, {130, 0, 0}, {160, 1, 0}}));
        FakeLevel lv2; lv2.rules[1] = {129, 131, 0};
        Analyzer c(m2, d); drive(&c, lv2);
        CHECK(byNumber(c, 2)->endTick == 160);
    }

    SECTION("free releases: survive -10 and +10 -> skipped with 2 trials");
    {
        MacroFile m = parseMacro(macroText({{100, 1, 0}, {200, 0, 0}, {300, 1, 0}, {304, 0, 0}, {400, 1, 0}}));
        FakeLevel lv;
        lv.rules[0] = {98, 102, 0};
        lv.rules[1] = {150, 250, 0};      // very forgiving release
        lv.rules[2] = {298, 302, 0};
        lv.rules[3] = {303, 306, 0};      // release 4 ticks after the press: -10 is not reachable, so no probe
        lv.rules[4] = {399, 401, 0};
        AnalyzerSettings st; st.radius = 10; st.maxWidth = 0; st.useCheckpoints = false;
        Analyzer a(m, st); drive(&a, lv);
        auto rel = byNumber(a, 2);
        CHECK(rel && rel->state == GroupState::Free && rel->w.trials == 2);
        auto near = byNumber(a, 4);
        CHECK(near && near->state == GroupState::Analyzed && near->w.lo == -1 && near->w.hi == 2);
        CHECK(byNumber(a, 1)->state == GroupState::Analyzed);   // presses are never filtered
        CHECK(formatCsv(a, 240).find(",free,") != std::string::npos);

        AnalyzerSettings off = st; off.skipFreeReleases = false;
        Analyzer b(m, off); drive(&b, lv);
        CHECK(byNumber(b, 2)->state == GroupState::Analyzed && byNumber(b, 2)->w.lo == -10 && byNumber(b, 2)->w.hi == 10);

        FakeLevel lv2 = lv; lv2.rules[1] = {196, 210, 0};       // -10 fails -> not free, real window, probe results reused
        Analyzer c(m, st); drive(&c, lv2);
        CHECK(byNumber(c, 2)->state == GroupState::Analyzed && byNumber(c, 2)->w.lo == -4 && byNumber(c, 2)->w.hi == 10);
    }

    SECTION("resume: interrupted analysis continues, finished inputs are not measured again");
    {
        std::vector<std::tuple<int, int, int>> ev;
        FakeLevel lv;
        for (int i = 0; i < 12; ++i) { ev.push_back({200 + i * 120, 1, 0}); ev.push_back({240 + i * 120, 0, 0}); }
        for (int i = 0; i < 12; ++i) { lv.rules[2 * i] = {198 + i * 120, 202 + i * 120, 0}; lv.rules[2 * i + 1] = {238 + i * 120, 243 + i * 120, 0}; }
        MacroFile m = parseMacro(macroText(ev));
        AnalyzerSettings st; st.useCheckpoints = false; st.maxWidth = 0; st.skipFreeReleases = false;

        Analyzer full(m, st); drive(&full, lv);
        CHECK(full.report().complete);
        CHECK(formatCsv(full, 240).find("# complete 1") != std::string::npos);

        Analyzer part(m, st);
        for (int i = 0; i < 70; ++i) { auto sp = part.nextTrial(); if (!sp) break; part.submit(runFake(lv, *sp)); }
        (void)part.nextTrial();
        part.cancel("paused");
        std::string csv = formatCsv(part, 240);
        CHECK(csv.find("# complete 0") != std::string::npos);
        SavedResults saved = parseResultsCsv(csv);
        CHECK(!saved.complete && saved.total == 24 && saved.done > 0 && saved.done < 24);
        std::printf("  interrupted after 70 trials: %d of %d inputs saved\n", saved.done, saved.total);

        Analyzer res(m, st);
        res.usePrior(saved.rows);
        drive(&res, lv);
        CHECK(res.report().complete && res.report().resumed == saved.done);
        bool same = true;
        for (size_t i = 0; i < full.report().groups.size(); ++i) {
            const auto& x = full.report().groups[i]; const auto& y = res.report().groups[i];
            if (x.state != y.state || x.w.lo != y.w.lo || x.w.hi != y.w.hi || x.w.clipLo != y.w.clipLo || x.w.clipHi != y.w.clipHi) same = false;
        }
        CHECK(same);
        std::printf("  trials: full %d, interrupted %d + resumed %d\n", full.report().totalTrials, part.report().totalTrials, res.report().totalTrials);
        CHECK(res.report().totalTrials < full.report().totalTrials);

        // a changed macro (other x) must not take over old rows
        MacroFile other = parseMacro(macroText({{300, 1, 0}, {340, 0, 0}}));
        Analyzer w(other, st); w.usePrior(saved.rows);
        FakeLevel lvo; drive(&w, lvo);
        CHECK(w.report().resumed == 0);
    }

    SECTION("sound pitch: 5+ normal, -0.1 per frame down to 1");
    CHECK(pitchForWidth(10) == 1.0f && pitchForWidth(5) == 1.0f);
    CHECK(std::fabs(pitchForWidth(4) - 0.9f) < 1e-6 && std::fabs(pitchForWidth(3) - 0.8f) < 1e-6);
    CHECK(std::fabs(pitchForWidth(2) - 0.7f) < 1e-6 && std::fabs(pitchForWidth(1) - 0.6f) < 1e-6);
    CHECK(std::fabs(pitchForWidth(0) - 0.6f) < 1e-6);
}

static void testCapAndCheckpoints() {
    SECTION("window: width cap stops early and reports N+");
    {
        SearchConfig c; c.rangeLo = -20; c.rangeHi = 20; c.maxWidth = 10; c.verify = false; c.probeBeyond = 0;
        std::vector<int> order;
        auto r = runSearch(c, [](int) { return Verdict::Pass; }, &order);
        CHECK(r.capped && r.width() == 10 && r.clipHi == Limit::Cap);
        CHECK(order.size() <= 12);                               // 0 plus about 10 offsets, not 41
        auto narrow = runSearch(c, window(-2, 2));
        CHECK(!narrow.capped && narrow.width() == 5);            // narrow windows are still measured exactly
        c.maxWidth = 0;
        CHECK(!runSearch(c, [](int) { return Verdict::Pass; }).capped);
        CHECK(windowText(r) == "10+F");
    }

    SECTION("analyzer: 10+ and moving on");
    {
        MacroFile m = parseMacro(macroText({{100, 1, 0}, {200, 0, 0}, {300, 1, 0}}));
        FakeLevel lv;
        lv.rules[0] = {80, 125, 0};   // 46 ticks wide
        lv.rules[1] = {198, 202, 0};
        AnalyzerSettings st; st.radius = 20; st.useCheckpoints = false;   // maxWidth 10 is the default
        Analyzer a(m, st); int n = 0; drive(&a, lv, &n);
        auto g0 = byNumber(a, 1); auto g1 = byNumber(a, 2);
        CHECK(g0 && g0->w.capped && g0->w.width() == 10);
        CHECK(g1 && !g1->w.capped && g1->w.lo == -2 && g1->w.hi == 2);
        const char* tagDummy = nullptr;
        CHECK(formatLine(a.groups(), *g0, ReportStyle{}, tagDummy).find("10+F") != std::string::npos);
        std::printf("  trials with cap: %d\n", n);
    }

    SECTION("checkpoints: same windows, far fewer ticks simulated");
    {
        std::vector<std::tuple<int, int, int>> ev;
        FakeLevel lv;
        for (int i = 0; i < 12; ++i) { ev.push_back({200 + i * 120, 1, 0}); ev.push_back({240 + i * 120, 0, 0}); }
        for (int i = 0; i < 12; ++i) { lv.rules[2 * i] = {198 + i * 120, 202 + i * 120, 0}; lv.rules[2 * i + 1] = {238 + i * 120, 243 + i * 120, 0}; }
        MacroFile m = parseMacro(macroText(ev));
        AnalyzerSettings off; off.useCheckpoints = false; off.maxWidth = 0;
        AnalyzerSettings on = off; on.useCheckpoints = true;
        Analyzer a0(m, off); lv.ticksSimulated = 0; drive(&a0, lv); long long ticksOff = lv.ticksSimulated;
        Analyzer a1(m, on);  lv.ticksSimulated = 0; drive(&a1, lv); long long ticksOn = lv.ticksSimulated;
        std::printf("  ticks simulated: from start %lld, with checkpoints %lld (%.1fx less)\n", ticksOff, ticksOn, double(ticksOff) / double(ticksOn));
        CHECK(ticksOn * 3 < ticksOff);
        CHECK(a1.report().usedCheckpoints);
        bool same = true;
        for (size_t i = 0; i < a0.report().groups.size(); ++i) {
            const auto& x = a0.report().groups[i]; const auto& y = a1.report().groups[i];
            if (x.state != y.state || x.w.lo != y.w.lo || x.w.hi != y.w.hi || y.cpFallback) same = false;
        }
        CHECK(same);
        CHECK(!a1.report().groups[0].usedCheckpoint && a1.report().groups[5].usedCheckpoint);   // first input has nothing before it

        SECTION("checkpoints: frames that end mid-input -> the closest earlier checkpoint is used, same windows");
        FakeLevel sparse = lv; sparse.captureStride = 7;
        Analyzer a3(m, on); sparse.ticksSimulated = 0; drive(&a3, sparse);
        bool sameSparse = true; int usedCp = 0;
        for (size_t i = 0; i < a0.report().groups.size(); ++i) {
            const auto& x = a0.report().groups[i]; const auto& y = a3.report().groups[i];
            if (x.state != y.state || x.w.lo != y.w.lo || x.w.hi != y.w.hi) sameSparse = false;
            usedCp += y.usedCheckpoint;
        }
        CHECK(sameSparse);
        CHECK(usedCp > 0 && sparse.ticksSimulated < ticksOff);
        std::printf("  sparse checkpoints: %d inputs started from one, %lld ticks (from start: %lld)\n", usedCp, sparse.ticksSimulated, ticksOff);

        SECTION("checkpoints: a restore that does not reproduce the original timing falls back to the level start");
        FakeLevel bad = lv; bad.cpArtifact = true;
        Analyzer a2(m, on); drive(&a2, bad);
        bool allOk = true; int fallbacks = 0;
        for (size_t i = 0; i < a0.report().groups.size(); ++i) {
            const auto& x = a0.report().groups[i]; const auto& y = a2.report().groups[i];
            if (x.state != y.state || x.w.lo != y.w.lo || x.w.hi != y.w.hi) allOk = false;
            fallbacks += y.cpFallback;
        }
        CHECK(allOk);
        CHECK(fallbacks == 2);   // two failed checkpoint starts are enough: after that the run stops trying checkpoints
        CHECK(a2.report().cpDisabled && a2.report().cpFallbacks == 2);
        CHECK(flagsText(a2.report().groups[1]).find("CP-FALLBACK") != std::string::npos);
        std::printf("  trials with a broken restore: %d (every input retrying would be ~%d)\n", a2.report().totalTrials, a0.report().totalTrials + 2 * 23);
    }
}

static void testFastMode() {
    SECTION("fast mode (beta): extremes decide 10+, calibrated checkpoints skip the base test");
    std::vector<std::tuple<int, int, int>> ev;
    FakeLevel lv;
    for (int i = 0; i < 12; ++i) { ev.push_back({200 + i * 120, 1, 0}); ev.push_back({240 + i * 120, 0, 0}); }
    for (int i = 0; i < 12; ++i) {
        const bool wide = (i % 2 == 0);                       // every other click is forgiving (>= 10 wide)
        lv.rules[2 * i] = wide ? Rule{190 + i * 120, 215 + i * 120, 0} : Rule{198 + i * 120, 202 + i * 120, 0};
        lv.rules[2 * i + 1] = {238 + i * 120, 243 + i * 120, 0};
    }
    MacroFile m = parseMacro(macroText(ev));
    AnalyzerSettings normal; normal.skipFreeReleases = false;          // checkpoints + maxWidth 10 are the defaults
    AnalyzerSettings fast = normal; fast.fastMode = true;
    Analyzer a0(m, normal); drive(&a0, lv);
    Analyzer a1(m, fast);   drive(&a1, lv);
    std::printf("  trials: normal %d, fast mode %d\n", a0.report().totalTrials, a1.report().totalTrials);
    CHECK(a1.report().totalTrials < a0.report().totalTrials);
    bool same = true; int assumed = 0;
    for (size_t i = 0; i < a0.report().groups.size(); ++i) {
        const auto& x = a0.report().groups[i]; const auto& y = a1.report().groups[i];
        if (x.state != y.state || x.w.width() != y.w.width() || x.w.capped != y.w.capped) same = false;
        assumed += y.fastAssumed;
    }
    CHECK(same);                 // same widths / 10+ labels as the full search on this level
    CHECK(assumed == 6);         // the six forgiving clicks were decided by two trials each
    CHECK(flagsText(a1.report().groups[4]).find("FAST-ASSUMED") != std::string::npos);
    CHECK(a1.report().cpOk >= 3);

    SECTION("fast mode: extreme probe that fails falls back to the normal search");
    {
        FakeLevel narrow = lv;
        narrow.rules[4] = {438, 441, 0};                   // click at tick 440, was forgiving: now [-2,+1]; +5 fails, so no assumption
        Analyzer a(m, fast); drive(&a, narrow);
        const GroupReport* g = byNumber(a, 5);
        CHECK(g && !g->fastAssumed && g->w.lo == -2 && g->w.hi == 1);
    }
}

static void testSeekAndOld() {
    SECTION("cursor: seekTo after respawn restores a held button");
    std::vector<MacroAction> v(4);
    v[0].x = 100; v[0].down = true;
    v[1].x = 200; v[1].down = false;
    v[2].x = 300; v[2].down = true;  v[2].p2 = true;
    v[3].x = 400; v[3].down = false; v[3].p2 = true;
    Cursor c; c.bind(&v, 0.001);
    std::vector<Fire> out;
    c.seekTo(0.0, out);   CHECK(out.empty() && c.next() == 0);              // start of level
    out.clear(); c.seekTo(150.0, out);
    CHECK(c.next() == 1 && out.size() == 1 && out[0].down && !out[0].p2);   // was holding P1 -> press again
    out.clear(); c.seekTo(250.0, out);
    CHECK(c.next() == 2 && out.empty());                                     // P1 released at 200
    out.clear(); c.seekTo(350.0, out);
    CHECK(c.next() == 3 && out.size() == 1 && out[0].p2);                    // P2 held
    out.clear(); c.seekTo(200.0, out);
    CHECK(c.next() == 1);                                                    // action exactly at the position is NOT skipped
    out.clear(); c.seekTo(9999.0, out);
    CHECK(c.done() && out.empty());

    SECTION("overlay: results from an old version are detected");
    SavedResults o = parseResultsCsv("numbers,player,kind,x,state,lo,hi,width_ticks,width_ms,clip_lo,clip_hi,flags,trials\n2,P1,press,129.8,analyzed,-2,3,6,25.0,,,stable,9\n");
    CHECK(o.oldFormat && o.items.empty());
    CHECK(averageWidth({}) == 0.0);
}

static void testWrap() {
    SECTION("report: wrapping keeps one colour tag per line");
    auto l = wrapLine("aaaa bbbb cccc dddd eeee", 10);
    CHECK(l.size() >= 3);
    for (auto& x : l) CHECK(x.size() <= 12);
    CHECK(wrapLine("short", 10).size() == 1);
    CHECK(wrapLine("x", 0).size() == 1);
    std::string out;
    ReportStyle st; st.tags = true; st.wrap = 12;
    addLine(out, st, "cr", "one two three four five six");
    int opens = 0, closes = 0;
    for (size_t i = 0; (i = out.find("<cr>", i)) != std::string::npos; ++i) ++opens;
    for (size_t i = 0; (i = out.find("</c>", i)) != std::string::npos; ++i) ++closes;
    CHECK(opens == closes && opens >= 2);
}

int main() {
    testFastMode();
    testHorizonFreeResume();
    testCapAndCheckpoints();
    testSeekAndOld();
    testWrap();
    testParser();
    testGroups();
    testCursor();
    testWindowSearch();
    testAnalyzer();
    testRealMacroSmoke();
    std::printf("\n%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
