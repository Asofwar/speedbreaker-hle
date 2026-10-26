# Online multiplayer: plan and status

Goal: live online play in a shared world, built on the game's own two-player split-screen mode (the user reports the game has one).
Design: **lockstep**. Each player runs the whole game on their own machine in two-player mode; player 1 is the local controller, player 2's
"controller" is fed from the network. Every simulation step only the inputs cross the network. The game's world advances in fixed 1/60 s
steps from an accumulator (`game/frame_time.cpp`), which suits lockstep, provided the simulation is deterministic.

## Phases
1. **Second controller** (done in part, below).
2. **Determinism check**: record both players' inputs, replay twice, compare the cars' positions. If traffic, AI or physics depend on frame
   timing or unseeded randomness, find and pin those sources. Make or break.
3. **Netcode**: UDP, a few steps of input delay, a handshake that agrees the random seed and race setup, desync detection.
4. **Internet play**: NAT traversal or a relay, finding opponents.
Risks: determinism (may need work in many places), both views shown unless the renderer is changed to draw only the local one, input delay.

## Phase 1 status
Done (`runtime/hid/input.cpp`): XInput controller 1 is a second SDL gamepad (the first one player 1 is not using; a lone pad stays player 1's),
or `NFSMW_INPUT_SCRIPT2` (the `NFSMW_INPUT_SCRIPT` syntax) for tests without one. Sticks, triggers and rumble go through the same deadzones and
Vibration setting; no keyboard, no Back/Start chord for it. `NFSMW_LOG_PLAYER2=1` logs the first time the game asks about controller 1.
**Checked:** the game does ask about controller 1 at startup (before it only ever heard "not connected"); with player 2 present it was told
"connected" and the game reached the same point as without (frame 600 at 22 s, no new problems in the log).
**Local two-player race reached (driven with `NFSMW_INPUT_SCRIPT2` and the new `NFSMW_INPUT_CMD` file, one step at a time):**
main menu > Quick Race > Split Screen > Circuit > track > options > player 1's car and transmission > **"Select player 2: Player 2 press START"**
(the game identifies player 2 by the controller that presses START; it needs XInput controller 1 to answer, which is what Phase 1 added) >
player 2's car and transmission > the race starts with both HUDs (positions 1 and 2, lap 1/2, two speedometers). Seen: player 1's
throttle moved car 1 (top speedometer 24 mph, the other at 0); the top/bottom split-screen view was drawn.
**Player 2 controls a car (checked):** with only player 2's throttle pressed (`2 RT`), the speedometer in view went from 32 to 62 mph while
the other car's stayed at 0.
**Cost of the local mode (corrected):** the 120-170 ms frames seen once were a transient. Settled over about 30 s: 16-17 fps, GPU 60 ms, about 4500
draws and 30 passes a frame, against a single-player race at 26 fps, GPU 38 ms, about 2400 draws and 18 passes. So two views cost about 1.6x
the GPU time (about 2x the draws): what two scene renders should, not an anomaly. For online play, drawing only the local view would bring
it back to about single-player cost, but the game draws both views, so the viewport setup would need changing; not looked at yet.
**Not verified:** a real second pad (only the script and command file were used).

## Phase 2 (determinism): what has to hold, from what is known
- The game polls the controller once per frame, but its world advances in 1/60 s steps from a real-time accumulator, so the steps per frame
  depend on how long the frame took. Two machines of different speeds would run different numbers of steps between the same two inputs.
  Lockstep needs the steps per frame fixed (for example always the same number per presented frame, from the vblank count; `game/frame_time.cpp`
  and `game/whole_vblanks.h` already quantise frame time, which is the place to look).
- Inputs are then exchanged per frame, and each frame applies both players' inputs for that frame's steps.
- Random numbers, AI and traffic must start from the same seed on both machines (the race setup is chosen by the host).
- To measure it: apply scripted inputs by frame number (not time), run twice, hash the cars' state each frame, and compare. The cars' memory
  addresses move between launches (heap), so the hash needs the cars found by walking the game's own structures, not by a fixed address.

## Phase 2 status
Built (all off by default; nothing changes without them):
- `NFSMW_FIXED_STEPS=<1-4>`: every game frame is exactly that many world steps, whatever it took (`game/frame_time.cpp`). It also makes the
  game's `mftb` (the timebase it reads, `cpu/guest_time.h`) a function of the game frame (n/60 s a frame, plus a few ticks per read in a frame,
  so a wait for time to pass still ends); `NFSMW_REAL_TIMEBASE=1` keeps the host's. Changing that header rebuilds all recompiled sources.
- `NFSMW_INPUT_FRAMES` / `NFSMW_INPUT_FRAMES2`: scripted inputs keyed to world frames, with a `SHOT` step (`hid/input.cpp`).
- `NFSMW_INPUT_CMD=<file>`: step-by-step commands for either player; `NFSMW_FRAME_HASH=1|detail`, `NFSMW_FRAME_HASH_FILE=<file>`: a hash of the
  shader constants of every draw, per frame and per draw (`gpu/renderer.cpp`), to compare two runs.
**Where the first attempts went wrong:** frame-keyed inputs counted frames from launch (the front end also runs the world stepper), so they
fired in menus. The one state value found (`0x82A38D70`, 0 in the front end, 2 from the start of loading) marks the start of loading, not the first
race frame, and loading takes a varying number of frames. No exact race-start anchor yet.
**Where the comparison stands (two identical two-player races, fixed steps, deterministic timebase, async shaders off, player 1's throttle held):**
- Whole-frame hashes never match between runs (0 of about 800 frames), and many constants change every frame even while the game is paused.
- Per-draw hashes align: from about race frame 170 on, frame i of one run lines up with frame i+offset of the other for 200+ frames (offsets 119 and
  -41 in the two pairs compared), and about 13% of a frame's draws match exactly, steady through the countdown and after.
- Per-slot digests (`NFSMW_FRAME_GROUPS_FILE`, 3200 (stage, slot, shader) groups compared per pair of aligned frames): 61% of the groups differ.
  The vertex shader slots c0-c3 (the transform) differ in 120 of 123 shaders, and no shift between -120 and +120 frames makes them agree
  (best 2.8%). So the camera and the transformed objects are not the same in the two runs at any alignment.
- **Cause not known.** The harness itself is a suspect: the throttle was pressed by wall-clock command during loading, so it was held for a
  different number of frames before the start in each run (the engine may ramp with it), and the alignment of static draws cannot pin a frame
  exactly. Real divergence of the world (random seeds, thread timing, streaming) is equally possible.
**What this means for lockstep:** not yet shown to work, and not shown to be impossible. It needs an exact race-start anchor in the game's own state
(the first frame of the race world), so that inputs can be keyed to the race's clock and not the wall's; with that, repeat the comparison. If the
runs still differ with identical inputs on the same frames, the game has sources of nondeterminism beyond the clock that would each have to be found.
**Next:** find the race's own clock or start flag in the game's state (the heap structures reached from a global pointer), key the inputs to it, and
repeat the two-run comparison. If lockstep proves out of reach, other designs: ghost racing (record and replay a run, no live sync) or a
host-authoritative design where one machine runs the race and the other only draws.

## Design (with a server): native, reusing the game's own two-player mode
Rule for this work: no poking at cars in memory. Every car is a real game vehicle created and simulated by the game's own code, and every change goes
through a path the game already has (its input, its split-screen setup, its reset and teleport routines).
The game already has everything a second car needs: the split-screen mode makes player 2 a full native car whose controller input comes from XInput
controller 1, which `hid/input.cpp` already supplies (Phase 1). So an online race is the game's own two-player race in which controller 1's input
comes from the network:
1. **Input relay (native input path).** Each machine sends its local pad state once per game frame to the server, and the server forwards it. On
   each machine the remote player's input is what the game reads for its second controller. The local pad maps to controller 0 on one machine and
   controller 1 on the other, so both machines have the same grid (the "Player 2 press START" screen picks whichever pad presses Start).
2. **Both cars simulated natively on both machines**, with a few frames of input delay so inputs arrive before they are used.
3. **Resync with the game's own routines.** The two simulations will drift (see Phase 2: not shown deterministic). The server periodically sends one
   machine's car states (position, orientation, velocity) and the other applies them through the same code the game uses to put a car back on the
   road (the respawn/reset routine), at a moment when that is not visible, not by writing the rigid body.
4. **One view.** The game draws both viewports; each machine should draw only its own (skip the other viewport's draws in the renderer: about 1.6x
   GPU time saved), without changing what the game simulates.
5. Server: any small UDP relay (rooms, input forwarding, state sync).
Cost: the second car's physics on every machine; benefit: nothing in the game's rules has to be re-implemented.

### Findings so far on the game's own structures
- The data segment names the classes: `PVehicle`, `AIVehicle`, `AIVehicleHuman`, `AIVehicleRacecar`, `AIVehicleTraffic`, `AIVehicleCopCar`, `RigidBody`,
  `SimpleRigidBody`. In a race the globals hold about 3000 aligned heap pointers; 471 point at objects with a vtable-like first word (373 share
  0x8208A794). The heap moves between launches: objects have to be found by walking pointers.
- Two snapshots of about 52,000 objects reachable from the globals (two levels), taken at the start of a circuit with one opponent and about 10 s
  later, showed world positions of the form (x, y, z) = (377, 4525, 234) in 4x4 matrices with the translation in the fourth row, in arrays with a
  0x40-byte stride (one object holds a history of such poses: spacing about 0x40, positions moving along the road). These are poses of a car or
  the camera; which object is the player's vehicle, and which fields are position, orientation and velocity of its rigid body, is not yet known.
- Next: identify the vehicle object by a second signal (the speedometer value and the race clock), then find the game's own reset/teleport function
  for step 3 (the code behind the "Reset car" button and the respawn after a wreck), and confirm the controller-1 input moves the second car.

## The game already has an online stack: the native path (found 2026-10-16)
Strings in the executable show the game's own networking: `ConnectionCore::ResetSession_HaveMutex` ("removing all players from my session"), the
Xbox Live menu item, and EA web calls (`XeRecoverAccount.ashx`). Its imports are the plain Xbox networking set: `NetDll_XNet*` (startup, XNADDR, key
registration, DNS, QoS) and Winsock (`socket`, `bind`, `sendto`, `recvfrom`, `select`, `WSAWaitForMultipleEvents`, ...), 89 of them stubbed here; it uses
no `XSession*` functions. So the native route is to supply what the game's own online code asks of the runtime, with a server as the network, instead
of adding a second system beside it.
**Probe (`NFSMW_FAKE_LIVE=1`, off by default):** `XamUserCheckPrivilege` grants every privilege (the game asks for 254, multiplayer sessions),
`XamUserGetSigninState` says signed in to Live (2), and when the game asks for privilege 254 (the player opened Xbox Live) the listeners are sent
`XN_SYS_SIGNINCHANGED`, `XN_LIVE_CONNECTIONCHANGED` (0x001510F0) and `XN_LIVE_LINK_STATE_CHANGED`. (Sent at boot instead, the game crashed on a null object:
the notifications must come when it asks.) With `NFSMW_STUB_CONTINUE=1 NFSMW_TRACE=1` the game's own flow then ran:
1. `NetDll_XNetStartup`, `NetDll_WSAStartup` (twice, versions 2.2 and 0.2) from the online module (0x8281xxxx / 0x8282xxxx);
2. `NetDll_XNetGetTitleXnAddr` polled (56 times): it is waiting for the console to have a network address;
3. a networking thread loops on `NetDll_WSAWaitForMultipleEvents` / `NetDll_WSAResetEvent` (40,000 calls: the stubs return at once, so it spins);
4. `XamAlloc` (unimplemented) returns 0, and the game dereferences it: SIGSEGV at guest address 8, `lr=0x828100E4`, about 146 s in.
So the next steps are concrete and visible in the log:
- `XamAlloc`/`XamFree`; `XNetStartup`/`Cleanup`; `WSAStartup`/`Cleanup`; `XNetGetTitleXnAddr` returning a static address with the right flags;
- the Winsock event calls (`WSACreateEvent`, `WSAEventSelect`, `WSAWaitForMultipleEvents`, `WSAResetEvent`, `WSASetEvent`) on host primitives;
- sockets on host UDP/TCP (`socket`, `bind`, `sendto`, `recvfrom`, `select`, `ioctlsocket`, `getsockname`, ...), with the Xbox's secure addresses
  (`XNetXnAddrToInAddr`, `XNetServerToInAddr`, key registration) mapped to ordinary host addresses;
- then whatever the game's lobby needs to find a session: it was a hosted matchmaking, so the server has to provide that, and it is not known yet
  which web calls the game makes (EA's services are gone). That is the unknown to measure next.
The probe, as it stands, only gets the game to the start of its network bring-up.

## Decision: our own small netplay layer instead of the game's online stack
The game's own online code (`ConnectionCore`, Xbox Live, EA's services) was brought up as far as its first network calls (probe above), but it
depends on servers that no longer exist and a lobby layer of session objects I would have to emulate completely. So it is replaced, not repaired:
- `runtime/net/netplay.{h,cpp}`: a UDP client (about 150 lines) that sends the local pad about 30 times a second and receives the other player's.
  `NFSMW_NETPLAY=<host>:<port>/<room>[/<slot>]` turns it on; the slot (0 or 1) says whether this machine's pad is player 1 or 2 of the game's own
  two-player race, so both machines agree.
- `hid/input.cpp`: the remote pad is delivered as XInput controller 1 (or 0, by slot), the same path as a real second pad. The cars are the game's own;
  nothing in the game's memory is written.
- `scripts/netplay/server.py`: the relay (rooms of two, forwards INPUT as RELAY, 15 s timeout). `scripts/netplay/bot.py`: a scripted second player for tests.
**Checked (this machine, localhost, throw-away save):** the game joined a room, a bot joined as the second player; the bot's START was accepted
at "Player 2 press START" (no local controller involved), the bot chose a car, and in the race the bot's throttle moved the bottom car (67 mph) while
the local throttle moved the top car; about 290 inputs arrived in each 10-second window from the bot (30 a second) with no loss counted.
**Not done:** the two machines' simulations will drift (determinism was not shown); input delay and a periodic resync through the game's own reset
routine; hiding the other viewport; a second real machine and the internet (the server must be reachable: a public host or a port-forward);
authentication and cheating (inputs are trusted); more than two players (the game's own mode has two).

## First test between two real machines (2026-10-17, over a ZeroTier link, 42 ms)
Both machines joined the relay (which ran on the x86 machine) and each saw "2 players"; inputs arrived at about 520 a second from the x86 machine (its client
sent every loop iteration: now limited to 60 a second, and at once on a change). The x86 machine's START reached "Player 2 press START" here and the game
went on to player 2's car select. Then my game entered the race while the other machine's game sat on its main menu: the menus had diverged, because button
state shared during menus is lost or counted differently when the two games are at different points (loading, a transition). So the pads are now shared
only after an explicit link (F8 on both machines at "Select player 2"); before it each machine's menus run on its own pad. A bug found on the way: a
command-file line that made no tap (the link toggle) was never cleared and toggled every frame; fixed.
Checked locally with the bot: unlinked, the bot's START is ignored at the title screen and at "Select player 2"; after the link it reaches player 2.
**Still not measured:** whether the two simulations stay together in a race (no race ran on both machines yet).

## Second two-machine test (2026-10-17, same link, pads linked by hand at "Select player 2")
**Worked:** with each machine on its own menus and the pads linked (F8) at "Select player 2", the other machine's START went through, player 2 chose a car and a
transmission on that machine, and both games entered the same race (same Ironwood Estates circuit, 2 laps, minimum traffic) and showed **the same race clock**
(reported by the other player). I held player 1's throttle for 25 s on my side (car reached 84 mph, scraping a barrier at race time 1:12). About 4 minutes
later (clock 4:58 on mine) the other player reported the **same scene** as mine on their screen (a car stopped on grass beside a wooden fence and a tree),
with both cars stationary, after car movements that came from both machines.
**What this does and does not show:** both simulations were still showing the same stationary scene four minutes in. It is one observation, reported from
the other screen in words, with no screenshot or number from it. It does not show that the cars follow the same path during the movement, or after a
long race, a collision or a wrecked car; it is encouraging, not a measurement.
**Unexplained:** while the other player held their throttle (for an unknown time), the car that accelerated on my screen (27, 56, 61, 85 mph) was in the **top**
view (player 1), where I expected the bottom one (player 2). Possible causes: which viewport is which depends on the cars' relative positions, or the other
machine's input arrives as player 1's. Not tested. To settle it, log the remote pad's state changes on arrival (not built), and have the other machine press
throttle at a stated moment.
**Next:** log the remote pad on my side; hold throttle on the second machine at a stated moment and see which car moves; then compare the two screens during a
long race and after a crash; then the resync design if they drift.

## Third two-machine test (2026-10-17): logging confirms the routing; clocks not comparable by eye
With `NFSMW_LOG_NET_PADS=1` the log shows each remote change and what the game is given, to the millisecond: the other machine's START (buttons 0010) arrived
and the game had it on **pad1** (player 2) 2 ms later; its A presses (car and transmission) likewise; pad0 stayed 0. So the other machine's controller is
player 2 on mine, as designed. The earlier surprise (the top car moving) is therefore not explained by routing: the other player's throttle was not
timestamped, so that run cannot be matched to the log.
Race clocks: mine read 49.99 at 16:06:22 and 1:41.77 at 16:07:14 (51.78 game seconds in 51.8 wall seconds: the game runs at real time here). The other player read
about 57 at the first moment (offset about 7 s, partly reading delay) and could not give the second reading. Reading a running clock by eye is not a
usable measurement. The 7 s may just be each machine's own loading time before its race clock starts.
**Needed:** a self-measuring test: each machine logs, with a shared time base, a number that depends on the race state (the race clock or a car's
position), and the logs are compared afterwards. That needs the address of such a value in the game's memory (found by walking objects, as done earlier,
the heap moves between launches) or a value the game exposes through a function. Alternatively each machine could send the other a small state digest
over the relay and log both, so one log holds the comparison.

## Lockstep netplay (built 2026-10-17, after the two-machine tests showed the cars staying together)
The other player reported that, with the pads linked, a car moved on one machine moved the same way on the other and at the same coordinates, until
it hit the first player's car. That is consistent with a deterministic game, and the clean way to use it is lockstep: both machines apply the same
inputs on the same race frames, so a collision comes out the same on both, because each machine has the other player's input for that frame.
- **Race frame:** `game::RaceFrame()` (game/frame_time.cpp): the number of game frames since the world clock first read above 0. The world clock is a float
  in seconds at guest address 0x82A153B8 (found by watching floats that advance one second per second in two snapshots; it equals the HUD race clock plus
  a constant 9.95 s; 0 in the front end; with `NFSMW_FIXED_STEPS=2` it advances 0.03333 s per world-step call). It makes both machines agree on the frame
  number whatever their loading times (race frames were not comparable before: loading takes a different number of frames).
- **Protocol** (net/netplay.cpp, scripts/netplay/server.py): FRAMES packets carry the local pad for the last 8 frames, each tagged with the frame it applies on
  (current frame + 3: an input delay of 3 frames, 100 ms at the game's 30 fps), so a lost packet costs nothing; the server forwards them.
- **Lockstep poll** (hid/input.cpp, `LockstepPads`): in a race, linked, with fixed steps, the game's controller reads for frame g return the local pad of frame
  g-3 and the other player's pad for frame g, waiting up to 2 s for it; the same answer for every read in a frame. Mapped to controller 0/1 by slot, so both
  machines give the game the same two pads in the same order. If the other input never comes (2 s), a message says the games are no longer in step.
- **Needs** `NFSMW_FIXED_STEPS=2` on both machines (fixed 2 world steps a frame: the same simulation steps on both), and the same game build on both.
**Checked here (one machine, the bot as the other player):** lockstep ran through a race (the bot saw the game's frame number advance; the game logged no late input);
the bot's throttle arrived on controller 1 and moved the second car; the race clock advanced normally. **Not checked:** two machines in lockstep, drift over a
long race and a collision (the point of the change), how much the 3-frame delay and a 42 ms link hurt the feel, and what happens when a machine's frame rate
differs (the faster waits for the slower). Both machines must be restarted onto the new build and use `NFSMW_FIXED_STEPS=2`.

## Lobby server (2026-10-17): the server learns about matches
Decision: the F8 link and the menu-by-hand steps exist only because the server knows nothing of a match. So the server grows into a lobby (scripts/netplay/server.py,
protocol in its header):
- **Rooms:** named, optional password, host (slot 0) and one guest (slot 1); `list`, `create`, `join`, `leave`, `ready`, `setup` (host only: mode, track, laps,
  traffic, whatever the client puts in the setup object), `start` (host only, when the guest is ready), `status`. Changing the setup clears the ready flags.
- **Events pushed to members:** `room` on every change, `start` (each player's slot, the setup, one shared random seed), `closed` (the host left).
- **Reliability over UDP:** each request carries an id; the client re-sends until it gets the reply; a repeated id is answered again, not run twice.
- **Game traffic unchanged:** `HELLO` by room name, `INPUT`/`FRAMES` relayed to the other member only. A player silent for 15 s leaves; the host leaving closes the room.
- `scripts/netplay/test_lobby.py` runs the server on a spare port and drives it with scripted clients: 29 checks (create, duplicate name, password, full room,
  ready, host-only setup and start, the idempotent request, start event, relay to the other member only, host leaving, the old HELLO way in), all passing.
**Not built yet (the client and game side):**
1. A lobby screen in the runtime's own UI (it already has an ImGui settings menu): server address, room list, create (name, password, the race setup), join, ready, start.
2. Starting the race without the manual menus and the F8 link: on the `start` event, drive the game's own front end with the agreed setup. That needs the front
   end's own function for "start this race" (the Split Screen flow ends in one), found and called, instead of key presses; and the pad link made automatic at the
   moment it asks for player 2. Not found yet.
3. Rematch and returning to the lobby after the race; a player dropping mid-race; the lockstep test on two machines.

## The lobby in the game (2026-10-17): Settings > Online
Built: a lobby client (`runtime/net/lobby.cpp`, with `net/json_lite.h`, tested in `tests/json_lite_test.cpp` under AddressSanitizer) and an **Online** page in
the settings menu (`ui/online_menu.cpp`).
- **Online Play** (Settings > Online, restart): every game frame runs exactly two world steps and the game's timer follows the frame (what lockstep needs): no
  environment variable any more (`NFSMW_ONLINE_PLAY=1` and `NFSMW_FIXED_STEPS=n` still work for tests). With it on, the page shows the lobby: server address and
  name (saved in `online.txt` in the user folder), Connect; the room list (name, players, password lock, race), Join (password if locked), Refresh; New room
  (name, optional password; the creator is the host); in a room, who is in it and who is ready; the host chooses the race (mode, track, laps, traffic) and
  starts it, the guest says Ready, either can Leave.
- **The start event** gives each player their slot and one shared seed and hands the lobby's connection to the game's input relay
  (`netplay::Attach`: the same socket, so the server knows the sender).
**Checked through the real game (keys scripted, a scripted host as the other member):** connect, the list showing "bot1 (1/2) Circuit, track 3, 2 laps, traffic Minimum",
create a room (the room appeared with the player as host), join the scripted host's room, Ready (the host saw it), the host's start (the game attached as player 2
with the seed and showed the match screen). **Not done:** the guest and the host both starting the game's own race automatically (below), the pad link without F8,
what the game does when a player quits mid-race, a password room tried in the UI, text fields on a controller (they need a keyboard).
**Still open (step 3):** on the start event, the game's own front end has to be driven to the agreed race instead of asking the players to click through
Quick Race > Split Screen by hand and press F8 at "Select player 2".

## Step 3, automatic race start: what was tried (2026-10-18)
Goal: on the lobby's start event, start the agreed race through the game's own code, with no menu navigation and no F8.
- `NFSMW_BT_OPEN=<text>` (kernel/imports_file.cpp) logs a backtrace of every open whose path contains the text. The race load's `TRACKS\<track>\TrackMaps.bin` open
  comes from a streaming thread (`sub_8284FE40` -> `sub_8261A318` -> `sub_8261EA18` -> `sub_82621280`/`sub_82621678` -> `sub_8284D6B8` -> NtCreateFile): the request was
  queued earlier by another thread, so the backtrace does not reach the front end. Not the way in.
- The executable's strings show the game has its own online lobby and race-start code: `UIOLGameRoom::StartGame`, `LobbyGames::StartGame`, `OLMGR: StartRace called at
  WorldTime`, `MSG_U_BI_STARTRACESYNC`, `OL_Quickrace_Main.fng`, `MMOnlineQuickRace`, `Game_StartRace: %s`, and the front end's `QuickPlay` / `SplitScreen` items. These
  are the native entry points to look for (their callers, and what a start needs: the race setup object and the player list). They reference each other through data
  tables, so a plain search for the string's address in the recompiled code finds nothing for most of them; the way in is the data tables (message names ->
  handlers) or running the game to the screen and breaking on its handler.
- **Not done**, and the largest remaining piece of native work. Until then the lobby's start event ends in a message on the Online page asking the players to start
  Split Screen by hand and link the pads (F8) at "Select player 2".
- Tools kept in `~/.cache/NFS-MostWanted/tools` (not in the repo): `launch_sp.sh` (game on a throw-away data copy, windowed), `view.sh`, `label.sh` (small captures
  of the window, to spare image quota).

## Step 3 findings (2026-10-18): how a race starts, and that the game has its own online race protocol
`NFSMW_PROBE_RACE=1` (game/race_probe.cpp, off by default) logs calls of chosen guest functions with registers and the guest call chain read from the stack.
- **Starting a race:** the handler bound to the script name "StartRace" is `sub_82365788` (it prints "Game_StartRace: %s"). It is called from the game's **script VM**
  (`sub_82371D18`: a bytecode interpreter; native calls go through `sub_8236BDE8` -> `sub_8237D3E8`), which runs inside the world step (`sub_823A2348` ->
  `sub_823A20C0` -> `sub_8239F140` -> `sub_823844B0` -> `sub_823C7D50` -> `sub_823C7950` -> `sub_823993C0`, an event dispatcher: its r5 is an event id, 0x319915CD for the
  one that led to StartRace in a split-screen race). So starting a race is posting that event with the race parameters already in the game's state; the front end
  prepares them as the player goes through its screens. A direct call of `sub_82365788` would skip that preparation. The producer that posts the event, and what
  state the screens leave behind, are not identified.
- **The game has its own online race code** (strings and code found in `sub_82554500..`): a server state machine `SERVERSTATE_INITIAL, WELCOME, WAITJOINS, LOADING, NIS,
  WAITLOADS, READY, RACING`, and messages `MSG_R_SC_WELCOME, MSG_R_BI_CARDESCRIPTION, MSG_U_BI_CARSPAM` (the cars' state, sent unreliably), `MSG_U_CS_CLOCKSYNC`,
  `MSG_U_SC_CARSPAMCLOCKSYNC`, `MSG_U_BI_SCORE`, `MSG_U_SC_SYNCANIMATION`, `MSG_R_BI_DRIVERFINISH`, `MSG_R_CS_CLIENTQUIT`, `MSG_R_SC_SERVERQUIT/CLIENTLEFT`,
  `MSG_R_*_STATECHANGE`, `MSG_R_BI_DATACRC`, `MSG_R_SC_RESTARTLOAD`, `MSG_U_BI_STARTRACESYNC` (R reliable, U unreliable; CS client to server, SC server to client, BI both),
  plus `OLMGR: StartRace called at WorldTime` and a lobby (`UIOLGameRoom::StartGame`, `LobbyGames::StartGame`, `OL_Quickrace_Main.fng`, `MMOnlineQuickRace`). This is the
  console game's native Xbox Live race: a server and clients exchanging car state, clock sync and state changes over the XNet/Winsock functions this runtime stubs
  (89 `NetDll_*`; the bring-up probe above reached `XNetStartup`/`WSAStartup` and stopped at `XamAlloc`).
- **Two ways to get native multiplayer, and the trade:**
  A. (built, partly tested) the lockstep over the game's two-player split-screen mode: the game itself needs no change, but both machines must simulate identically and
     each draws two views; the race start needs the front end driven.
  B. Supply what the game's own online stack asks of its platform (sockets, XNet addresses, the session and lobby services EA ran) and let it race itself: remote cars
     arrive as real game objects through its own car-state messages, one view per machine, no determinism needed, and the race starts through its own lobby path. It
     needs: the Winsock and XNet calls on host UDP, `XamAlloc`/the other bring-up calls, a replacement for EA's matchmaking (our lobby server telling both machines
     the peer's address), and then whatever the lobby and session code asks of the (dead) web services. Unknown size; the code is large and not written to be reused.

## Option B, first stage (2026-10-18): the game's network stack on host sockets; its lobby is EA's FESL-style protocol
Built: `kernel/imports_net.cpp` implements 64 of the stubbed network imports on host sockets: `XamAlloc`/`XamFree`, xnet (startup, `XNetGetTitleXnAddr` with this
machine's address (`NFSMW_NET_ADDR`), keys, address conversion where the secure address is the real address, DNS (`XNetDnsLookup`: every name resolves to
`NFSMW_NET_HOST`, default 127.0.0.1; XNDNS.iStatus is 0 for success), QoS refused), winsock (sockets, bind/connect/listen/accept, send/recv/sendto/recvfrom,
select, ioctl, options, `WSAEventSelect` through a poller thread, event objects, `WSAWaitForMultipleEvents`); `NFSMW_NET_LOG=1` logs the calls. Argument lists were
checked against the game's own calls (the event calls and `WSAGetLastError` take no caller argument; the others do). `scripts/gen_import_stubs.py` now lists 103 stubs.
**What the game's own online flow then does** (`NFSMW_FAKE_LIVE=1 NFSMW_STUB_CONTINUE=1 NFSMW_NET_LOG=1`, Xbox Live menu item): `XNetStartup`, `WSAStartup`, the
address, a TCP socket, a DNS lookup of **stestlobby03.beta.ea.com** (EA's beta lobby, baked in), a TCP connect to **port 30960**, then two messages in one stream
with no waiting for a reply:
  `@tic` + 4 zero bytes + length (23, big-endian, header included) + `RC4+MD5-V2\0`      (an encryption negotiation)
  `@dir` + 4 zero bytes + length (174) + text lines `REGN=NA`, `CLST=192641`, `NETV=0`, `FROM=US`, `LANG=EN`, `MID=`, `PROD=nfs-xbox-2006`, `VERS="xbox/1.2-Nov...`  (the directory request)
This is the console game's connection to EA's lobby service: tagged messages (4 characters, 4 bytes, a 4-byte length) carrying `KEY=VALUE` lines, with a negotiated
RC4 stream and MD5 integrity and what looks like an RSA public key (the strings "Public Key", "SKEY", "DIRECT", "INGAME", build date Oct 19 2005 sit beside them). A
directory server answers with where the next servers are (account, lobby); login, personas, game rooms, game creation and joining follow on other connections.
**What option B needs:** a server that speaks this protocol as the client expects: the encryption handshake (and, if the game checks the server's key against the
public key in the executable, replacing that key in the game's memory at start), the directory, authentication, the lobby and game-room services, and finally the
peer-to-peer race protocol the lobby sets up (the SERVERSTATE machine and CARSPAM messages above). None of it is documented that I know of; each message's form
would be read from the game's own code and tested one reply at a time. That is a long project (many sessions), with a native result at the end.

## The game's own direct-connect mode (found 2026-10-18): online.cfg
No EA server is needed for the game's own network race: the executable contains a developer configuration that skips the lobby entirely.
- **The file.** `sub_82538950` loads `D:\online.cfg` (here `game/files/online.cfg`), `KEY=VALUE` lines, **CRLF line endings required** (with plain LF only the first lines were
  applied, found by reading the parsed globals back from the running game); `sub_82538C10` applies each key to a global (`stricmp`, then `atoi` or a string copy).
  Keys: `SKIPFE`, `SKIPFEPLAYERCAR`, `SKIPFENUMAICARS`, `SKIPFENUMLAPS`, `SKIPFETRACKNUMBER`, `SKIPMOVIES` (start a race without the front end), `ONLINEENABLED`,
  `ISSERVER`, `NUMCLIENTCARS`, `SERVERIP`, `SERVERPORT`, `CLIENTIP0..2` (a direct host and clients), `NetworkUseLobbies`, `NetworkLobbyIP/Port`, `NetworkUserName`,
  `NetworkPersona`, `NetworkPasswd`, `NetworkGameName`, `AutomaticLogin`, `NetworkDebug`, `GAMESPEED`, `USERECORDABLE`, `FORCECLIENTSHUTDOWN`, `SOUND`, `DIAGNOSTICLEVEL`.
- **Nothing in the release build calls the loader** (no caller, no pointer to it), but the code that reads what it sets is live: the main loop (`sub_823B0008`) creates the game
  (`sub_823AF408`), reads `SKIPFE` (`0x82A2CD44`) and starts either a race straight away (`sub_822BE238`) or the front end. **`NFSMW_ONLINE_CFG=1`** (game/online_cfg.cpp) calls
  the loader once, right after the game is created. With `SKIPFE=1` the game does skip the front end (a loading screen replaces the title).
- **The network race setup** is `sub_822BE418` (called when `ONLINEENABLED` is set): it starts the network module (`sub_82539A38`: the EA DirtySock-style `NetConn` state
  machine and a lobby thread `sub_8253A4E0`), **waits for a 64-bit value at `0x82A2D7C8` to become non-zero** (the local identity/address formatted as `$<16 hex digits>` by
  `sub_8281B578`), then as a server (`ISSERVER`) calls `sub_82546F80(id, 3)` or as a client `sub_825471D8` towards `SERVERIP`, registers each `CLIENTIP<n>` as an expected player
  (`sub_825479D8`, names "TheHost", "Client%d") and waits until `NUMCLIENTCARS` players have joined.
- **Where it stops now:** the `NetConn` state machine (`sub_82819520`, states 0-4) polls `XNetGetTitleXnAddr` about 1000 times a second and never leaves state 1. It decodes the
  address flags (`sub_82819018`: `GATEWAY|DNS` -> '+isp'; the real flag values are NONE 1, ETHERNET 2, STATIC 4, DHCP 8, PPPOE 0x10, GATEWAY 0x20, DNS 0x40, ONLINE 0x80,
  TROUBLESHOOT 0x8000; an earlier version returned wrong values), and drives itself from Xbox Live notifications (`XN_LIVE_CONNECTIONCHANGED` 0x02000001 with status codes
  0x801510xx for failures, `XN_LIVE_LINK_STATE_CHANGED` 0x02000002). The notifications are delivered (`NFSMW_FAKE_LIVE=1`), the address is accepted, and state 1 still requires
  `[conn+328] == 1` and `[conn+329] == 0` (set by the module's start-up parameter string, passed to `sub_828198C0` as `0x8206xxxx+15936`), which has not been read yet.
- **Tools** (not in the repo, in `~/.cache/NFS-MostWanted/tools`): `image.bin` (a dump of the guest image), `xref.py` (code and data references to an address), `callers.py`,
  `fstr.py` (strings used by a code range), `live_probe.sh`, `launch_sp.sh`, `view.sh`, `label.sh`. The in-repo ones: `NFSMW_PROBE_RACE`, `NFSMW_NET_LOG`, `NFSMW_BT_OPEN`.
- **Next:** read `sub_828198C0` (the `NetConn` start-up and its parameter string) and the state machine's exits so the network module reaches its connected state with sockets
  of its own (no EA server involved), then the race setup above. Then the race itself runs on the game's own protocol (SERVERSTATE, CARSPAM), one view per machine.

### Experiment: forcing the local-address word (NFSMW_ONLINE_LOCALIP)

Setting the word at 0x82A2D7C8 (what sub_822BE418 waits for) after the online.cfg load lets the setup continue into sub_82546F80
(server start), which then crashes at +0x4e4: it calls sub_82542C98 (a connection/player-object constructor, `this`=0x82C652D0), and its
field +372 stays -1, so the code takes `r29 = 0` and reads [0+12]. So the word alone is not the missing piece: the object that
sub_82542C98 should have filled in (its +372 handle, normally set by the platform session/matchmaking layer) is also missing in this
mode. Disassemble any guest range with `scripts/mp/disasm.py <addr> [bytes]`.

## Direction (v2): a multiplayer layer of our own, not the game's EA stack

The game's EA online stack (lobby protocol, DirtySock NetConn, online.cfg direct connect) is abandoned: it needs a platform session
layer that does not exist here. Career mode is not ported. The new layer is:

- **Transport: ENet** (vendored, `runtime/thirdparty/enet`, MIT, plain C, no dependencies). Chosen over Valve's GameNetworkingSockets
  because GNS needs protobuf and OpenSSL/libsodium, which are system packages on Linux and a heavy port on iOS and Android, while
  SpeedBreaker ships on Linux, SteamOS, macOS, iOS and Windows. ENet builds from its own sources everywhere. The transport sits behind a
  small interface so a GNS backend can be added on desktop later. With a dedicated server, NAT traversal is not needed; encryption and
  authentication (session tokens, libsodium or a vendored primitive) are to be added in the protocol.
- **Model:** each client runs the game's physics for its own car and sends its state about 30 times a second; other players' cars are the
  game's own opponent vehicles driven by interpolated state; the server settles collisions; one client is the traffic and police authority.
- Test: `tests/enet_loopback_test.cpp` (`-DSB_TESTS=ON`).

## The player's car in memory (found 2026-10-18, Quick Play race, single player)
Method (scripts/mp/): `quickrace.sh` reaches a race through the menus on a throw-away data copy; `findpos.py` takes the car position from a static copy
(guest 0x82A2DBF0 holds the player's world position as three big-endian floats, metres) and finds every other place that holds the same triple and
follows it while driving; `localvel.py` reads the area around it twice and lists triples equal to (pos2 - pos1) / dt.
Result: the player's rigid body state is a heap block laid out as
- `-48 / -32 / -16`: three 16-byte rows (x y z 0) of the orientation matrix (right, forward, up as seen: row 0 = (0.80, -0.60, 0.01) ...),
- `+0`: world position (x, y, z, 1.0) in metres (x, y ground plane, z about 120 on the first Quick Play track),
- `+16`: linear velocity (x, y, z, 0) in m/s (matches (pos2 - pos1) / dt),
- `+32`: a second triple (looks like angular velocity or a previous velocity: not yet separated),
- `+112` of the block is a vtable-like pointer (0x8208D884); the block start and the pointer chain from globals are not yet identified (a global at
  0x8290BE18 holds a pointer to the block at -0x40 of the position but the other entries of that list are not bodies).
The heap moves between launches, so a fixed address does not work; the car has to be found through globals or by the position match against
0x82A2DBF0 at start-up.

## Multiplayer v2 status (2026-10-18): the game's cars driven over our own network layer
Pieces (all off unless the environment variables are set; nothing changes for normal play):
- `runtime/net/mp_proto.h`, `mp_client.*`, `mp_server.*`, tools `mp_server [port]` and `mp_bot host[:port] [delay s] [offset m]` (target `mp_net` and the two programs);
  tests `tests/mp_net_test.cpp`, `tests/enet_loopback_test.cpp`.
- `runtime/game/mp_adapter.cpp` hooks `sub_82528448(body, dt)`, the game's step of one rigid body (the list head is 0x8290BE18, see "The player's car in memory"), runs the
  game's own step first, then (a) reads the local player's car (the body at the static position copy 0x82A2DBF0) and sends it at 30 Hz, (b) writes the received state of a remote
  player into the body that stands for that player (matrix, position, linear and angular velocity). The rendered car follows the body.
  Variables: `NFSMW_MP_SERVER=host[:port]`, `NFSMW_MP_NAME`, `NFSMW_MP_BODIES=i,j,...` (list positions of the stand-in bodies, one per remote player in id order; the log shows the
  list every 3 s), and for a test without a network `NFSMW_MP_GHOST=<s>` + `NFSMW_MP_BODY=<i>` + `NFSMW_MP_OFFSET=<m>`.
- `scripts/mp/quickrace.sh <name> [env]`: menus to a Quick Play race on a throw-away data copy.
**Checked (one machine, the real server and protocol):** ghost mode: the opponent body follows the local car 3 s late (positions match to a metre) and with a 5 m offset the
rendered opponent drives next to the player. Network mode: `mp_server` + `mp_bot` (replays the player delayed 3 s, +8 m in x): the game log shows "net up, 1 remote, rtt 74 ms",
the stand-in body (#1, a grid opponent) sits at the local car's position + 8 m when the delay is short, and 40 m behind it with a 3 s delay.
**Stand-ins are found without picking by hand:** the game keeps its racing vehicles in a fixed vector at 0x82C745A8 ({vtable, data, capacity in bytes, count}; 8-byte entries
{owner object, 1}; the player first, then the opponents in creation order); a vehicle's body is the body whose +8 is that owner. Remote players, in id order, drive the opponents in that order
(`NFSMW_MP_BODIES` still overrides). Checked with `mp_server` + `mp_bot`: "remote player 1 (bot) drives body BBE1A3C0", at the local car's position + the bot's offset.
**Not done yet:** what happens when the local car hits a stand-in (it is overwritten
every step, so it is immovable; the plan is to blend the state in and let the game's collision act), AI of the stand-in cars switched off, car model per player, start
synchronisation, laps and finish shared, a real two-machine run, lobby, modes.

## Findings from the first real two-machine runs (2026-10-18/19, ARM Linux <-> x86 Linux over ZeroTier, rtt about 30 ms)
- Works: both machines connect to `mp_server`, each sees the other's car and its movement, cars collide (after writing the **physics body** instead of the tracker copy).
- **The physics body** (docs above): the list the step sub_82528448 walks holds *trackers*; a tracker's owner (+8) has a component (+128) whose +48 points at a pointer to the
  rigid body in a static array: quaternion +0, position +0x10, linear velocity +0x20, angular velocity +0x30, mass +0x2C, matrix rows +0x80. Axes: tracker (x, y, z) = (body z, -body x, body y).
  The step reads the body (vtable +64 matrix, +32 position, +36 velocity) and fills the tracker, so writing the body before the step moves both the car and its collisions.
- **Every game puts its own player in grid slot 0** and drives the cars into the grid itself (about 26 m of rolling, before the countdown), so a teleport to another slot is undone.
  Workaround: a remote car is shown 3.5 m per join-order step beside ours from the moment it is first seen until it has stood still on the grid and driven 20 m.
- **The two machines are not at the line at the same time**: loading and the intro (about 15 s of fly-in and burnouts, then 3-2-1-GO) take different times on each machine, so one car
  can be on the line while the other is still rolling in. Fixing that needs a start barrier (hold the race until every player is at the line). The dispatcher sub_823993C0 saw a single
  event id (0x319915CD) in a whole race, so the countdown is not driven through it; the strings `StartCountdown`, `NotifyCountdownDone`, `EShowRaceCountdown` and the OLMGR messages
  (`OLMGR: ReadyStartLine called.`, `mPostCountdownStartRaceTime`) are where to look next.
- Not done: car model per player (the opponent keeps whichever car the game picked), minimap blips for remote cars, start barrier, laps/finish sharing, lobby and modes.

## Grid slots and the start barrier (2026-10-19)
- **Start barrier** (`NFSMW_MP_PLAYERS=n`, game/mp_adapter.cpp + game/frame_time.cpp): when the race appears (the vehicle table's first entry is a new body, about 0.4 s before the intro) the
  world is held (`game::SetWorldHold`: the frame is given 0 time and the stepper 0 steps), each game sends its car state with `kFlagReady`, and everyone is released when n-1 others are ready
  (timeout 5 minutes). Checked between ARM and x86: both leave the loading bar together, countdown and GO together.
- **Grid slots**: every game puts its own player in slot 0 and drives all cars into the grid with `sub_82420FC0(vehicle, dt, pose, flag)` (pose = a 4x4 matrix in the body's axes, translation row at
  +48), one call per racing vehicle per frame in the vehicle table's order. The local car's call is given the pose that join-order slot's vehicle was given (copy of the 16 words), so the car
  starts in the slot of its join order. A remote car then sits in the opponent body that stood in that slot. Checked: separate lanes at the line on both machines, contact works, they stay in their lanes after GO.
- **Not done**: car models. The opponent is whatever the race script created (`sub_82333680`, the native that creates a racing vehicle, is called per opponent with a 32-bit key in r4 that is a racer
  (Attrib collection) key, not a car preset); the car presets are hashes in the game's data (no ASCII `lexusis300` anywhere, only display names like "Lexus IS300"). To show the other player's real car the race
  has to be created with that car, i.e. the racer/preset that the script hands to `sub_82333680` has to be chosen, which needs the Attrib tables of racers and presets decoded. Probes: `NFSMW_PROBE_RACE=4`.

## Race timing between machines (2026-10-20)
Measured with the race clock (the world clock at 0x82A153B8) that every game sends in its car state (protocol version 2, `CarState::raceClock`); the log line
`clock of player N minus mine` shows the other game's clock relative to ours.
- **Game speed**: the old Settings > Online > Online Play (two world steps a frame, whatever the frame took) made the faster machine's world run about 1.45x real time. The multiplayer
  races now take precedence over it (game/frame_time.cpp), so both worlds run at real time.
- **Start**: the race appears at a different moment of loading on each machine and the slower one's game time lags for a few seconds after the release (up to about 4 s). The barrier
  releases together, and every game then follows an agreed clock = the slowest game's clock at the release + real seconds since (`game::MpWantedClock`, applied in `GameFrameTime`: at most
  +100% / -50% of a frame's time), so the clocks close to within about 0.2 s within 3-6 s, before GO. Checked ARM <-> x86: countdown and GO together, HUD timers equal.
- **Vehicle table**: the count word at 0x82C745B4 grows with the traffic (29 seen about a lap in) while the table has room for 20 entries: read at most 20. A count above the room was once taken
  for an invalid table, which made the race look over and re-armed the barrier in the middle of the race.
- The barrier can be left with B / Back (a toast says so); the ready flag stays on 20 s after the release (a game that released at once is otherwise seen as ready for one packet only).

## Car models (solved 2026-10-21, lobby-based) and names
**How it works**: every car has a 64-bit type key, the string key (`sub_82144CA0`) of its folder name (IS300, RX8, ...), and a template record in the heap (key at +16, dimensions before it);
a vehicle's creation record is at `owner + 0x74` (the template itself for the player's car, a copy for opponents). The race script creates each opponent with `sub_82333680(call, racer key)`;
the car is instantiated from its template by `sub_82147BF8(layout, template, ...)`, and passing another car's template while `sub_82333680` runs makes the opponent that car (checked: all
opponents RX8, with the RX8's mass 1631 kg and inertia). game/car_select.cpp does that, and builds a catalogue from the game's own name table (the heap run `RX8, rx8, LANCEREVO8, ...`;
`NFSMW_LOG_CARS=1` prints it: 92 names with templates, cops and traffic filtered out). The player's own car cannot be changed from outside (its template pointer comes from the front end's state
through the vehicle constructor `sub_823D83E8`; not found), so the lobby sets what the *others* see: each player chooses a car in the lobby and marks itself ready (`PlayerInfo`: car key and ready flag, protocol version 4); every game makes its
opponents those cars, in player id order (the order the stand-ins are handed out in), and checks after creating its own player that the car is the lobby car (message if not). **A swapped
car is only drawn if the swap happens within about a second of the player's creation**: waiting inside `sub_82333680` (tried: announce the actual car after creation and wait for the others)
leaves the opponent without a body (wheels only), reproduced on one machine with `NFSMW_TEST_TEMPLATE=<key> NFSMW_TEST_OPP_DELAY=<s>`: delay 0 draws the car, 4 s and more do not. So the
choices must be known before the race loads; the connection is kept from the moment multiplayer is on. The world hold of the start barrier is armed only after the opponents are created.
**Why wheels only, and the fix (2026-10-21)**: a car body swapped into another car's racer is drawn as wheels and smoke (reproduced locally: Punto into the TT's slot, also for all three
opponents), because the parts and paint come from the racer the script names. The key `sub_82333680` gets in r4 is a 32-bit Attrib collection key: the Jenkins lookup2 hash (seed 0xABCDEF00)
of the lower-case car name (`tt` 193A8BC8, `cobaltss` 5A784095, `punto` A6EE7A11 are the three opponents of the Ironwood circuit race). So for a wanted car r4 is set to the hash of its name
and the game builds the opponent whole by its own generic racer path (checked: A3, Clio, Monaro, 911 Turbo, A4, Carrera GT, CTS, Impreza, GTO, RX-7, Murcielago, Cobalt SS all load their own files;
a solid Cobalt on the grid). The table of preset rides (`sub_824EF790`) is a different path and not needed. The template swap stays as a second safety net (it is a no-op when the keys agree).
The 32-bit key the creation looks up first (`sub_821E02C0`, same for the player and an opponent) is a vehicle behaviour preset, not the car.

## Earlier attempts on car models and names (2026-10-20), so they are not repeated
- The race script creates each opponent with `sub_82333680(call, racer key)`: r4 is a racer (Attrib collection) key, not a car preset; presets are hashes in the game's data (no ASCII preset names in memory, only display names
  such as "Lexus IS300"). The player's car goes through `sub_824729E0` / `sub_82473100` / `sub_82473870`, which are generic (called for many objects).
- The 64-bit values that looked like a per-car key at owner+0x84/+0x88 are not stable (small numbers and pointers in the next race). `sub_8212BFB8`, which is on every vehicle's creation path, is a generic hash helper.
- Opponent names in the HUD standings come from text tables (looked up by key); the player's alias is a wide string in a profile struct (about 0x82C5E19C).
- The camera is in static data: eye at 0x82A2DAC0, forward at 0x82A2DAE0 (tracker axes), look-at point at 0x82A2DB00. Name tags over the cars would need that plus the game's field of view and the picture fit
  (letterbox, Hor+/Vert+ view scale: video/picture_fit.h) to place text at any window shape; not done.

## The server owns the start of a race (protocol 5, 2026-10-22)
The start barrier is decided by the server (`net/mp_server.cpp`, `MaybeGo`), not by each game looking at the others' ready flags: a game held at the start of a race sends `Hold` (players the race needs,
its world clock), the server tells the held players how many are there (`Barrier`) and, when everyone is, sends `Go` with the slowest world clock and the time to start in (300 ms, less half the
round trip of that player, so all games start at the same instant). A game whose world clock is ahead waits the difference, so countdown and GO come together, as before. `Unhold` (cancel with B,
race left) and a disconnect take a player out of the barrier. Tested on loopback (`tests/mp_net_test.cpp`) and in the game with the bot (`[mp] start barrier released ... the server released the start`).
Next on the server: finish order and results (needs the game's finish event), then the headless server game (see the notes on `NFSMW_HEADLESS=1` in the memory file).

## Player names in the standings (2026-10-22)
A race's racers are records of 0x1A0 bytes (the first is the player, number 1, then the opponents in creation order); the record's +8 points to the racer's name, which
`sub_82335248(record)` picks (a random driver name from the table of names at about 0xBABCA4xx) as the race script creates each opponent. The standings screen (and the HUD's racer list,
which `sub_822A0B80` refreshes every frame in position order) read the name through that pointer. game/mp_adapter.cpp hooks `sub_82335248`: for the opponents that are other players (in the
order of the players in the race, id order, as their cars are made) the pointer is set to the player's name (capitals, digits, space, `-`, `_`, ten characters) in a block of the game's own heap.
Checked locally with a bot: the standings list the bot's name. Dead ends on the way: the ascii copy of the names at 0xA8FAC198 (the HUD's list, rewritten each frame from these pointers), and the UTF-16 copies
(built when the screen opens; writing them or the list after the race starts is too late).

## The game's own dev start works (2026-10-22): NFSMW_DEVSTART
`NFSMW_DEVSTART="car=is300,track=2000,ai=3,laps=2"` starts the game without the front end (game/online_cfg.cpp): the settings are written to `<user data>/devstart/online.cfg` and the VFS
(`vfs::SetOverride`) gives that file to the game's own `online.cfg` loader when it asks for `D:\online.cfg`, so nothing is put in the game's folder. Why the first attempts hung or crashed
(2026-10-18 and today), each found by logging the game's own routines:
1. `sub_822BE238` (the dev start) calls the network race setup `sub_822BE418` unless `ONLINEENABLED=0` (the default is on): it waited for a network that is not there. `ONLINEENABLED=0` skips it.
2. The track number must be in the game's track table (count at 0x82A2D350, array pointer at 0x82A2D34C, 288 bytes an entry, the id as a 16-bit number at +138, the folder name at +64): the
   table has 2000 ("Most Wanted World", folder L2RA: the open world), 2200, 2400, 2401, 27xx (test tracks), 2801, 2901, 2902 and 5400 (Carbon). With an unknown number (the sample file had 1000)
   `sub_823B2CE8` stores a null track object in the global at 0x82A2D358 and the next step formats its name at address 0x40: SIGSEGV at 1.5 s.
3. The car is looked up as an Attrib collection by the hash of its name, which is case-sensitive: `is300` works, `IS300` makes `sub_8212BFB8` return 0 and `sub_824729E0` crash at 4.8 s.
Result: the player's car (checked: the Lexus) in the open world, a free drive with no race and no menu, 60 s after launch, also with `NFSMW_HEADLESS=1` (no window, a null GPU: 30 fps,
about 80 % of a core, 500 MB; the vehicle table has the one car). Next: start a race from there (the race event the front end's Custom Race would pick) so a server or a host can choose it.

## Open world mode (started 2026-10-22; NFSMW_MP_WORLD=1 with NFSMW_DEVSTART)
The design the user asked for: players share the game's open world (the dev start boots into it), a custom marker starts a race that happens in that world, other players can see and
influence it, and more than one player can start it. First piece, working on one machine with a bot (game/world_mode.cpp, mp_adapter.cpp):
- **A vehicle for a remote player, made in the world.** The game's own opponent creation (`sub_82333680`, the script native a race script calls) is called from the game's frame
  hook with a hand-made call object (+0 receives the object, +4 points at an empty parameter block) and the racer key of the player's car (hash of the lower-case folder name). It adds a
  vehicle to the vehicle table, found as the owner that was not in the table before, and that owner is registered as the player's stand-in, so the body hook that drives a race's
  stand-ins (`ApplyRemote`) writes the player's state into it every step. Checked: a Supra driven by the bot's state, solid, 9 m ahead of the Lexus in the open world.
- **The limit found: the car's files are only requested for a vehicle made soon after the player's own.** The same call made 5 or 40 frames after the player's creation opens
  `CARS\SUPRA\GEOMETRY/TEXTURES/VINYLS.BIN` (and the car is drawn); made 120, 300 or 900 frames after (about 4 s on), the game asks for no file and the car has no model. So a
  vehicle must be made while the world is still loading: for every player already on the server when this game's world is created (`MpWorldNextSpawn`: a player with an announced car
  gets its vehicle at once, without waiting for its car state). A player that joins later is not drawn by the others yet: the open question is what the game does in those first seconds
  (a loading state that stops accepting car requests); static-memory diffs of the first seconds found only per-frame tables, not a flag.
- **Not done yet:** the marker and the race itself (server-owned: checkpoints, laps, order and results computed on the server from the reported positions, a screen overlay for the marker
  and the HUD), vehicles for late joiners and removing a vehicle when a player leaves, a settings switch instead of the environment variable.

### The car pool: late joiners and AI racers are drawn (2026-10-23)
What blocks a car made late (traced with gdb breakpoints over the game's functions in the two cases, early and late): the vehicle is registered and its file paths are built, but the car's files are
only fetched for a vehicle made soon after the player's own (the loader's job state differs: `0x82C5C4F0+20` is 1 early and 0 late, and a request node is in another state in the streaming code
`sub_823BBD20`); setting state words, growing the job pool and servicing the loader's job list by hand did not reopen it. So nothing is made late: right after the player's car exists, while the
world still loads, game/world_mode.cpp makes a pool of six vehicles (the game's opponent creation makes at most the player plus six: the vehicle table stops at 7): first the cars of the players
already on the server, then BMW M3 GTR, Lancer Evo VIII, RX-8, Supra, Mustang GT, Cobalt SS. A pool vehicle that nobody drives is held far below the world (its body is written every step in the same
hook that drives a stand-in); a player or an AI racer that appears later takes a free one of its car, or any free one (a different model) if its car is not in the pool; a player who leaves gives it back.
Checked: a bot joining 25 s after the world loaded is a solid car; a bot that was on the server first gets its exact car (a green Supra with its own livery).

### Two whole game clients, and Windows with Linux (2026-10-25)
Tested with real game clients, not bots (protocol 9):
- **Two Windows clients on one PC** (each its own profile and `multiplayer.txt`, open world mode, `mp_server.exe`): each makes a vehicle for the other from the pool and draws it where the
  other's own log says it is (IS300 and RX-8, names over the cars). Found and fixed on the way: every dev start puts the car on the same spot, so the two started inside each other and the
  collision threw one off; now a car still on that spot 3 s in moves along the road by its rank among the players' ids (`[mp] spawn spread`).
- **Windows and Linux**: the Linux `mp_server` in a container of its own (`archlinux`, the port published on 127.0.0.1), the Windows client on the host, and a Linux client in `sb-build` on
  a shared Docker network, rendering with lavapipe on an Xvfb display (25 fps in the world; with `NFSMW_HEADLESS=1` the game reads no input, so a headless client can't be driven). The Linux
  car's own path (982,2148 -> 992,2165 -> 993,2164, tracker axes) is what the Windows client drew (992,2165 / 992,2164), and the Linux client drew the Windows car's +10 m spawn move.
- In the dev start the car faces a house across the road: a pad that only accelerates drives into it and spins the wheels. Outside open world mode, the dev start counts as a race, and
  the start barrier holds it until the players arrive (B: start without them).
- **Delay and loss** (the server in a container with `tc qdisc ... netem` on its interface, two Windows clients, one driving; `NFSMW_MP_TRACE_REMOTE=1` logs each remote car's pose every
  50 ms, and the speed between two samples is compared with the car's own speed):

  | server's link | rtt | speed between samples vs own speed, abs. difference: median / p95 | worst | jumps (over 1.5x own + 5 m/s) |
  |---|---|---|---|---|
  | none | ~10 ms | 0.90 / 3.68 m/s | +4.2 m/s | 0 |
  | 80 +- 20 ms, 2% loss | 85 ms | 0.94 / 5.03 m/s | +8.5 m/s | 0 |
  | 150 +- 50 ms, 5% loss | 162 ms | 1.74 / 7.49 m/s | +14.6 m/s (0.7 m in 50 ms) | 3 |

  The car stayed where it should be with a VPN-like link; with jitter past the client's fixed 100 ms interpolation delay it was extrapolated now and then and stepped by under a metre.
  Then the client was changed (net/mp_client.cpp, Clock): a state is placed in time by the sender's own clock (`ownerTimeMs`, mapped to ours by the smallest offset seen), not by
  when it arrived, and the delay it is drawn behind follows the measured excess delay (mean + 3 deviations + 40 ms, 100-300 ms). The same three runs after it:

  | server's link | median / p95 | worst | jumps |
  |---|---|---|---|
  | none | 0.10 / 0.48 m/s | +3.7 m/s | 0 |
  | 80 +- 20 ms, 2% loss | 0.19 / 0.83 m/s | +8.4 m/s | 0 |
  | 150 +- 50 ms, 5% loss | 0.25 / 1.73 m/s | +6.3 m/s | 0 |

  Speeds in these runs were low (up to 16 m/s).
- **The start barrier in a real race** (after the free roam fix): two race replays of `race.jlg` (a Quick Race with three AI opponents) at once, multiplayer on with 2 players to wait for. Each
  game was held at the start once its opponents existed, the server released both together, and each gave its first opponent to the other player ("remote player 2 (TWO) drives opponent
  0"). The replays then left their recording (a car driven by another player is not the recorded one), as expected.
- **A Quick Race through the menus, two clients** (fresh profiles, a virtual pad: START, A for "no profile", Quick Race, Custom, Circuit, Ironwood, 2 laps and 3 opponents, a
  car, the gearbox): both games with Ready ticked and their lobby cars (an RX-8, a Supra). Each game made its player car the lobby car whatever the car selection said (the summary
  showed the IS300) through the SKIPFE path, made opponent 0 the other's car, held the start with 3 opponents made until the server released both, and both raced with the other's
  name over its car. The paint still differs (the red RX-8 was purple on the other machine).
Still to do on two real machines: the Steam Deck with a PC over Wi-Fi or a VPN.

### Same paint on every machine: what was tried (2026-10-25; solved by sending the skin, see the next section)
An opponent or pool car of a given model gets a different paint and livery on each machine and in each run (an RX-8 was green, red-black, silver-blue). The aim was the smaller one:
the same paint for a player on every machine, by making whatever random choice picks it the same. Probed, with the open world's pool (made through `sub_82333680`) and a race
replay's opponents:
- The C runtime's `rand()` (`sub_825A9370`, its two-call form `sub_825A9398`; seed at 0x828F6A54): never called while a car is made, nor at all in these runs.
- The per-thread `rand()` (`sub_826BD8C8`, constant 214013, state at +20 of the thread's data) and its one caller `sub_82469FF8` (next to the vehicle creation code): not called
  either.
- The other LCG-looking code (0x19660D in `sub_82722AC0`, 16807 in `sub_82894C38` and its neighbours) is the audio dither and library code; `sub_823B72D0` is the Joylog's read
  (0 outside a replay), not a generator.
So the choice is made elsewhere: by a generator of the game's own with another formula, or after creation when the model streams in.
- Then the vehicle's memory right after `MakeVehicle` (the RX-8 the first pool car, four runs: lime green, green and black, pink, teal): its owner object (0x200 bytes) and the
  0x100 bytes behind each heap pointer in it. Two runs with the same heap layout and different paint (lime, pink) have the same words everywhere outside the pointers, so the paint is
  not in that object or one level below it at that point: it is chosen later (as the car's model and textures stream in), or kept in an object the vehicle doesn't point to yet.
- The same 300 frames later (the model loaded; five runs: purple, green, copper, red with a white vinyl, purple with a white vinyl), two levels of pointers deep: about 5000
  words differ between the runs, almost all geometry, streaming indices and floats; no small set follows the paint. The scripted preset rides (`sub_824EF790`, AndhikaFW's first
  approach) are the game's named cars (the Blacklist's), not a car's stock look.
- Meanwhile a player's own race car became the stock one (the SKIPFE path, see the lobby car), which looks the same in every run (a red RX-8, a black Supra), so what's missing is
  making an opponent come out stock too.
- Not a shader constant: the pixel shaders' c0..c63 of one frame in three runs (yellow-green, turquoise, dark red RX-8, the same scene) differ only in one scalar that follows
  the draw order; so the paint and vinyl are in a texture (the car's skin is put together when it is made).
- A lead that wasn't one: the RX-8's type key sits 76 bytes before a word that differs in every run, and holding that word didn't change the paint. A hardware write
  breakpoint on it (DR0, every thread; the host instruction mapped to its recompiled function through PPCFuncMappings) found the writer: `sub_824618C0`, a 24-byte list node
  allocator (its pool at 0x82A39580, the node's +0 to +8 from r4..r6, +12/+16 the pair at r7), called from 0x82460D6C with resource keys. The word was just a node that heap reuse
  put there; the keys are the same set in every run (70 of 81, the rest still loading), in another order. So nothing in the vehicle's memory has shown the paint yet.
- Every draw of the 240 frames after the RX-8 pool car is made (about 76,000 draws a run; turquoise with a red vinyl, gold, dark green in three runs), with the pixel shaders'
  c0..c31: no constant of the same shaders reads as the car's colour in each run. So the colour reaches the GPU another way (vertex colours, vertex shader constants, or a
  palette texture the skin samples).
- One normal frame with every draw's vertex shader constants (c0..c255) and pixel shader constants (c0..c63), purple against lime green: the only colour-like rows are world
  matrix rows (their fourth component is a position). Every texture of the car shaders (ps_7f7cf48a), its first 4 KB hashed, brown with a blue vinyl against silver: the same in
  both runs. The textures that differ belong to the frame's own targets (reflections, the environment cube, the grade), which hide anything else.
- Every draw's vertex buffers hashed whole (green against purple): the car shaders' buffers in the scene are the same; the only ones that differ are a shadow map's (another
  car in its frustum). So the colour isn't in the vertex data either.
- The cars' own textures (sampled by the car shaders 48553c3b, 781d5cda, 793a572b, ca767628; loaded at other addresses each run, so matched by size and order, first 64 KB
  hashed; silver with a red vinyl against pink): of 20 per run, 9 have the same contents in both runs, 11 differ, among them a 1024x1024 one of a pair whose other half is the
  same, and the 32x32, 128x64, 256x128 and 256x256 ones of both cars. The 1024x1024 one is the best candidate for the composited skin (paint and vinyl).
- Not composited on the GPU: no resolve goes to a 1024-pitch surface (`NFSMW_LOG_RESOLVES_PITCH=1024`) while the cars load; so the skin's pieces are textures loaded from
  the car's files (the paint and vinyl chosen pick which).
- Every resource key `sub_824618C0` gets (once each, with the guest call chain from the stack's back chain: 0(r1), the saved LR 8 bytes below each frame): about 850 a run, 840 in
  three runs alike; the 7 to 13 that differ all come through one path, 8245FF28 < 8243DAE8 < 8221AA18 < 8221B09C < 82217290 < 823B9BA8 (the streaming side), the common ones
  through the start-up path (8221AF60 < 823B023C ...). Whether those keys are the car's parts or the world streamed around it, the keys alone don't say.
- **Found: the paint and the vinyl are one 1024x1024 texture per car, its skin.** Zeroing the 1 MB of the higher-addressed 1024x1024 texture the car shader 48553c3b samples
  (through the CPU's view, so the texture cache loads it again) turned the rival's RX-8 solid black, paint and vinyl gone, and left the player's IS300 as it was. It isn't drawn
  by the GPU (no resolve goes to it), so the game puts it together on the CPU or loads it so.
- Next, if picked up, two ways:
  1. the owner's own skin over the network: the owner's game reads its car's skin (the same layout for the same model on every machine), sends it once per race (1 MB, in
     reliable chunks), and every other game writes it into the opponent's skin; a car's skin is found by matching the car shader's draws to the car by the world position in its
     vertex shader constants (c1/c3 carry it) against the bodies the multiplayer layer knows;
  2. the same paint everywhere without sending it: a write watch on the skin (its address once the car's textures are allocated) names the function that composites it, and
     its inputs (the paint and vinyl chosen) can be fixed per player. (The front end's 408-byte customisation record, +1124 of *0x82A2C684, is null in a Quick Race: it is a career car's, so it says nothing about an opponent.)
- A gate test before building it (an RX-8 player with an mp_bot RX-8 beside it): copying the player's whole skin (4 MB: the texture is 8_8_8_8, 4 bytes a texel, so the 1 MB
  of the first try left half the car in the old colour) over the rival's made the rival the player's stock red, still red 30 s later in 3 of 4 runs (once the game put its own
  back), so it has to be written again when that happens. No CPU write to it was caught by a write watch, so way 2 has nothing to hook.

### The skin over the network (protocol 11, 2026-10-26)
Way 1, built:
- The renderer (`gpu::renderer::CarSkins`) records, for the car body shaders' draws (ps 48553c3b, 781d5cda), every 1024x1024 8_8_8_8 texture they sample, with where that draw
  puts the car in view space (the w of the vertex shader's c5..c7: x right, y up, z ahead) and the frame. Only draws into the widest viewport these shaders have used count:
  the rear-view mirror draws the cars behind with its own view, and its places matched the wrong cars (a game sent another car's skin as its own before this).
- The multiplayer layer (`mp_adapter.cpp`, twice a second) puts each car's body (tracker axes: body z, -x, y) through the player camera's view matrix (eView 1 at
  0x82C3F590 + 384, the matrix at +0x50: rows, the translation in the last) and takes the skin drawn within 3 m of it in x and z in the last 30 frames. Positions agree within
  about a metre.
- Its own car's skin goes to the server once (again when its address changes, as in a new race): `Msg::Skin` on a channel of its own (kChanSkin, reliable), 4 MB, with the range
  coder on. The server keeps each player's and sends them on, also to whoever joins later.
- A skin from another player is written (through the CPU's view of the memory, so the texture cache loads it again) over the skin of the car that stands for that player, when
  the texture's format and size words match and three 256-byte samples differ, so again whenever the game has put its own back. Only over a car of the owner's model (another
  model's skin has another layout): in the open world a pool car made as that car (`game::WorldPoolCar`; a player whose car isn't in the pool drives another model's pool car,
  which keeps its own paint), in a race an opponent of the roster. Only the base level: the mips' layout isn't known, so a far car may keep its own colours.
- `NFSMW_MP_SKINS=0` turns it off.
- Checked with two Windows clients on one machine: in the open world an IS300 player saw the other's RX-8 in its owner's stock red, and with the cars swapped the BMW that stood
  for an IS300 (not in the pool) was left alone; in a Custom Race (RX-8 against Supra) each saw the other's car in its owner's colours (red RX-8, black Supra). Spoilers and
  body kits are parts, not the skin: see the next section.

### Parts: stock on every machine (2026-10-27)
- Wheels, spoiler, body kit, hood, roof, window tint are parts of the 408-byte customisation record a vehicle is built from (+0: a part index for each of 139 slots). The
  opponent creation (sub_82333680) fills it from a preset ride if the script names one (sub_822CDBC0), else from a RideInfo: SetStockParts (sub_824EE090), then
  SetRandomParts (sub_824EE4D0: random parts for one of three groups of slots, among them 23 BODY, 44 SPOILER, 66 FRONT_WHEEL, 131 WINDOW_TINT; the game's own xorshift at
  0x829043AC), then the ride into the record (sub_822CDAB8). The vehicle model (sub_824E5E68) gets stock parts when it has no record.
- Every player's own car in a multiplayer game is the car's stock one (the lobby car or the dev start's, made by name, without the front end's career record). So a car that
  stands for a player (a race opponent made as a player's car, an open-world pool car) gets only the stock parts: SetRandomParts calls SetStockParts alone then
  (`game::StockPartsFor`, car_select.cpp). Nothing goes over the network. `NFSMW_MP_STOCK_PARTS=0` turns it off.
- Checked with fe2.ps1 (RX-8 against Supra) and mp2s.ps1: before, the other player's RX-8 had a body kit, a big spoiler and tinted windows, the Supra a wing and other wheels;
  now both look as on their owner's screen. The players' cars went through SetRandomParts, not a preset ride (NFSMW_LOG_CARS=1 logs a preset ride).
- A player's career car (its own parts) would need its record sent (139 part indices, or the parts' name hashes if the part packs can load in another order) and written in
  sub_822CDAB8's place; not done, as multiplayer has no career car yet.
