// Host tests for the pure-C++ core.  Build & run:  ./tests/run_tests.sh
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

#include "../src/analyzer.hpp"
#include "../src/playback.hpp"
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
    std::multimap<int, int> lateCheck;  // tick -> rule id that must have fired by then
    for (auto& kv : lv.rules) lateCheck.insert({kv.second.B + 1, kv.first});

    int deathAt = INT_MAX;
    std::vector<Fire> fires;
    int lastFire = 0;
    for (int t = 0;; ++t) {
        if (t >= spec.maxTick) { r.timedOut = true; r.ticksRun = t; break; }
        const double x = t * lv.speed;
        if (spec.recordX) r.xs.push_back(x);
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
    }

    SECTION("analyzer: horizon - a late death needs a long enough horizon");
    {
        MacroFile m2 = parseMacro(macroText({{100, 1, 0}, {400, 0, 0}}));
        FakeLevel lv2;
        lv2.rules[0] = {98, 100, 30};  // pressing early kills 30 ticks later
        AnalyzerSettings sShort; sShort.horizonMinTicks = 10; sShort.horizonGroups = 0;
        Analyzer a1(m2, sShort); drive(&a1, lv2);
        AnalyzerSettings sLong; sLong.horizonMinTicks = 60; sLong.horizonGroups = 0;
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
