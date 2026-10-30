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
#include <memory>
#include <sstream>

#include "analyzer.hpp"
#include "macro.hpp"
#include "overlay.hpp"
#include "playback.hpp"
#include "report.hpp"

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
        const double tps = static_cast<double>(mod->getSettingValue<int64_t>("expected-tps"));
        s.maxRunTicks = static_cast<int>(tps * 180.0);  // 3 minutes of game time per run
        return s;
    }

    void showInfo(const char* title, const std::string& text) {
        FLAlertLayer::create(title, text, "OK")->show();
    }

    // ---------------------------------------------------------------- on-level window markers (saved results)

    struct OverlayState {
        Ref<CCNode> world;  // number above every analysed click, scrolls with the level
        Ref<CCNode> hud;    // 1 / 2 / 3 / 4 / 5+ counter, fixed on screen
        std::string key;
    } ov;

    void clearOverlay() {
        if (ov.world) ov.world->removeFromParent();
        if (ov.hud) ov.hud->removeFromParent();
        ov.world = nullptr;
        ov.hud = nullptr;
        ov.key.clear();
    }

    void refreshOverlay(PlayLayer* pl) {
        auto mod = Mod::get();
        const bool want = mod->getSettingValue<bool>("show-overlay") && g.phase == Phase::Idle;
        if (!want) {
            if (ov.world || ov.hud || !ov.key.empty()) clearOverlay();
            return;
        }
        const std::string macro = mod->getSavedValue<std::string>("selected-macro", "");
        const std::string key = macro + "|" + std::to_string(g.overlayRevision) + "|" + (mod->getSettingValue<bool>("overlay-hud") ? "h" : "n");
        if (ov.key == key && (!ov.world || ov.world->getParent() == pl->m_objectLayer)) return;

        clearOverlay();
        ov.key = key;  // remembered even when nothing loads, so a missing file is not retried every frame
        if (macro.empty()) return;

        fs::path csv = fs::path(mod->getSaveDir()) / "results" / (fs::path(macro).stem().string() + ".windows.csv");
        std::string raw;
        if (!readFile(csv, raw)) return;
        fw::SavedResults sr = fw::parseResultsCsv(raw);
        if (sr.items.empty()) return;
        const std::string playing = std::string(pl->m_level->m_levelName);
        if (!sr.level.empty() && lower(sr.level) != lower(playing)) return;  // results belong to another level

        auto world = CCNode::create();
        for (const fw::OverlayItem& it : sr.items) {
            auto label = CCLabelBMFont::create(fw::markerText(it).c_str(), "bigFont.fnt");
            const fw::Rgb c = fw::widthColor(it.width);
            label->setColor({c.r, c.g, c.b});
            label->setScale(0.45f);
            label->setPosition({static_cast<float>(it.x), static_cast<float>(it.y) + 22.f});
            world->addChild(label);
        }
        pl->m_objectLayer->addChild(world, 1000);
        ov.world = world;

        if (mod->getSettingValue<bool>("overlay-hud")) {
            auto hud = CCNode::create();
            int h[5];
            fw::histogram(sr.items, h);
            const char* names[5] = {"1:", "2:", "3:", "4:", "5+:"};
            const auto win = CCDirector::sharedDirector()->getWinSize();
            for (int i = 0; i < 5; ++i) {
                auto label = CCLabelBMFont::create((std::string(names[i]) + "  " + std::to_string(h[i])).c_str(), "bigFont.fnt");
                const fw::Rgb c = fw::widthColor(i + 1);
                label->setColor({c.r, c.g, c.b});
                label->setScale(0.5f);
                label->setAnchorPoint({0.f, 1.f});
                label->setPosition({8.f, win.height - 40.f - 17.f * i});
                hud->addChild(label);
            }
            pl->addChild(hud, 1000);
            ov.hud = hud;
        }
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
        g.tick = 0;
        g.lastFire = -1;
        g.idleCalls = 0;
        g.probe.clear();
        g.trialActive = true;

        g.resettingByUs = true;
        pl->resetLevelFromStart();
        g.resettingByUs = false;

        // make sure nothing is held from the previous trial
        g.injecting = true;
        pl->handleButton(false, static_cast<int>(PlayerButton::Jump), true);
        pl->handleButton(false, static_cast<int>(PlayerButton::Jump), false);
        g.injecting = false;
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

    void openPicker(PauseLayer* pause) {
        fs::path dir = resolveMacrosDir();
        std::vector<std::string> files = listMacros(dir);

        if (files.empty()) {
            showInfo("Frame Window", "No .txt macros found in:\n" + dir.string() +
                                         "\n\nRecord one with percent-macro, or set Macros folder in this mod's settings.");
            return;
        }
        std::string sel = Mod::get()->getSavedValue<std::string>("selected-macro", "");
        size_t idx = 0;
        for (size_t i = 0; i < files.size(); ++i) if (files[i] == sel) idx = i;

        // Keep this popup SHORT (4-5 lines): a tall one pushes the buttons off small screens.
        std::string text = "<cg>" + files[idx] + "</c>  (" + std::to_string(idx + 1) + "/" + std::to_string(files.size()) + ")\n";
        if (files.size() > 1) text += "next: " + files[(idx + 1) % files.size()] + "\n";
        if (auto pl = PlayLayer::get()) {
            std::string raw;
            if (readFile(dir / files[idx], raw)) {
                fw::MacroFile mf = fw::parseMacro(raw);
                std::string playing = std::string(pl->m_level->m_levelName);
                if (!mf.levelName.empty() && lower(mf.levelName) != lower(playing))
                    text += "<cr>recorded on " + mf.levelName + ", playing " + playing + "</c>\n";
            }
        }
        text += "<co>Restarts the level many times - use a copy.</c>";

        Ref<PauseLayer> ref = pause;
        createQuickPopup("Frame Window", text, "Next file", "Analyze", [ref, files, idx](FLAlertLayer*, bool analyze) {
            if (!analyze) {
                Mod::get()->setSavedValue<std::string>("selected-macro", files[(idx + 1) % files.size()]);
                openPicker(ref);
            } else {
                Mod::get()->setSavedValue<std::string>("selected-macro", files[idx]);
                startAnalysis(ref, files[idx]);
            }
        });
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
        g.analyzer = std::make_unique<fw::Analyzer>(std::move(mf), readSettings());
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
        if (isHalfTick || !g.trialActive || static_cast<GJBaseGameLayer*>(PlayLayer::get()) != this) {
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
    }

    void pauseGame(bool b) {
        if (g.phase == Phase::Running) cancelSession(this, "cancelled (paused)", false);
        PlayLayer::pauseGame(b);
    }

    void onQuit() {
        if (g.phase == Phase::Running) endSession(nullptr, false, false);
        clearOverlay();
        PlayLayer::onQuit();
    }
};

// ------------------------------------------------------------------ pause menu buttons
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

        {
            auto spr3 = ButtonSprite::create("Show Windows");
            spr3->setScale(0.6f);
            auto btn3 = CCMenuItemSpriteExtra::create(spr3, this, menu_selector(FWPause::onToggleWindows));
            auto menu3 = CCMenu::create();
            menu3->addChild(btn3);
            menu3->setPosition({78.f, 88.f});
            this->addChild(menu3);
        }

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

    void onToggleWindows(CCObject*) {
        const bool now = !Mod::get()->getSettingValue<bool>("show-overlay");
        Mod::get()->setSettingValue<bool>("show-overlay", now);
        Notification::create(now ? "Frame windows: ON" : "Frame windows: OFF", NotificationIcon::Success, 1.f)->show();
    }
    void onFrameWindow(CCObject*) { openPicker(this); }
    void onFrameWindowSettings(CCObject*) { openSettingsPopup(Mod::get()); }
};
