// Percent Frame Window - Geode glue.
// All the logic (parsing, grouping, window search, analyzer) lives in the pure
// C++ headers next to this file and is unit-tested on a PC (tests/).
// This file only does what needs the game:
//   * count physics ticks and read the player's x          (GJBaseGameLayer::processCommands)
//   * send the macro's inputs by position                  (GJBaseGameLayer::handleButton)
//   * detect death / level end without the death screen    (PlayLayer::destroyPlayer / levelComplete)
//   * restart the level for every trial and run it fast    (GJBaseGameLayer::update)
//   * pick a macro and show the result                     (PauseLayer button + popups)

#include <Geode/Geode.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>

#include "analyzer.hpp"
#include "macro.hpp"
#include "overlay.hpp"
#include "playback.hpp"
#include "report.hpp"

// Checkpoint fast mode uses PlayLayer::createCheckpoint / loadFromCheckpoint. If your Geode bindings do not have them,
// build with -DFW_NO_CHECKPOINTS (see CMakeLists.txt) and everything falls back to restarting from the level start.
using namespace geode::prelude;
namespace fs = std::filesystem;

namespace {

    constexpr float kStepDt = 1.f / 60.f;  // fixed dt for every analysis frame => identical batching in every trial

    enum class Phase { Idle, Running };

    struct Run {
        Phase phase = Phase::Idle;
        std::unique_ptr<fw::Analyzer> analyzer;
        std::string macroName;

        fw::TrialSpec spec;
        fw::TrialResult res;
        fw::Cursor cursor;
        bool trialActive = false;
        bool injecting = false;      // true while WE call handleButton (real touches are ignored otherwise)
        bool inFrame = false;        // true while we drive a full PlayLayer frame (hook must pass straight through)
        bool resettingByUs = false;  // lets the resetLevel hook tell our restarts from foreign ones
        int tick = 0;                // physics ticks executed in the current trial
        int lastFire = -1;
        int idleCalls = 0;

        long long tickTotal = 0;     // for the observed-TPS check
        long long updateCalls = 0;

        int speed = 8;               // update() calls per real frame
        double eps = 0.001;
        double expectedTps = 240.0;
        bool saveReport = true;
        int ignoreEarlyTicks = 0;
        int overlayRevision = 0;     // bumped when new results are saved so the markers reload
        int anticheatHits = 0;       // hits of the game's hidden anti-cheat spike ignored during this analysis
#ifndef FW_NO_CHECKPOINTS
        std::map<int, Ref<CheckpointObject>> cps;  // reference run: checkpoint taken at the end of tick N
#endif
        std::string probe;           // x/y of the first ticks of a reference run (shown if the run dies early)

        Ref<CCLabelBMFont> label;
        std::string pendingTitle, pendingText;  // popup to show on the next frame
    } g;

    // ---------------------------------------------------------------- helpers

    double playerX(PlayerObject* p) {
        // If a build error mentions this line, or windows look shifted by one tick, this is the one place to change.
        return p ? static_cast<double>(p->getPositionX()) : 0.0;
    }

    std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }

    // Folder that the percent-macro mod keeps its macros in:
    //   .../save/geode/mods/yourname.percentmacro/macros
    fs::path resolveMacrosDir() {
        std::error_code ec;
        auto mod = Mod::get();
        std::string custom = mod->getSettingValue<std::string>("macros-dir");
        if (!custom.empty() && fs::is_directory(fs::path(custom), ec)) return fs::path(custom);

        if (auto pm = Loader::get()->getLoadedMod("yourname.percentmacro")) {
            fs::path p = fs::path(pm->getSaveDir()) / "macros";
            if (fs::is_directory(p, ec)) return p;
        }
        fs::path sibling = fs::path(mod->getSaveDir()).parent_path() / "yourname.percentmacro" / "macros";
        if (fs::is_directory(sibling, ec)) return sibling;

        fs::path fixed = "/storage/emulated/0/Android/media/com.geode.launcher/save/geode/mods/yourname.percentmacro/macros";
        if (fs::is_directory(fixed, ec)) return fixed;
        return sibling;  // does not exist; shown to the user
    }

    std::vector<std::string> listMacros(const fs::path& dir) {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return out;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (!it->is_regular_file(e2)) continue;
            std::string ext = lower(it->path().extension().string());
            if (ext != ".txt") continue;
            out.push_back(it->path().filename().string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    bool readFile(const fs::path& p, std::string& out) {
        std::ifstream f(p, std::ios::binary);
        if (!f) return false;
        std::stringstream ss;
        ss << f.rdbuf();
        out = ss.str();
        return true;
    }

    fw::AnalyzerSettings readSettings() {
        auto mod = Mod::get();
        fw::AnalyzerSettings s;
        s.radius = static_cast<int>(mod->getSettingValue<int64_t>("scan-radius"));
        s.thorough = mod->getSettingValue<bool>("thorough-scan");
        s.probeBeyond = static_cast<int>(mod->getSettingValue<int64_t>("probe-beyond"));
        s.verify = mod->getSettingValue<bool>("verify-boundaries");
        s.horizonGroups = static_cast<int>(mod->getSettingValue<int64_t>("horizon-groups"));
        s.horizonMinTicks = static_cast<int>(mod->getSettingValue<int64_t>("horizon-min-ticks"));
        s.horizonToEnd = mod->getSettingValue<bool>("horizon-to-end");
        s.epsilon = mod->getSettingValue<double>("x-epsilon");
        s.onlyAction = static_cast<int>(mod->getSettingValue<int64_t>("only-action"));
        s.maxWidth = static_cast<int>(mod->getSettingValue<int64_t>("max-width"));
        s.useCheckpoints = mod->getSettingValue<bool>("use-checkpoints");
        const double tps = static_cast<double>(mod->getSettingValue<int64_t>("expected-tps"));
        s.maxRunTicks = static_cast<int>(tps * 180.0);  // 3 minutes of game time per run
        return s;
    }

    void showInfo(const char* title, const std::string& text) {
        FLAlertLayer::create(title, text, "OK")->show();
    }

    // ---------------------------------------------------------------- saved results, window markers, playback

    // 0 = off, 1 = show windows, 2 = show windows AND play the macro (saved, so it survives restarts)
    int windowsMode() { return Mod::get()->getSavedValue<int>("fw-mode", 0); }
    void setWindowsMode(int m) {
        Mod::get()->setSavedValue<int>("fw-mode", m);
        ++g.overlayRevision;
    }
    fs::path resultsDir() { return fs::path(Mod::get()->getSaveDir()) / "results"; }
    fs::path resultsCsv(const std::string& macro) { return resultsDir() / (fs::path(macro).stem().string() + ".windows.csv"); }

    void toast(const std::string& text, NotificationIcon icon = NotificationIcon::Success) {
        Notification::create(text, icon, 2.5f)->show();
    }

    struct OverlayState {
        Ref<CCNode> world;                  // rings + numbers, scrolls with the level
        Ref<CCNode> hud;                    // 1 / 2 / 3 / 4 / 5+ counter, fixed on screen
        Ref<CCLabelBMFont> hudLabels[5];
        std::vector<fw::OverlayItem> items; // sorted by x
        int passed = 0;                     // how many clicks the player already went past
        double lastX = 0.0;
        std::string key;
    } ov;

    void clearOverlay() {
        if (ov.world) ov.world->removeFromParent();
        if (ov.hud) ov.hud->removeFromParent();
        ov.world = nullptr;
        ov.hud = nullptr;
        for (auto& l : ov.hudLabels) l = nullptr;
        ov.items.clear();
        ov.passed = 0;
        ov.key.clear();
    }

    std::vector<CCPoint> circlePoints(float cx, float cy, float r, int n = 24) {
        std::vector<CCPoint> pts;
        for (int i = 0; i < n; ++i) {
            const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
            pts.push_back(ccp(cx + r * std::cos(a), cy + r * std::sin(a)));
        }
        return pts;
    }

    // counter = clicks the player already passed, like in the video
    void refreshHud() {
        int h[5];
        std::vector<fw::OverlayItem> done(ov.items.begin(), ov.items.begin() + std::min<size_t>(ov.passed, ov.items.size()));
        fw::histogram(done, h);
        const char* names[5] = {"1:", "2:", "3:", "4:", "5+:"};
        for (int i = 0; i < 5; ++i)
            if (ov.hudLabels[i]) ov.hudLabels[i]->setString((std::string(names[i]) + "  " + std::to_string(h[i])).c_str());
    }

    // expanding circle when the player reaches a click
    void pulse(const fw::OverlayItem& it) {
        if (!ov.world) return;
        const fw::Rgb c = fw::widthColor(it.width);
        auto d = CCDrawNode::create();
        auto pts = circlePoints(0.f, 0.f, 8.f);
        const ccColor4F fill = ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, 0.35f);
        d->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), fill, 0.f, fill);
        d->setPosition(ccp(static_cast<float>(it.x), static_cast<float>(it.y)));
        ov.world->addChild(d, 5);
        d->runAction(CCSequence::create(CCScaleTo::create(0.3f, 3.2f), CCRemoveSelf::create(), nullptr));
    }

    void updateOverlayProgress(PlayLayer* pl) {
        if (!ov.world || ov.items.empty()) return;
        const double x = playerX(pl->m_player1);
        const double eps = 0.001;
        const int n = static_cast<int>(std::upper_bound(ov.items.begin(), ov.items.end(), x + eps,
                                                        [](double v, const fw::OverlayItem& it) { return v < it.x; }) - ov.items.begin());
        if (n == ov.passed) { ov.lastX = x; return; }
        const bool forward = n > ov.passed && x >= ov.lastX;
        if (forward && n - ov.passed <= 3) {
            for (int k = ov.passed; k < n; ++k) pulse(ov.items[k]);
        }
        ov.passed = n;  // a respawn / restart (n went down, or a big jump) just resyncs silently
        ov.lastX = x;
        refreshHud();
    }

    void refreshOverlay(PlayLayer* pl) {
        auto mod = Mod::get();
        const bool want = windowsMode() >= 1 && g.phase == Phase::Idle;
        if (!want) {
            if (ov.world || ov.hud || !ov.key.empty()) clearOverlay();
            return;
        }
        const std::string macro = mod->getSavedValue<std::string>("selected-macro", "");
        const std::string key = macro + "|" + std::to_string(g.overlayRevision) + "|" + (mod->getSettingValue<bool>("overlay-hud") ? "h" : "n");
        if (ov.key == key && (!ov.world || ov.world->getParent() == pl->m_objectLayer)) return;

        clearOverlay();
        ov.key = key;  // remembered even when nothing loads, so the message below is shown once, not every frame
        if (macro.empty()) return;

        std::string raw;
        if (!readFile(resultsCsv(macro), raw)) {
            toast("No saved windows for " + macro + " - open Frame Window and press Analyze", NotificationIcon::Warning);
            return;
        }
        fw::SavedResults sr = fw::parseResultsCsv(raw);
        if (sr.items.empty()) {
            toast(sr.oldFormat ? "Saved windows of " + macro + " are from an old version - Analyze again"
                               : "Saved windows of " + macro + " are empty - Analyze again",
                  NotificationIcon::Warning);
            return;
        }
        const std::string playing = std::string(pl->m_level->m_levelName);
        if (!sr.level.empty() && lower(sr.level) != lower(playing))
            toast("Windows were analysed on level \"" + sr.level + "\" (you are on \"" + playing + "\")", NotificationIcon::Warning);

        ov.items = sr.items;
        std::sort(ov.items.begin(), ov.items.end(), [](const fw::OverlayItem& l, const fw::OverlayItem& r) { return l.x < r.x; });
        ov.passed = static_cast<int>(std::upper_bound(ov.items.begin(), ov.items.end(), playerX(pl->m_player1) + 0.001,
                                                      [](double v, const fw::OverlayItem& it) { return v < it.x; }) - ov.items.begin());
        ov.lastX = playerX(pl->m_player1);

        auto world = CCNode::create();
        auto rings = CCDrawNode::create();
        for (const fw::OverlayItem& it : ov.items) {
            const fw::Rgb c = fw::widthColor(it.width);
            const ccColor4F col = ccc4f(c.r / 255.f, c.g / 255.f, c.b / 255.f, 1.f);
            auto pts = circlePoints(static_cast<float>(it.x), static_cast<float>(it.y), 7.5f);
            rings->drawPolygon(pts.data(), static_cast<unsigned int>(pts.size()), ccc4f(0.f, 0.f, 0.f, 0.f), 2.4f, col);  // hollow ring

            auto label = CCLabelBMFont::create(fw::markerText(it).c_str(), "bigFont.fnt");
            label->setColor({c.r, c.g, c.b});
            label->setScale(0.4f);
            label->setPosition({static_cast<float>(it.x), static_cast<float>(it.y) + 19.f});
            world->addChild(label, 2);
        }
        world->addChild(rings, 1);
        pl->m_objectLayer->addChild(world, 1000);
        ov.world = world;

        if (mod->getSettingValue<bool>("overlay-hud")) {
            auto hud = CCNode::create();
            const auto win = CCDirector::sharedDirector()->getWinSize();
            for (int i = 0; i < 5; ++i) {
                auto label = CCLabelBMFont::create("0", "bigFont.fnt");
                const fw::Rgb c = fw::widthColor(i + 1);
                label->setColor({c.r, c.g, c.b});
                label->setScale(0.5f);
                label->setAnchorPoint({0.f, 1.f});
                label->setPosition({8.f, win.height - 40.f - 17.f * i});
                hud->addChild(label);
                ov.hudLabels[i] = label;
            }
            pl->addChild(hud, 1000);
            ov.hud = hud;
            refreshHud();
        }
    }

    // ---- playback: run the selected macro by position in normal play (mode 2)
    struct Playback {
        bool active = false;
        std::string key;
        std::vector<fw::MacroAction> actions;
        fw::Cursor cursor;
    } pb;

    void injectFires(GJBaseGameLayer* layer, const std::vector<fw::Fire>& fires) {
        g.injecting = true;
        for (const fw::Fire& f : fires) layer->handleButton(f.down, static_cast<int>(PlayerButton::Jump), !f.p2);
        g.injecting = false;
    }

    // after a respawn / restart / late start: continue from the player's position (re-presses if mid-hold)
    void seekPlayback(PlayLayer* pl) {
        std::vector<fw::Fire> fires;
        pb.cursor.seekTo(playerX(pl->m_player1), fires);
        injectFires(pl, fires);
    }

    void refreshPlayback(PlayLayer* pl) {
        auto mod = Mod::get();
        const std::string macro = mod->getSavedValue<std::string>("selected-macro", "");
        const bool want = windowsMode() == 2 && g.phase == Phase::Idle && !macro.empty();
        if (!want) {
            pb.active = false;
            pb.key.clear();
            return;
        }
        if (pb.key == macro) return;  // loaded (or failed once) already
        pb.key = macro;
        pb.active = false;

        std::string raw;
        if (!readFile(resolveMacrosDir() / macro, raw)) {
            toast("Cannot read macro " + macro, NotificationIcon::Error);
            return;
        }
        fw::MacroFile mf = fw::parseMacro(raw);
        if (!mf.ok()) {
            toast("Macro " + macro + " has errors (line " + std::to_string(mf.errors.empty() ? 0 : mf.errors[0].line) + ")", NotificationIcon::Error);
            return;
        }
        pb.actions = mf.actions;
        pb.cursor.bind(&pb.actions, mod->getSettingValue<double>("x-epsilon"));
        pb.active = true;
        seekPlayback(pl);
        toast("Playing macro " + macro);
    }

    // ---------------------------------------------------------------- progress label

    void ensureLabel() {
        auto scene = CCDirector::sharedDirector()->getRunningScene();
        if (!scene) return;
        if (!g.label) {
            auto l = CCLabelBMFont::create("Frame Window", "bigFont.fnt");
            l->setScale(0.38f);
            l->setAnchorPoint({0.f, 1.f});
            l->setOpacity(200);
            g.label = l;
        }
        if (g.label->getParent() != scene) {
            g.label->removeFromParent();
            auto win = CCDirector::sharedDirector()->getWinSize();
            g.label->setPosition({8.f, win.height - 8.f});
            scene->addChild(g.label, 999999);
        }
    }

    void updateLabel() {
        if (!g.label || !g.analyzer) return;
        fw::Progress p = g.analyzer->progress();
        std::string t;
        if (p.currentNumber == 0) t = "Frame Window: reference run...";
        else {
            t = "Frame Window  " + std::to_string(p.groupsDone) + "/" + std::to_string(p.groupsTotal) + "  input #" +
                std::to_string(p.currentNumber) + "  offset " + (p.currentOffset >= 0 ? "+" : "") + std::to_string(p.currentOffset) +
                "  trials " + std::to_string(p.trials) + "  (pause = cancel)";
        }
        g.label->setString(t.c_str());
    }

    void removeLabel() {
        if (g.label) g.label->removeFromParent();
        g.label = nullptr;
    }

    // ---------------------------------------------------------------- session control

    void endSession(PlayLayer* pl, bool showReport, bool restartLevel) {
        g.phase = Phase::Idle;
#ifndef FW_NO_CHECKPOINTS
        g.cps.clear();
#endif
        g.trialActive = false;
        removeLabel();

        if (showReport && g.analyzer) {
            double tps = g.expectedTps;
            std::string warn;
            if (g.updateCalls > 0 && g.tickTotal > 0) {
                const double observed = static_cast<double>(g.tickTotal) / (static_cast<double>(g.updateCalls) * kStepDt);
                warn = "game ran at ~" + fw::fmt("%.0f", observed) + " ticks/s (expected " + fw::fmt("%.0f", g.expectedTps) + ")";
                if (std::fabs(observed - g.expectedTps) > g.expectedTps * 0.02) warn = "WARNING: " + warn + ". Windows use the measured rate.";
                tps = observed;
            }
            if (g.anticheatHits > 0) warn += std::string(warn.empty() ? "" : "; ") + "ignored " + std::to_string(g.anticheatHits) + " anti-cheat spike hits";
            fw::ReportStyle plain, tagged;
            plain.tps = tagged.tps = tps;
            tagged.tags = true;
            tagged.wrap = 50;
            std::string body;
            if (!warn.empty()) fw::addLine(body, tagged, warn.rfind("WARNING", 0) == 0 ? "cr" : "cy", warn);
            body += fw::formatReport(*g.analyzer, tagged, g.macroName);

            {
                // The CSV is the persistent store the on-screen markers load from, so it is ALWAYS written.
                std::error_code ec;
                fs::path dir = fs::path(Mod::get()->getSaveDir()) / "results";
                fs::create_directories(dir, ec);
                std::string stem = fs::path(g.macroName).stem().string();
                std::ofstream(dir / (stem + ".windows.csv")) << fw::formatCsv(*g.analyzer, tps);
                if (g.saveReport) {
                    std::ofstream(dir / (stem + ".windows.txt")) << warn << "\n" << fw::formatReport(*g.analyzer, plain, g.macroName);
                    body += "\nSaved: " + (dir / (stem + ".windows.txt")).string();
                } else {
                    body += "\nSaved: " + (dir / (stem + ".windows.csv")).string();
                }
                ++g.overlayRevision;
            }
            g.pendingTitle = "Frame Windows";
            g.pendingText = body;
        }
        g.analyzer.reset();
        if (restartLevel && pl) {
            g.resettingByUs = true;
            pl->resetLevelFromStart();
            g.resettingByUs = false;
        }
    }

    void cancelSession(PlayLayer* pl, const std::string& why, bool restartLevel) {
        if (g.analyzer) g.analyzer->cancel(why);
        endSession(pl, true, restartLevel);
    }

    void injectFires(GJBaseGameLayer* layer, const std::vector<fw::Fire>& fires);

    bool startNextTrial(PlayLayer* pl) {
        auto spec = g.analyzer->nextTrial();
        if (!spec) {
            endSession(pl, true, true);
            return false;
        }
        g.spec = std::move(*spec);
        g.cursor.bind(&g.spec.actions, g.eps);
        g.res = fw::TrialResult{};
        g.res.firedTick.assign(g.spec.actions.size(), -1);
        if (g.spec.recordX) g.res.xs.reserve(4096);
        g.lastFire = -1;
        g.idleCalls = 0;
        g.probe.clear();
        g.trialActive = true;

        bool restored = false;
#ifndef FW_NO_CHECKPOINTS
        if (g.spec.checkpointTick >= 0) {
            auto it = g.cps.find(g.spec.checkpointTick);
            if (it != g.cps.end() && it->second) {
                g.resettingByUs = true;
                pl->loadFromCheckpoint(it->second);  // state at the end of that tick: ticks stay absolute
                g.resettingByUs = false;
                g.tick = g.spec.checkpointTick + 1;
                restored = true;
            }
        }
#endif
        if (!restored) {
            g.tick = 0;
            g.resettingByUs = true;
            pl->resetLevelFromStart();
            g.resettingByUs = false;
        }

        // nothing may stay held from the previous trial; then continue the macro from the player's position
        g.injecting = true;
        pl->handleButton(false, static_cast<int>(PlayerButton::Jump), true);
        pl->handleButton(false, static_cast<int>(PlayerButton::Jump), false);
        g.injecting = false;
        if (restored) {
            std::vector<fw::Fire> fires;
            g.cursor.seekTo(playerX(pl->m_player1), fires);  // re-presses if the macro was mid-hold
            injectFires(pl, fires);
        }
        return true;
    }

    bool trialEnded() {
        if (g.res.died || g.res.completed || g.res.aborted) return true;
        if (g.spec.endTick >= 0 && g.tick > g.spec.endTick) return true;
        if (g.spec.stopAfterLastFire >= 0 && g.cursor.done() && g.lastFire >= 0 && g.tick >= g.lastFire + g.spec.stopAfterLastFire) return true;
        if (g.tick >= g.spec.maxTick) {
            g.res.timedOut = true;
            return true;
        }
        return false;
    }

    void finishTrial() {
        g.trialActive = false;
        g.res.ticksRun = g.tick;
        g.analyzer->submit(g.res);
    }

    // ---------------------------------------------------------------- macro picker

    void startAnalysis(PauseLayer* pause, const std::string& fileName);

    struct MenuEntry {
        std::string name;
        int clicks = 0;
        double avg = 0.0;
        bool hasResults = false;  // current-format results with at least one click
        bool oldFormat = false;   // results from an older version (no y column)
        bool canDelete() const { return hasResults || oldFormat; }
    };

    struct MenuState {
        Ref<FLAlertLayer> alert;
        std::vector<MenuEntry> entries;
        std::vector<Ref<CCNode>> nodes;
        int page = 0;
    } mn;

    std::vector<MenuEntry> loadMenuEntries() {
        std::vector<MenuEntry> out;
        for (const std::string& name : listMacros(resolveMacrosDir())) {
            MenuEntry e;
            e.name = name;
            std::string raw;
            if (readFile(resultsCsv(name), raw)) {
                fw::SavedResults sr = fw::parseResultsCsv(raw);
                e.oldFormat = sr.oldFormat && sr.items.empty();
                e.hasResults = !sr.items.empty();
                e.clicks = static_cast<int>(sr.items.size());
                e.avg = fw::averageWidth(sr.items);
            }
            out.push_back(e);
        }
        return out;
    }

    void startAnalysis(PauseLayer* pause, const std::string& fileName) {
        auto pl = PlayLayer::get();
        if (!pl) { showInfo("Frame Window", "Open a level first."); return; }

        fs::path path = resolveMacrosDir() / fileName;
        std::string raw;
        if (!readFile(path, raw)) { showInfo("Frame Window", "Cannot read\n" + path.string()); return; }
        fw::MacroFile mf = fw::parseMacro(raw);
        if (!mf.ok()) {
            std::string msg = "This macro cannot be analysed:\n";
            for (size_t i = 0; i < mf.errors.size() && i < 6; ++i)
                msg += "line " + std::to_string(mf.errors[i].line) + ": " + mf.errors[i].message + "\n";
            if (mf.errors.size() > 6) msg += "... " + std::to_string(mf.errors.size() - 6) + " more";
            showInfo("Frame Window", msg);
            return;
        }

        auto mod = Mod::get();
        g.speed = std::clamp(static_cast<int>(mod->getSettingValue<int64_t>("speed")), 1, 64);
        g.eps = mod->getSettingValue<double>("x-epsilon");
        g.expectedTps = static_cast<double>(mod->getSettingValue<int64_t>("expected-tps"));
        g.saveReport = mod->getSettingValue<bool>("save-report");
        g.ignoreEarlyTicks = static_cast<int>(mod->getSettingValue<int64_t>("ignore-early-death-ticks"));
        g.macroName = fileName;
        g.tickTotal = 0;
        g.updateCalls = 0;
        g.anticheatHits = 0;
        g.trialActive = false;
        fw::AnalyzerSettings settings = readSettings();
#ifdef FW_NO_CHECKPOINTS
        settings.useCheckpoints = false;
#else
        settings.useCheckpoints = settings.useCheckpoints && pl->m_isPracticeMode;  // checkpoints are a practice-mode feature
#endif
#ifndef FW_NO_CHECKPOINTS
        g.cps.clear();
#endif
        if (settings.useCheckpoints) toast("Practice mode: fast checkpoint analysis (your own checkpoints get cleared)");
        else if (readSettings().useCheckpoints) toast("Not in practice mode: analysing from the level start (slower)", NotificationIcon::Warning);
        g.analyzer = std::make_unique<fw::Analyzer>(std::move(mf), settings);
        g.phase = Phase::Running;  // the next update() starts the reference run
        pause->onResume(nullptr);
    }

}  // namespace

// ------------------------------------------------------------------ physics tick hooks
class $modify(FWBaseLayer, GJBaseGameLayer) {
    // One call = one physics tick. The tick index and the player's x BEFORE the tick are what the macro format is about.
    // 2.2081 signature: (dt, isHalfTick, isLastTick). Half ticks are not physics ticks of the macro's 240 TPS,
    // so they pass straight through; if the report says the game ran at ~480 ticks/s, flip this condition.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (isHalfTick || static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) {
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        if (!g.trialActive) {
            if (pb.active) {  // normal play with the macro running by position
                std::vector<fw::Fire> fires;
                pb.cursor.poll(playerX(m_player1), fires);
                if (!fires.empty()) injectFires(this, fires);
            }
            GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
            return;
        }
        const double x = playerX(m_player1);
        if (g.spec.recordX) {
            g.res.xs.push_back(x);
            g.res.ys.push_back(m_player1 ? static_cast<double>(m_player1->getPositionY()) : 0.0);
            if (g.tick < 8) {
                g.probe += "t" + std::to_string(g.tick) + " x=" + fw::fmt("%.2f", x) + " y=" +
                           fw::fmt("%.1f", m_player1 ? static_cast<double>(m_player1->getPositionY()) : 0.0) + "; ";
            }
        }

        std::vector<fw::Fire> fires;
        g.cursor.poll(x, fires);
        for (const fw::Fire& f : fires) {
            g.res.firedTick[f.pos] = g.tick;
            g.lastFire = g.tick;
            g.injecting = true;
            this->handleButton(f.down, static_cast<int>(PlayerButton::Jump), !f.p2);
            g.injecting = false;
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
#ifndef FW_NO_CHECKPOINTS
        // reference run: a checkpoint at the END of every tick in which inputs fired = "right after the release of click N"
        if (g.spec.captureCheckpoints && !fires.empty()) {
            if (PlayLayer* pl = PlayLayer::get()) {
                if (CheckpointObject* cp = pl->createCheckpoint()) {
                    g.cps[g.tick] = cp;
                    g.res.checkpointTicks.push_back(g.tick);
                }
            }
        }
#endif
        ++g.tick;
    }

    // While analysing, real touches must not leak into the run.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (g.phase == Phase::Running && !g.injecting && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void update(float dt) {
        if (g.inFrame) {
            GJBaseGameLayer::update(dt);
            return;
        }
        PlayLayer* pl = PlayLayer::get();
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) {
            GJBaseGameLayer::update(dt);
            return;
        }
        if (!g.pendingText.empty()) {
            std::string title = g.pendingTitle, text = g.pendingText;
            g.pendingText.clear();
            FLAlertLayer::create(nullptr, title.c_str(), text, "Close", nullptr, 420.f, true, 250.f, 0.65f)->show();
        }
        refreshOverlay(pl);
        updateOverlayProgress(pl);
        refreshPlayback(pl);
        if (g.phase != Phase::Running || !g.analyzer) {
            GJBaseGameLayer::update(dt);
            return;
        }

        ensureLabel();
        int budget = g.speed;
        while (budget-- > 0 && g.phase == Phase::Running) {
            if (!g.trialActive && !startNextTrial(pl)) break;
            const int before = g.tick;
            g.inFrame = true;
            pl->update(kStepDt);  // whole PlayLayer frame, not just the base-layer part
            g.inFrame = false;
            ++g.updateCalls;
            g.tickTotal += g.tick - before;
            if (g.tick == before) {
                if (++g.idleCalls > 600) {
                    cancelSession(pl, "no physics ticks observed - the tick hook is not firing on this game version", false);
                    break;
                }
            } else {
                g.idleCalls = 0;
            }
            if (trialEnded()) finishTrial();  // the next iteration asks the analyzer for more, or ends the session
        }
        updateLabel();
    }
};

// ------------------------------------------------------------------ death / completion / pause / quit
class $modify(FWPlayLayer, PlayLayer) {
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (g.trialActive && static_cast<GJBaseGameLayer*>(PlayLayer::get()) == static_cast<GJBaseGameLayer*>(this)) {
            // The game's hidden anti-cheat spike sits on the spawn point (id 8 at x=0, y=105). Our fast fixed-step loop trips it
            // and it would "kill" the player every tick. Analysis runs never reach a completion screen and submit nothing,
            // so during analysis ONLY it is ignored (and counted in the report). Normal play is untouched.
            if (obj && obj->m_objectID == 8 && std::fabs(obj->getPositionX()) < 0.5 && std::fabs(obj->getPositionY() - 105.0) < 0.5) {
                ++g.anticheatHits;
                return;
            }
            if (g.tick < g.ignoreEarlyTicks) return;  // user-chosen workaround for spawn glitches
            if (!g.res.died) {
                g.res.died = true;
                g.res.deathTick = g.tick;  // tick in progress (g.tick is incremented after processCommands)
                g.res.deathX = playerX(player);
                std::string where = "no object (out of bounds / script)";
                if (obj) {
                    where = "object id " + std::to_string(obj->m_objectID) + " at (" + fw::fmt("%.1f", static_cast<double>(obj->getPositionX())) +
                            "," + fw::fmt("%.1f", static_cast<double>(obj->getPositionY())) + ")";
                }
                g.res.deathNote = where + ", player at (" + fw::fmt("%.1f", playerX(player)) + "," +
                                  fw::fmt("%.1f", player ? static_cast<double>(player->getPositionY()) : 0.0) + ")" +
                                  (player == this->m_player2 ? " P2" : "") + (g.probe.empty() ? "" : " | first ticks: " + g.probe);
            }
            return;  // no explosion, no death screen, no respawn
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void levelComplete() {
        if (g.trialActive) {
            g.res.completed = true;
            return;
        }
        PlayLayer::levelComplete();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (g.trialActive && !g.resettingByUs) g.res.aborted = true;  // the game restarted by itself: run is meaningless
        if (pb.active && !g.trialActive) seekPlayback(this);          // respawn / restart: continue from where the player is
    }

    void pauseGame(bool b) {
        if (g.phase == Phase::Running) cancelSession(this, "cancelled (paused)", false);
        PlayLayer::pauseGame(b);
    }

    void onQuit() {
        if (g.phase == Phase::Running) endSession(nullptr, false, false);
        clearOverlay();
        pb.active = false;
        pb.key.clear();
        PlayLayer::onQuit();
    }
};

// ------------------------------------------------------------------ pause menu: button + the macro/windows menu
class $modify(FWPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = ButtonSprite::create("Frame Window");
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(FWPause::onFrameWindow));
        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({78.f, 56.f});
        this->addChild(menu);

        if (Mod::get()->getSettingValue<bool>("settings-button")) {
            auto spr2 = ButtonSprite::create("FW Settings");
            spr2->setScale(0.6f);
            auto btn2 = CCMenuItemSpriteExtra::create(spr2, this, menu_selector(FWPause::onFrameWindowSettings));
            auto menu2 = CCMenu::create();
            menu2->addChild(btn2);
            menu2->setPosition({78.f, 24.f});
            this->addChild(menu2);
        }
    }

    void onFrameWindowSettings(CCObject*) { openSettingsPopup(Mod::get()); }

    void onFrameWindow(CCObject*) {
        mn.entries = loadMenuEntries();
        if (mn.entries.empty()) {
            showInfo("Frame Window", "No .txt macros found in:\n" + resolveMacrosDir().string() +
                                         "\n\nRecord one with percent-macro, or set Macros folder in this mod's settings.");
            return;
        }
        mn.page = 0;
        mn.nodes.clear();
        std::string blank;
        for (int i = 0; i < 11; ++i) blank += "\n";  // gives the panel its height; the rows are drawn on top
        FLAlertLayer* alert = FLAlertLayer::create(nullptr, "Frame Windows", blank, "Close", nullptr, 440.f, false, 0.f, 1.f);
        mn.alert = alert;
        fwBuildMenu();
        alert->show();
    }

    // (re)draw the current page of the list inside the alert
    void fwBuildMenu() {
        for (auto& n : mn.nodes) if (n) n->removeFromParent();
        mn.nodes.clear();
        FLAlertLayer* alert = mn.alert;
        if (!alert) return;

        const auto win = CCDirector::sharedDirector()->getWinSize();
        // positions are given relative to the screen centre and converted into each parent's own space
        auto at = [&](CCNode* parent, float dx, float dy) { return parent->convertToNodeSpace(ccp(win.width / 2 + dx, win.height / 2 + dy)); };
        auto label = [&](const std::string& text, const char* font, float scale, float dx, float dy, ccColor3B col, bool left) {
            auto l = CCLabelBMFont::create(text.c_str(), font);
            l->setScale(scale);
            l->setColor(col);
            if (left) l->setAnchorPoint({0.f, 0.5f});
            l->setPosition(at(alert->m_mainLayer, dx, dy));
            alert->m_mainLayer->addChild(l, 10);
            mn.nodes.push_back(l);
        };
        auto button = [&](const char* text, float scale, float dx, float dy, int tag, SEL_MenuHandler handler) {
            auto spr = ButtonSprite::create(text);
            spr->setScale(scale);
            auto b = CCMenuItemSpriteExtra::create(spr, this, handler);
            b->setTag(tag);
            b->setPosition(at(alert->m_buttonMenu, dx, dy));
            alert->m_buttonMenu->addChild(b);
            mn.nodes.push_back(b);
        };

        const std::string selected = Mod::get()->getSavedValue<std::string>("selected-macro", "");
        const int mode = windowsMode();
        std::string now = mode == 0 ? "off" : (mode == 1 ? "windows shown" : "macro playing + windows");
        label("Now: " + (selected.empty() ? std::string("-") : selected) + "  [" + now + "]", "bigFont.fnt", 0.3f, 0.f, 100.f, ccc3(255, 255, 255), false);

        const int perPage = 4;
        const int total = static_cast<int>(mn.entries.size());
        const int pages = std::max(1, (total + perPage - 1) / perPage);
        mn.page = std::clamp(mn.page, 0, pages - 1);
        for (int r = 0; r < perPage; ++r) {
            const int i = mn.page * perPage + r;
            if (i >= total) break;
            const MenuEntry& e = mn.entries[i];
            const float dy = 68.f - 34.f * r;
            std::string shown = e.name.size() > 18 ? e.name.substr(0, 16) + ".." : e.name;
            const bool isSel = e.name == selected && mode > 0;
            label(shown, "bigFont.fnt", 0.4f, -208.f, dy + 7.f, isSel ? ccc3(110, 255, 110) : ccc3(255, 255, 255), true);
            std::string st = e.hasResults ? std::to_string(e.clicks) + " clicks, avg " + fw::fmt("%.1f", e.avg) + "F"
                             : e.oldFormat ? std::string("old results - Analyze again")
                                           : std::string("not analysed yet");
            label(st, "chatFont.fnt", 0.5f, -208.f, dy - 8.f, ccc3(170, 200, 255), true);
            button("Analyze", 0.4f, 8.f, dy, i, menu_selector(FWPause::onMenuAnalyze));
            button("Show", 0.4f, 70.f, dy, i, menu_selector(FWPause::onMenuShow));
            button("Play", 0.4f, 116.f, dy, i, menu_selector(FWPause::onMenuPlay));
            if (e.canDelete()) button("Del", 0.4f, 160.f, dy, i, menu_selector(FWPause::onMenuDelete));
        }

        label("page " + std::to_string(mn.page + 1) + "/" + std::to_string(pages), "chatFont.fnt", 0.6f, 0.f, -70.f, ccc3(255, 255, 255), false);
        if (pages > 1) {
            button("<", 0.45f, -60.f, -70.f, 0, menu_selector(FWPause::onMenuPrev));
            button(">", 0.45f, 60.f, -70.f, 0, menu_selector(FWPause::onMenuNext));
        }
        button("Off", 0.45f, 160.f, -70.f, 0, menu_selector(FWPause::onMenuOff));
    }

    int fwTag(CCObject* sender) { return static_cast<CCNode*>(sender)->getTag(); }

    void fwCloseMenu() {
        if (!mn.alert) return;
        mn.alert->setKeypadEnabled(false);
        mn.alert->setTouchEnabled(false);
        mn.alert->removeFromParentAndCleanup(true);
    }

    void onMenuPrev(CCObject*) { --mn.page; fwBuildMenu(); }
    void onMenuNext(CCObject*) { ++mn.page; fwBuildMenu(); }

    void onMenuOff(CCObject*) {
        setWindowsMode(0);
        toast("Windows / playback: OFF");
        fwBuildMenu();
    }

    void onMenuAnalyze(CCObject* sender) {
        const int i = fwTag(sender);
        if (i < 0 || i >= static_cast<int>(mn.entries.size())) return;
        const std::string name = mn.entries[i].name;
        Mod::get()->setSavedValue<std::string>("selected-macro", name);
        fwCloseMenu();
        startAnalysis(this, name);
    }

    void fwSelect(int i, int mode) {
        if (i < 0 || i >= static_cast<int>(mn.entries.size())) return;
        const MenuEntry& e = mn.entries[i];
        if (!e.hasResults) {
            toast(e.oldFormat ? e.name + ": old results, press Analyze again" : e.name + ": no windows yet, press Analyze first",
                  NotificationIcon::Warning);
            if (mode == 1) return;  // nothing to show; Play still runs the macro without markers
        }
        Mod::get()->setSavedValue<std::string>("selected-macro", e.name);
        pb.key.clear();  // force a reload of the macro
        setWindowsMode(mode);
        toast((mode == 2 ? "Playing + windows: " : "Showing windows: ") + e.name + "  (resume the level)");
        fwBuildMenu();
    }

    void onMenuShow(CCObject* sender) { fwSelect(fwTag(sender), 1); }
    void onMenuPlay(CCObject* sender) { fwSelect(fwTag(sender), 2); }

    void onMenuDelete(CCObject* sender) {
        const int i = fwTag(sender);
        if (i < 0 || i >= static_cast<int>(mn.entries.size())) return;
        const std::string name = mn.entries[i].name;
        createQuickPopup("Delete windows", "Delete the saved windows of <cy>" + name + "</c>?\n(the macro file itself is NOT touched)", "No", "Delete",
                         [this, name](FLAlertLayer*, bool yes) {
                             if (!yes) return;
                             std::error_code ec;
                             const std::string stem = fs::path(name).stem().string();
                             fs::remove(resultsDir() / (stem + ".windows.csv"), ec);
                             fs::remove(resultsDir() / (stem + ".windows.txt"), ec);
                             if (Mod::get()->getSavedValue<std::string>("selected-macro", "") == name) setWindowsMode(0);
                             else ++g.overlayRevision;
                             mn.entries = loadMenuEntries();
                             toast("Deleted windows of " + name);
                             fwBuildMenu();
                         });
    }
};
