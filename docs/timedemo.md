# Timedemo: a recorded race as a benchmark

A timedemo is one race, recorded once, that the game then plays back by
itself as often as you like: the same cars, the same traffic, the same
camera, frame for frame, on any build and at any speed of the machine. Only
the time each frame takes changes from run to run, and that is what it
measures. Use it to see whether a change to the runtime or the renderer made
the game faster or slower, by how much, and where in the race.

It needs nothing new in the game: the retail `default.xex` still holds EA's
own record/replay system, "Joylog", switched off by a flag the game never
sets. SpeedBreaker sets it (`NFSMW_TIMEDEMO`). While recording, the game
writes everything a race depends on into a log: the pad, the time each frame
was given, the random seed and when each file finished loading. While
replaying, it reads all of that back instead of the pad and the clock, so the
simulation on frame N is the same in every run. The game also checks a
checksum of its own state every frame and stops the replay at the first
frame that differs; the report then says the run diverged, instead of
quietly measuring a different race.

What it doesn't do: it doesn't replay across game versions (only the
supported USA `default.xex`), and a log recorded on one build replays on
another only as long as nothing the game itself can see has changed (see
[Limits](#limits)).

## In short

On a Steam Deck in Desktop Mode, over SSH or in Konsole, from the repo (or
any copy of `scripts/`):

```sh
# Record: drive a Quick Race, about two minutes; the game exits by itself.
scripts/timedemo.sh capture race --bundle ~/sb/A --seconds 150

# Replay A and B in turns, three times each (the first call of each side
# replays once more before, as a warmup that isn't counted).
scripts/timedemo.sh run A 1 --bundle ~/sb/A --warmup NFSMW_TIMEDEMO_UNCAPPED=1
scripts/timedemo.sh run B 1 --bundle ~/sb/B --warmup NFSMW_TIMEDEMO_UNCAPPED=1
for i in 1 2; do
    scripts/timedemo.sh run A 1 --bundle ~/sb/A NFSMW_TIMEDEMO_UNCAPPED=1
    scripts/timedemo.sh run B 1 --bundle ~/sb/B NFSMW_TIMEDEMO_UNCAPPED=1
done

# Compare.
python3 scripts/bench_compare.py ~/sb-bench/runs/A-[0-9]* -- ~/sb-bench/runs/B-[0-9]*
```

`~/sb/A` and `~/sb/B` are two unpacked SteamOS downloads (or
`scripts/steamos/make_bundle.sh` folders) of the two builds. Without
`--bundle`, the script runs the dev build, `build/main/runtime/SpeedBreaker`
(`--build <dir>` for another build folder's, `<dir>/runtime/SpeedBreaker`),
from the repo root (on a Steam Deck set up as in
[scripts/steamos/README.md](../scripts/steamos/README.md), run the script
inside the `nfsmw-build` distrobox: `distrobox enter nfsmw-build -- scripts/timedemo.sh ...`).

## 1. Record a race

```sh
scripts/timedemo.sh capture <label> [--race <path.jlg>] [--seconds <n>] [--input-script '<taps>'] [options] [VAR=value...]
```

The game starts as usual. Go to a Quick Race (a circuit of two laps is a good
benchmark: traffic, cops off, the whole track) and drive. With `--seconds
150` the recording stops 150 seconds after the race has loaded and the game
exits by itself; without it, quit the game when you are done (the recording
ends there, the menus after the race included).

Everything the game does is recorded, including the menus, so the replay
goes through the same menus too: you only drive once. To keep the menus
short (they are part of every replay's run time, though not of what is
measured), go straight to the race.

Instead of driving yourself, `--input-script` can drive (`NFSMW_INPUT_SCRIPT`,
taps by seconds since launch): `sec:BUTTON` a tap, `from-to/step:BUTTON` a tap
every `step` seconds, `from~to:BUTTON` held. Buttons: `A B X Y START BACK LB RB
LS RS UP DOWN LEFT RIGHT`, the triggers `LT RT` and the left stick `LL LR LU
LD`. A script only has to work once: whatever it did is in the log, and a
car driven badly is as good a benchmark as one driven well. For example, the
first career race from a new profile, through the taps `scripts/bench.sh`
uses to reach it, then full throttle (a starting point to adapt: check what
the screen does at each tap):

```sh
scripts/timedemo.sh capture career1 --race ~/sb-bench/career1.jlg --fresh-save --seconds 90 \
    --input-script '8-26/2:START,30-130/4:DOWN,31-131/4:A,32.5-132.5/4:START,145:A,150~260:RT'
```

What it writes (with the default `--race ~/sb-bench/race.jlg`):

| Path | What |
|---|---|
| `~/sb-bench/race.jlg` | the log, EA's format (the game writes it) |
| `~/sb-bench/race.jlg.json` | its meta: the race's first frame, where the log stops being replayable, the build, settings and `NFSMW_*` it was recorded with |
| `~/sb-bench/race.jlg.save/` | the profile the race started from (`save/` and `profile_settings.bin`), copied before the game started |
| `~/sb-bench/runs/capture/<label>/` | the game's log (`game.log`), and the copy of the profile it played on (`profile/`) |

The profile comes from the game's own data folder
(`~/.local/share/speedbreaker`, a portable bundle's `data/`), or from
`--save-from <dir>`; `--fresh-save` records with a new, empty profile. The
game records on a copy (`NFSMW_SAVE_DIR`), so the snapshot stays exactly as
the race began, and your own saves are never touched. An existing race is
never replaced without `--force`.

The recording is good if the game's log (its `[timedemo]` lines are printed
at the end) has `[timedemo] capture on: <race>` near the start and
`[timedemo] capture saved: <race> (<n> bytes; race at loop ..., log to loop
..., valid until loop ...)` at the end, with a race loop (the race did load)
and a "valid until" well after it.

## 2. Replay and measure

```sh
scripts/timedemo.sh run <label> [runs] [--race <path.jlg>] [--bundle <dir>] [--warmup] [options] [VAR=value...]
```

Each run copies `race.jlg.save/` to a fresh profile folder (`NFSMW_SAVE_DIR`: a
replay can autosave, and the profile must be the recorded one), starts the
game with `NFSMW_TIMEDEMO=replay`, and waits until it exits by itself:
loading, the menus and the race all play from the log, the recorder measures
a window of the race, writes its report and quits the game. Runs go to
`~/sb-bench/runs/<label>-<n>/` (`--out` for another folder), with the next free
`n`, so repeated calls add up (`A-1 B-1 A-2 B-2 ...`). The script prints each
run's exit code, its `[bench]` line and where its `summary.json` is.

Every run gets the same fixed environment, so that nothing outside the
change differs: `NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0
NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0`. More goes on the command line as
`VAR=value` (after the fixed ones, so they win). `NFSMW_*` variables already set
in your shell apply too; the script names them once.

For good numbers:

- **`NFSMW_TIMEDEMO_UNCAPPED=1`** for throughput: frames are released
  without waiting for the vblank, so the run shows how fast frames can be
  made. Without it, the game is held to 60 fps and a run on a fast machine
  only measures the pacing (bench_compare warns about such "capped" runs).
  It applies inside the recorder's window only: before it, the replay is
  paced as the capture was, because movies and the race's intro run on real
  time, and a replay that rushes through them can fall out of step with the
  log where the intro hands over to the race.
- **`NFSMW_BENCH_FROM`** past the race's intro, so the window measures
  driving: for the race captured with the default input script, the intro
  ends about 1520 world steps after the race loads (`NFSMW_BENCH_FROM=1530`).
- **`--warmup`** once per build before measuring: the first run after a new
  build compiles shaders (the pipeline cache) and loads files cold. It also
  leaves the list of the pipelines it drew with (`pipeline_keys.bin`, beside
  the pipeline cache), and every later run makes those while the game starts,
  so a measured run creates none in the window (`pipelines.created` 0; without
  the list, the default race created 19 while driving, every run). Its report
  goes to `~/sb-bench/runs/warmup/<label>/` and isn't compared.
- **A and B in turns** (ABAB...), at least three each: a Steam Deck warms up
  and clocks down over consecutive runs, which would favour whichever side ran
  first.
- The same settings for both sides: the bundles share the game's settings
  file, unless one is portable.

### Game Mode, through Steam

Over SSH, the script shows the game on the Deck's screen: in Desktop Mode
through the desktop session's display (it borrows `DISPLAY` and the rest from
plasmashell), in Game Mode through gamescope's (`DISPLAY=:0`). Game Mode,
though, shows the window of the game Steam started, so a run started from
SSH may play behind Steam's own screen. To measure in Game Mode, start the
replay from Steam instead, with the bundle's shortcut and these launch options (one
run per launch; a fresh copy of the profile each time):

```
NFSMW_TIMEDEMO=replay NFSMW_TIMEDEMO_FILE=/home/deck/sb-bench/race.jlg NFSMW_SAVE_DIR=/home/deck/sb-bench/save-run NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_LABEL=A NFSMW_FRAME_RATE=60 NFSMW_MUTE=1 NFSMW_PERF_OVERLAY=0 NFSMW_INPUT_LOG=0 NFSMW_LOG_LATENCY=0 %command%
```

and before each launch: `rm -rf ~/sb-bench/save-run && cp -a ~/sb-bench/race.jlg.save ~/sb-bench/save-run`.
Reports then go to `~/.local/share/speedbreaker/logs/bench/` (`NFSMW_BENCH_OUT`
for a folder of your own).

## 3. Compare

```sh
python3 scripts/bench_compare.py <A runs...> -- <B runs...> [--threshold 3] [--allow <key>] [--force] [--json <path>]
```

A run is a run's folder, its `summary.json`, or a folder of run folders. A is
the baseline, B the change. Python 3's standard library is all it needs (the
Steam Deck's `python3` does).

1. **Comparability.** Every run must have ended `ok`, replayed the same race
   file (`timedemo.file_sha256`) over the same window (`timedemo.from`,
   `timedemo.steps`) through the same frames (`determinism.trace_hash`), drawn
   the same (`determinism.draws_total` within 0.1%) in the same mode and
   settings. Otherwise the runs measured different work, and the comparison
   stops with an error naming the key and which runs have which value (exit
   2). `--allow mode.vsync` (any key) accepts a difference that is the point
   of the comparison; `--force` compares anyway. Warnings, not errors:
   pipelines created or draws skipped in the window (a cold pipeline cache,
   or no pipeline key list yet: use `--warmup`), runs capped at the vblank,
   differing `NFSMW_*` environment (often exactly what A and B compare), sound
   and pad settings, systems, builds within one side, a display at another
   refresh rate.
2. **The table.** For each metric (timedemo fps, avg fps, 1% low, frame time
   p50/p95/p99/max, CP and GPU busy, waits for loading, hitches), the median
   and min..max of each side's runs and the change of the medians. The
   verdict is `noise` when the change is no larger than the wider side's
   spread (max - min), otherwise `better` or `worse`; a gated metric (`*`:
   timedemo fps, avg fps, 1% low, p99 by default, `--gate` to choose) worse by
   more than `--threshold` percent (3) and outside the noise is a
   `REGRESSION`. With one run a side there is no spread, and every difference
   counts.
3. **Frame by frame.** Every run's `frames.csv` is lined up with A's first run
   by the number of draws of each swap (the measured window starts a swap or
   three apart from run to run; the best shift within 8 swaps wins), each
   side's median frame time is taken per frame, and the 60-frame stretches
   where B is slowest are printed with their time in the race (from the
   window's start, at 60 frames a second of race time) and how much of the
   change is the CP's and the GPU's busy time. That is where to look in the
   race for what got slower.

## 4. Compare pictures

A change meant to make the game faster must not change what it draws. With
`NFSMW_BENCH_SHOTS=<step>,<step>,...` a replay also saves the picture of
those world steps of the race (counted from the race's load, as
`NFSMW_BENCH_FROM` is): `shot-<step>.ppm` in the run's folder, the frame as
the renderer hands it to the presenter, before the presenter's
post-processing (anti-aliasing, scaling, sharpening) and at the internal
resolution. Each is the same frame in every run, capped or uncapped:

```sh
SHOTS=NFSMW_BENCH_SHOTS=1600,2200,2800,3400,4000,4600,5200
# The measured runs take them (a GPU copy a shot): the strict gate.
scripts/timedemo.sh run A 1 --bundle ~/sb/A NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530 $SHOTS
scripts/timedemo.sh run B 1 --bundle ~/sb/B NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530 $SHOTS
python3 scripts/compare_shots.py ~/sb-bench/runs/A-1 ~/sb-bench/runs/B-1 --strict --diff /tmp/shots-diff
# Capped runs (as the warm-ups, which measure nothing) compare the same way.
scripts/timedemo.sh run WA 1 --bundle ~/sb/A NFSMW_BENCH_FROM=1530 $SHOTS
scripts/timedemo.sh run WB 1 --bundle ~/sb/B NFSMW_BENCH_FROM=1530 $SHOTS
python3 scripts/compare_shots.py ~/sb-bench/runs/WA-1 ~/sb-bench/runs/WB-1 --strict
```

Runs at different internal resolutions (0.75x with Upscaling FSR 1 against
1x) have front images of different sizes, and neither is what the player
sees. With `NFSMW_BENCH_SHOTS_SHOWN=1` too, each shot's frame is also saved
as the window shows it, after the presenter's post-processing and at the
window's size, without the menus' UI or letterbox bars:
`shot-<step>-shown.ppm`, compared with `compare_shots.py --shown`. The
presenter copies the frame as it draws it, on a thread of its own: a frame
it replaces with a newer one before drawing it (uncapped runs drop many) has
no such picture (`shown_status` failed, with `shown_why`).

`compare_shots.py` prints, for each step, whether the two pictures are
identical, and if not how many pixels changed, by how much at most (0-255),
the PSNR, how many are off by more than `--threshold`, and where those are
(the rectangle they lie in, and the tiles of an 8x8 grid that hold most of
them); `--diff <dir>` writes a picture of where. A shot passes when no more
than `--tolerance` percent of its pixels are off by more than `--threshold`
and its PSNR reaches `--min-psnr`. Exit 0 when every shot passes, 1 when
one doesn't, 2 when the runs can't be compared.

A replay draws its pictures the same at any pace: the two things a picture
took from the timing, the exposure's brightness and the clouds' drift, wait
and hold still in a replay that takes pictures ([Exposure and
clouds](#exposure-and-clouds)).
On the Steam Deck (the default race, seven shots):

- Uncapped against uncapped, the usual build against its ThinLTO and PGO
  build: two of the seven pictures identical, the rest off by 1 level at
  most (0.01% to 0.04% of the pixels), PSNR 86.9 dB or more.
- Capped against uncapped, one build: one picture identical, five off by 1
  or 2 levels at most, one with a spot off by up to 11 (0.31% of the pixels
  changed), PSNR 57.1 dB or more.
- Before the wait and the hold, uncapped runs of one build differed over
  much of the picture (0.01% to 43% of the pixels, by 1 to 20 levels mostly,
  at worst 49, PSNR down to 37.4 dB; 61 and 33.6 dB against a capped run),
  worst in the sky and bright areas.

So `--strict` (`--threshold 8 --tolerance 0.01 --min-psnr 50`) works for
uncapped runs too, the measured ones included, and catches subtle changes:
use it. The default gate (`--threshold 64 --tolerance 0.05 --min-psnr 30`)
fails only what a broken draw does, a missing, misplaced or wrongly textured
object (hundreds of pixels far off) or the whole picture off by 8 levels or
more; a picture two frames later fails it by far (2% to 12% of the pixels
off by more than 64, PSNR 14 to 24 dB): for runs made without the wait and
the hold. `--exact` asks for identical pictures. Either way only runs that
skipped no draws in the window compare (`pipelines.skips` 0: a warm
pipeline cache), as for their times.

Which frame a step's picture is. The game's main loop runs ahead of its
pictures: its render thread draws and swaps a frame while the loop runs the
next ones (up to four ahead, uncapped), and the GPU side executes that swap
later still, by a lag that changes from frame to frame, so the swap the GPU
side is at when the loop reaches a step is a different frame in every run.
What is the same in every run is that in a race the game swaps once a
frame, in order: the race frame that began at main loop iteration L
(`RealLoopCounter`) is the game's swap L + 1 + an offset (0 in the race
recorded on the Deck; its menus ran 2), and two runs of a replay make the
same swaps. The recorder measures the offset in the race (the most swaps the
game had made at the start of a race frame, less the frame's loop count: at
the vblank's pace, before the window, the render thread has caught up at
nearly every frame's start), keeps it from the first shot on, and names
each shot's swap from its frame's loop count; the command processor copies
that swap's front image when it executes it. `shots.verified` says the
offset held: no race frame after the first shot started with more swaps
than it allows (a swap that isn't a frame's). An unverified shot may picture
another frame. The offset needs race frames at the vblank's pace before the
first shot: keep `NFSMW_BENCH_FROM` well past the race's load (the default
race's 1530 gives 1500 of them).

What it costs, for each shot: at its swap the command processor makes a
buffer of the picture's size (3.5 MB at 1280x720) and records a copy into it
(well under a millisecond), the GPU copies, and a thread of its own writes
the file once the copy has landed. The game waits for none of it, so shots
can be taken in measured runs: on the Deck five uncapped runs with shots
made 118.4 to 119.1 fps, three without 118.7 to 118.8. The trace hash and the
draws are those of a run without shots.

## 5. Faster builds

The usual build (README.md) compiles each source file on its own and lays
out each function by the compiler's guesses of which way its branches go.
Three options of the top-level `CMakeLists.txt` do better, on Linux (SteamOS
bundles included) and Android; ThinLTO also on Windows (the clang cross-build,
`SB_CMAKE_ARGS=-DSB_LTO=ON scripts/windows/build.sh`). All are off by default,
and without them the build is the usual one (`scripts/steamos/release_hle.sh`
makes its Linux and Windows downloads with ThinLTO):

| CMake option | What |
|---|---|
| `-DSB_PGO_PROFILE=<file.profdata>` | profile-guided optimisation (PGO): the game's code (the recompiled code and the runtime) laid out and inlined by the counts of a training run |
| `-DSB_PGO_GENERATE=ON` | the instrumented build that makes those counts, for training only: it counts how often each branch, call and loop runs, and writes the counts when the game exits |
| `-DSB_LTO=ON` | ThinLTO, linked with `lld`: functions inline across source files (the recompiled code's indirect-call lookup into every guest function that calls one) |

They change how the code is laid out and inlined, never what it computes:
their replays have the trace hash of the usual build. The training is a
timedemo replay of the instrumented build, and `scripts/pgo_train.sh` does
each step (its header has the details):

```sh
scripts/pgo_train.sh build                       # build/pgo-gen: the instrumented game
scripts/pgo_train.sh run pgo --build build/pgo-gen NFSMW_TIMEDEMO_UNCAPPED=1 NFSMW_BENCH_FROM=1530
scripts/pgo_train.sh merge build/speedbreaker.profdata ~/sb-bench/pgo/raw
cmake -S . -B build/pgo -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++ -DSB_LTO=ON -DSB_PGO_PROFILE=$PWD/build/speedbreaker.profdata
ninja -C build/pgo SpeedBreaker
```

For a Steam Deck: build in the container, bundle `build/pgo-gen` with
`SB_ALLOW_PATHS=1 scripts/steamos/make_bundle.sh build/pgo-gen <out>` (the
instrumented game carries its functions' names), run `pgo_train.sh run
<label> --bundle <dir> ...` on the Deck from the folder that has
`timedemo.sh`, and bring the `.profraw` back to merge it. The instrumented
game plays at about 92% of the usual speed (114 fps uncapped on the default
race), so its replay stays in step; a run writes a 7 MB `.profraw`, and two
trainings of the same race agree to 99.9%. The build says what it is
(`Release, ThinLTO, PGO` in the log's `[build]` line, bug reports and
`summary.json`'s `build`).

What they gave on the Steam Deck (the default race, uncapped; the usual
build against the other in turns, medians of 3 or 4 runs a side, each
comparison with its own runs of the usual build; thread CPU is the time a
thread ran in the window, read from `/proc`, per frame):

| | usual build | PGO | PGO + ThinLTO |
|---|---|---|---|
| avg fps | 124.2 | 125.1 (+0.7%) | 125.9 (+1.4%) |
| frame p95 / p99 ms | 11.65 / 12.59 | 11.47 / 12.33 | 11.33 / 12.17 |
| CP busy ms a frame | 7.17 | 7.00 (-2.4%) | 6.95 (-3.1%) |
| GPU busy ms a frame | 7.82 | 7.79 | 7.74 (-1.0%) |
| CPU of all threads, ms a frame | 23.95 | 23.58 (-1.6%) | 23.25 (-3.2%) |
| guest worker g2, main thread | 6.33, 6.05 | 6.17, 6.04 | 6.09, 5.92 |

The pictures pass the strict check against the usual build's (capped runs:
at most 2 levels apart). ThinLTO alone gave +0.4% fps and 1% less CPU on the
guest's threads. On Windows (a PC with an i9-14900K and an RTX 5080, where the
command processor bounds the race: 1800 steps from 1530, uncapped, 3
alternated pairs) ThinLTO gave 224.0 -> 226.7 fps (+1.2%, every pair), the
command processor 4.27 -> 4.23 ms a frame, the same trace hash. On Android
(Turnip on an Adreno 840, GPU-bound) it changed nothing measurable.

Notes:

- `gpu/renderer.cpp` takes no profile (`-fno-profile-instr-use`). With it,
  Draw inlined what it calls (50 KB of code grew to 159 KB) and its hottest
  loop, the scan of a draw's indices for 0xFFFF, came out at 6 instructions
  an index instead of 4: the command processor took 4% longer (CP busy 7.46
  against 7.17 ms), although its i-cache misses halved and its branch misses
  fell 13%. The rest of the command processor's code
  (`command_processor.cpp`) gains with the profile. Worth trying again once
  that scan is gone.
- The race is all the training sees. Code it never ran (police chases, when
  the race has none) is compiled as rarely used: smaller, and slower if
  anything when it does run. More races, merged into one profile, cover
  more.
- A profile from older code still works: the compiler leaves out the
  functions that changed since (a warning a file), so train again after
  large changes.
- Until the stream hold ([Audio streams](#audio-streams)), the default
  race's replay sometimes went out of step where its intro hands over to the
  race (`DIVERGENCE kind=checksum loop=4416 step=1520`, or `loop=4415`), most
  often with ThinLTO: in 8 of 22 runs of ThinLTO builds (7 of 17 with PGO),
  1 of 27 of the PGO builds without it, none of 37 of the usual build. That
  was the replay's timing, not what the code computes: a faster build moved
  the frame in which the race's intro stream had its data.

## Audio streams

The Joylog holds what the game loads and when, but not its audio streams
(speech, music, a race's intro track): the stream player reads them on
threads of its own, as fast as the files come, and the game's code asks how
much of a stream is in (`sub_821C8AD8`). A race's start waits on that: the
default race starts its intro stream at loop 4412, waits for its first data,
then starts the race, and its cars move two frames later. The frame in which
the read had landed decided the frame the race started: in its capture
4414, in a replay a frame either side now and then (the read landing during
the 120 ms frame after the intro movie, or not), which the checksum caught
at loop 4415 or 4416, the cars already moving or not yet.

So in a timedemo, capture and replay alike, the game's main thread sees a
stream it started without data for exactly two frames, then with it: data
that came sooner waits, and data still on its way is waited for (3 s at
most). The stream player's own threads see their streams as they are. Each
stream leaves a line, e.g. `[timedemo] stream 2: started at loop 4412, its
data at loop 4414 (2569 ms in; waited 0.0 ms)`; a wait above a few ms
there is a slow read, and counts in that frame's time like any wait. Only a
timedemo holds streams.

## Exposure and clouds

The game's exposure follows the scene's brightness: once a frame its render
thread draws the frame down to 64x64, resolves that into one of two
textures, and averages the other one, the previous frame's, on the CPU. It
locks that texture read-only, which skips D3D's wait for the GPU: on the
console the GPU is done with the previous frame by then. Here the GPU side
can be frames behind (uncapped, the game runs up to four ahead), and the
average was of the previous frame's picture or of an older one, as the lag
had it: every picture after came out a little brighter or darker, by up to
20 to 49 levels between uncapped runs of one build (PSNR down to 37 dB), at
the vblank's pace a level or two.

So in a replay that takes pictures (`NFSMW_BENCH_SHOTS`) that one lock
waits for its texture's fence, as a lock that isn't read-only does, and
every run averages the previous frame (`[timedemo] exposure: ...` in the
log). The render thread then waits, each frame, for the GPU to finish the
previous frame's brightness pass, where it used to run on (one of D3D's
waits on the GPU, as the `[hitch]` lines and `NFSMW_TIMELINE` count them).
That costs an uncapped replay on the Steam Deck about 1.5% of its frame
rate (121.4 against 123.2 fps, p99 13.5 against 12.6 ms, one build), and a
capped one a few late frames (0 to 6 in a run, up to 29 ms; none without
it), so a replay that takes no pictures doesn't wait: its times are those of
play. `NFSMW_TIMEDEMO_EXPOSURE_WAIT=1` or `=0` makes any
replay wait or not. Times from replays with the wait don't compare with
times from replays without it: give both sides of an A/B the same shots,
or the same switch.

The clouds were the other thing a picture took from the timing. The sky's
cloud scroll (its vertex shader constant c13.x) moves on whenever the sky's
setup, as it draws, finds the main thread's tick counter changed, by the
wind times the main thread's frame time: both read live from the drawing
side, so how far the clouds had drifted by a shot was the threads' timing
(between uncapped runs of builds of different speeds, up to 24 levels over a
fifth of a picture). In a replay the clouds hold still where the replay
started them (`[timedemo] clouds: ...`; `NFSMW_TIMEDEMO_CLOUD_HOLD=0` lets
them drift). With both, the shader constants of every draw were the same in
three uncapped headless replays.

## Switches

All off by default: without them the game behaves exactly as without this
feature. A timedemo also holds audio streams ([Audio streams](#audio-streams)),
capture and replay alike, a replay that takes pictures waits for the
exposure's brightness, and every replay holds the clouds still
([Exposure](#exposure-and-clouds)); `NFSMW_TIMEDEMO_EXPOSURE_WAIT=1|0` and
`NFSMW_TIMEDEMO_CLOUD_HOLD=0` change those.

| Variable | Meaning |
|---|---|
| `NFSMW_TIMEDEMO=capture\|replay` | record, or replay as a benchmark |
| `NFSMW_TIMEDEMO_FILE=<path.jlg>` | the log (default `<data folder>/timedemo/race.jlg`); beside it its meta, `<path.jlg>.json`, and the profile snapshot, `<path.jlg>.save/` |
| `NFSMW_TIMEDEMO_FIXED=1\|0` | capture: give every frame exactly 1/60 s (default 1) |
| `NFSMW_TIMEDEMO_CAPTURE_SECONDS=<s>` | capture: stop and exit this long after the race has loaded (0: at the quit) |
| `NFSMW_TIMEDEMO_UNCAPPED=1` | replay: release frames without waiting for the vblank |
| `NFSMW_BENCH_FROM=<steps>` | the window starts this many world steps (1/60 s each) after the race has loaded (0) |
| `NFSMW_BENCH_STEPS=<n>` | the window's length in world steps (default: to 60 steps before the log's replayable end) |
| `NFSMW_BENCH_LABEL=<text>` | the run's name in its report |
| `NFSMW_BENCH_OUT=<dir>` | where the report goes (default `<data folder>/logs/bench/<date>_<time>_<label>/`) |
| `NFSMW_BENCH_TIMEOUT=<s>` | a partial report and exit after this long (900) |
| `NFSMW_BENCH_EXIT=0` | stay in the game after the report (the default is to exit) |
| `NFSMW_BENCH_SHOTS=<step>,...` | the picture of these world steps of the race (counted as `NFSMW_BENCH_FROM` is), as `shot-<step>.ppm` in the report's folder (see [Pictures](#4-compare-pictures)) |
| `NFSMW_BENCH_SHOTS_SHOWN=1` | each of those also as the window shows it, after the presenter's post-processing: `shot-<step>-shown.ppm` |
| `NFSMW_TIMEDEMO_EXPOSURE_WAIT=1\|0` | replay: the exposure's brightness waits for its frame on the GPU (1), or is as the GPU side has it when the game reads it (0); default: the wait in a replay that takes pictures (`NFSMW_BENCH_SHOTS`) only (see [Exposure](#exposure-and-clouds)) |
| `NFSMW_TIMEDEMO_CLOUD_HOLD=0` | replay: the clouds drift as the threads' timing has it, as before the hold (see [Exposure](#exposure-and-clouds)) |
| `NFSMW_SAVE_DIR=<dir>` | an absolute path: `save/` and `profile_settings.bin` there instead of the data folder's |

## The report

In the run's folder (`NFSMW_BENCH_OUT`):

- `summary.json`: `status`, `label`, `version`, `build_id`; `timedemo_fps`
  (the classic figure: the window's game frames over their time);
  `timedemo{file_sha256, race_loop, from, steps}`; `window{frames, swaps,
  seconds, ...}`; `fps{avg, low_1pct, low_01pct, p99_fps}`; `frame_ms` and
  `game_frame_ms{p50, p95, p99, p999, max, mean, stdev, mean_abs_delta}`;
  `wait_ms{total, max}` (the game thread waiting for file loads, apart from
  render time); `cpu{cp_busy_mean, cp_waits{...}}`; `gpu{busy_mean,
  exec_mean}` (exec with `NFSMW_GPU_TIMING=1`); `hitches{late,
  over_2x_median, over_50ms, worst[]}`; `pipelines{created, skips}`;
  `memory`; `determinism{divergence, trace_hash, draws_total}`; `mode`,
  `settings`, `env`, `system`.
- `frames.csv`: one row per swap in the window: `swap, t_us, frame_us,
  draws, cp_busy_us, cp_wait_throttle_us, cp_wait_other_us, gpu_busy_us,
  late`.
- `main.csv`: one row per game frame: `loop, step, t_us, frame_us, wait_us,
  flow`.
- With `NFSMW_BENCH_SHOTS`: `shot-<step>.ppm` for each step taken, and
  `shots` in `summary.json`: `verified`, `swap_offset` (the game's swaps
  against its frames), `race_frames` (the race frames noted),
  `frames_at_offset` (those that showed it before the first shot),
  `frames_past_offset`, and
  `taken[]`, each step's `status` (`ok`, `unverified`, `failed` with `why`,
  `pending`, `not_reached`), the frame it pictures (`loop`, `frame_step`),
  its `swap`, `file`, `width` and `height`; with `NFSMW_BENCH_SHOTS_SHOWN`
  also `shown_status` (`ok`, `failed` with `shown_why`, `pending`),
  `shown_file`, `shown_width` and `shown_height`.
- A `[bench]` line in the game's log: `[bench] A-1 ok: 7200 frames 98.4 s |
  timedemo 73.2 fps, 1% low 51.0 | p50 13.1 p99 21.4 max 48.2 ms | ...`.

## Exit codes

| | Code | Meaning |
|---|---|---|
| the game, replaying | 0 | ok: the window measured, the report written |
| | 2 | setup failed (the log didn't open, or is from another `default.xex`), the log ended before the window did, or the bench timed out |
| | 3 | diverged: the game's checksum found the replay out of step (the log has `[timedemo] DIVERGENCE kind=... loop=... step=...`) |
| | 4 | interrupted: the runtime's own menu or a notice came up during the replay |
| `timedemo.sh` | 0 | every run ended ok with a report |
| | 1 | a run didn't (each run's code is printed; 124: stopped at `--timeout`) |
| | 2 | bad arguments, or a missing race or profile snapshot |
| `bench_compare.py` | 0 | comparable, and no regression beyond the threshold outside the noise |
| | 1 | a regression |
| | 2 | the runs aren't comparable, or bad input |
| `compare_shots.py` | 0 | every picture within the tolerance (by default the run-to-run noise; `--exact`: identical) |
| | 1 | a picture beyond it |
| | 2 | the pictures can't be compared: one missing from a run, another size, another swap, not a binary PPM |

## Limits

- **One `default.xex`.** The guest addresses the feature uses are those of
  the supported USA release only.
- **The same `ppc/` build.** A replay stays in step while the recompiled
  code computes exactly the same floating point: the same recompiler output,
  the same compiler and flags. A rebuilt `ppc/` (another compiler, other
  flags) or a runtime change the game can see (memory it is given, file
  sizes, timing it is told) can make an old log diverge; the run then ends
  with code 3 at the first frame that differs, and the race has to be
  recorded again on the new build. A change to the renderer alone leaves the
  game's own computation as it was, which is what A/B runs of one race are
  for.
- **The same settings.** What the game itself sees must match the recording:
  the aspect ratio (ultrawide), Frame Rate 60, the language. `race.jlg.json`
  lists what it was recorded with; the checksum has the last word.
  Rendering settings may differ from the recording, but must match between A
  and B (bench_compare checks).
- **The same profile.** Every replay must start from the recorded profile:
  the snapshot in `race.jlg.save/`, a fresh copy per run (the script does this).
  The gearbox, the camera and the units come from the profile, and a replay
  may autosave.
- **Only what is in the log.** The runtime's own menu or notices during a
  replay aren't in it: the run ends as interrupted (code 4). During a
  recording, the log stays replayable only up to the first such notice (the
  meta's end). Don't open the runtime's menu while recording or replaying.
- **Uncapped runs** play the race faster than real time while sound,
  movies and loading screens keep real time; file loads then can't keep up,
  and the game waits for them inside its frames. That time is `wait_ms`, kept
  apart from render time: if it grows, the run is limited by streaming, not
  by rendering.
- **Audio streams** are held for two frames (see [Audio streams](#audio-streams)):
  a capture made before the hold replays as long as each stream its game
  logic waits on had its data two frames after it started (the default race
  did).
- **A crash while recording** loses up to the last 10 seconds of the log.
- Timedemo runs keep a 1 MB log buffer in the game's memory: compare them
  with each other, not with normal play.
