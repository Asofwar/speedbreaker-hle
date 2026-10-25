# Multiplayer v2: first test between two machines

Status: **untested between two real machines** (one machine with a scripted second player works, docs/MULTIPLAYER.md, "Multiplayer v2 status").
This is not the old lockstep/split-screen link (docs/NETPLAY_QUICKSTART.md): each machine runs its own single-player race, sends its car 30 times a second to
a small server, and shows the other player's car as one of its opponents.

## What you need
- Both machines build the same source (the fork at this commit or later). On the second machine, apply the changes as in docs/NETPLAY_QUICKSTART.md §1, then
  `ninja -j2 -C build/main SpeedBreaker mp_server mp_bot` (the server and the test bot are small and need no game files).
- The same race on both: **the same track and the same direction**, because positions are world coordinates. Choose it by hand in Quick Race on each machine
  (not "Quick Play", which picks a random track). With one other human, set **2 opponents**: the first opponent becomes the other player's car, the second stays AI.
- UDP port 27960 open to the other machine on the machine that runs the server (Fedora: `sudo firewall-cmd --add-port=27960/udp`, temporary).

## Run
**Without environment variables:** open the settings (F1, or Back + Start on a pad), **Online** page, section *Multiplayer races*: tick *Join multiplayer races*, enter the server
(`address:port`), your name and how many players to wait for at the start. It is saved in `multiplayer.txt` in the user data folder and read again every couple of seconds, so it applies
to the next race. The page shows the connection status. The environment variables below still work and override the page (for scripts).

1. Server (either machine, or any host both reach): `build/main/runtime/mp_server 27960`. It prints a line when a player joins.
2. Machine A: `NFSMW_MP_SERVER=<server-ip>:27960 NFSMW_MP_NAME=a NFSMW_MP_PLAYERS=2 build/main/runtime/SpeedBreaker`
   Machine B: `NFSMW_MP_SERVER=<server-ip>:27960 NFSMW_MP_NAME=b NFSMW_MP_PLAYERS=2 build/main/runtime/SpeedBreaker`
   `NFSMW_MP_PLAYERS=2` is the **start barrier**: the race is held (the world stands still, on the loading bar) from the moment it appears until the other player's game is
   there and ready too, then both start together. Without it each game starts when it is ready (the cars start at different times). It gives up after 5 minutes.
3. Start the same race on both. The log (or terminal) shows `[mp] connecting to ...`, then every 3 s `net up, 1 remote, rtt N ms`, and
   `[mp] remote player 1 (b) drives body ...` when the other player's car is taken over.
4. What to look for: the other player's car appears at their position and drives as they drive; when a car hits the other, each machine sees its own car react.

## Known limits (see the status section)
laps and the finish are not shared; the other player's car looks like whichever opponent
car the game chose; the remote car is overwritten every step, so it cannot be pushed on your screen (its owner's machine decides how it reacts).
