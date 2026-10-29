# Percent Frame Window — Geode mod (Android)

Frame window counter at 240 TPS for position-based **percent-macro v2** files
(`x percent down p2`). For each click and release it answers: *how many ticks earlier or later could this input happen and still survive?*

```
# example output format (numbers are illustrative, not measured):
#52 press   x=1000.961  [-2,+2] 5F 20.8ms  stable
#41 release x=816.605   [-1,+3N] 5F ...    stable      (N = limited by the neighbouring input)
```

## Use

1. Build it (below) and install the `.geode`.
2. Open the level the macro was recorded on, **pause**, tap **Frame Window**.
3. **Next file** cycles through `.../mods/yourname.percentmacro/macros/*.txt`, **Analyze** starts.
   (Folder lookup order: *Macros folder* setting → percent-macro mod's save dir → sibling of this mod's save dir → the fixed Android path.)
4. The level is restarted hundreds of times at high speed; a label shows progress. Pause = cancel (partial results are kept).
5. A scrolling report appears and is saved to `<this mod's save dir>/results/<macro>.windows.txt` and `.csv`.

> Every trial restarts the level, so **attempts/jumps stats go up**. Use a copy of the level.

## How a window is measured

1. **Reference run**: replay the untouched macro from tick 0, record the player's x before every tick and the tick each input fired on.
2. For input at reference tick `T` and offset `d`: take `x = xs[T+d]`, move the input to that x (macro stays position based), replay from tick 0.
3. **PASS** = survive until a fixed end tick (same for every offset): `max(T + horizon-min-ticks, tick of the Nth following input + 6)`, capped by where the reference itself died.
4. Search: test 0, expand `+1, +2…` until the first FAIL, then `-1, -2…`, probe past each FAIL (finds islands), then re-run each boundary once more. Typical cost ≈ 13 trials per input instead of 17.
5. Output `[lo,hi]` and width in ticks and ms.

Edge letters: `R` scan radius · `N` neighbouring input · `S` level start · `E` end of reference · `P` x not growing. A letter means "no FAIL found before this limit", so the true window may be wider.
Flags: `UNSTABLE` (same offset gave different results) · `ISLAND` (a PASS beyond the first FAIL, not merged) · `DRIFT` (input fired on another tick than planned, e.g. speed portals changed the path) · `BASE-FAIL` (original timing doesn't pass) · `HORIZON-CAPPED`.

## About your macro (`spamy_wVs`, checked by script)

* 138 actions: 66 presses, 72 releases; sorted; 1 same-x pair (#41 release + #42 press at x=816.605, one tick after the press at #40) → analysed as one **tap**.
* 6 inputs change nothing (#1 release at x=0, and the second release in #60, #77, #114, #127, #134). They are listed as *no-op* and skipped.
* One tick = 1.298 units, i.e. 1x speed at 240 TPS.
* Your header lines were pasted without `#`; the parser accepts that. `1000.961` is action **#52** (the "#53" in your notes is the release after it).
* The `percent` column drifts from x in a few places, so it is ignored, exactly as the format says.

## Your bug list → what the code does

| # | Problem | Handling |
|---|---|---|
| 1 | bad lines shift values | trim, split on whitespace, exactly 4 numbers, strict locale-free parse; any bad line = analysis refuses and shows line numbers; UTF-8 BOM, CRLF, lost `#` tolerated |
| 2 | float compare on x | `x >= action.x - epsilon` (setting), never `==` |
| 3 | same-x actions | grouped per player into one event; moved together, order kept |
| 4 | hold desync after respawn | every trial restarts from tick 0 and the game's own restart aborts the trial (no mid-hold state to restore) |
| 5 | wrong action after respawn | same: no respawn, playback cursor always starts at action 0 |
| 6 | pass/fail/pass | window = contiguous passes from the original timing; passes beyond a fail are flagged `ISLAND`, not merged |
| 7 | P1/P2 mixed | actions split by player; neighbours and hold state are per player |
| 8 | speed changes | candidate x comes from the real reference run; if the input then fires on another tick → `DRIFT`; x not growing → clipped `P` / unmappable |
| 9 | checkpoint false passes | trials always start from the level start, never from a checkpoint |
| 10 | off-by-one range | offsets `-R…+R` inclusive, tested on a fake level; death in the last batch ticks after the end tick is ignored |
| 11 | unsorted file | stable sort in memory + warning |
| 12 | edge frame noise | boundaries are run twice; disagreement shrinks the window and flags `UNSTABLE` |

Extra ones handled: input moved past its neighbour (clipped, `N`), redundant inputs (no-op), reference macro that dies (later inputs `unreachable`, horizon capped), late deaths (horizon setting), real touches during analysis (ignored), the game restarting by itself (trial aborted), no tick hook firing (cancelled after a watchdog), game not running at the expected TPS (warning, windows labelled with measured TPS).

## Tests (run on a PC, no Geode needed)

```
./tests/run_tests.sh      # 81 checks, ASan+UBSan: parser, grouping, cursor, window search,
                          # full analyzer against a fake game, and the real spamy_wVs file
```

## Build

Same as Show Tap: push the folder to GitHub, run **Build Percent Frame Window for Android**, download the `.geode` artifact.
Local: `geode sdk install-binaries -p android64 && geode build -p android64`.

## Not verified — read this

This was **not compiled against the Geode SDK or run in Geometry Dash** (no SDK here). The core logic is tested; the glue in `src/main.cpp` relies on these game hooks, which exist in the Geode bindings as far as I know but could differ on 2.2081/Android:

* `GJBaseGameLayer::processCommands(float)` runs once per physics tick; `handleButton(bool,int,bool)`; `update(float)`
* `PlayLayer::destroyPlayer`, `levelComplete`, `resetLevel`, `resetLevelFromStart`, `pauseGame`, `onQuit`
* player x is read with `getPositionX()` (function `playerX()` in `main.cpp`); if windows look shifted by a tick, that is the place to look
* `createQuickPopup`, `FLAlertLayer::create(delegate,title,desc,btn1,btn2,width,scroll,height,scale)`

First thing to check in-game: the report header. If it says the untouched macro **dies**, the reference replay doesn't match the recording (other level/version, other firing point), and the windows after that point are not trustworthy. If the build fails, send the full error log.
