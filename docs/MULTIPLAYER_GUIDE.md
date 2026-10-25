# Multiplayer races: how to host and join

Race other people in the game's own races, over the internet or a LAN. Each player runs their own copy of the game and a small server relays everyone's car; the cars of the other players
appear as the race's opponents (they take over the AI cars). Career mode is not part of it. How it works inside: [MULTIPLAYER.md](MULTIPLAYER.md).

## What you need
- A copy of SpeedBreaker built from this fork (the multiplayer layer is in `runtime/net/mp_*` and `runtime/game/mp_adapter.cpp`), with your own game files, on every machine.
- One machine that runs the server and that every player can reach. The server is the `mp_server` program built next to the game (`ninja -C build/main mp_server`): it needs no game files.
  It listens on UDP port 27960 by default and can run anywhere, also on a machine that doesn't play.
- The players' machines need to reach the server's address. Over the internet: forward UDP 27960 on the router, or put all machines on one virtual network such as ZeroTier or Tailscale
  and use the server's address on it (this is what the tests used). On the server machine open the port in its firewall (Fedora: `sudo firewall-cmd --add-port=27960/udp`; add `--permanent`
  and `sudo firewall-cmd --reload` to keep it after a restart).

## Host
```
build/main/runtime/mp_server 27960
```
It prints a line when a player joins or leaves, and every chat line. Leave it running. What you type into it goes to every player as a chat line from "server"; `/players` lists who
is on (name, car, ready), `/help` the commands.

## Join
Open the settings (F1, or Back + Start on a pad), the **Online** page, section *Multiplayer races*:
- tick **Join multiplayer races**,
- **Server**: the server's address and port, for example `10.65.96.90:27960`,
- **Your name**,
- **Players to wait for at the start**: how many players the race has, yourself included (1 = don't wait).

The settings are saved (`multiplayer.txt` in the user data folder) and take effect from the next race. The status line under them shows the connection. For scripts the environment variables
`NFSMW_MP_SERVER`, `NFSMW_MP_NAME` and `NFSMW_MP_PLAYERS` override the page. The old *Online Play* switch (the earlier lockstep link) is ignored while multiplayer is on.

## Cars (the lobby)
Cars are set in the lobby, on the settings page (Online > *Multiplayer races*): **choose your car** from the list and tick **Ready for the next race**. Every game makes its opponents as
the cars the *ready* players chose, in the order of their ids, and the opponent that stands for a player is that player's car (same model). Then, in the game's own **car selection, choose
the same car** as in the lobby: your own car cannot be set from outside, so if the two differ the other players see the lobby car and a message tells you to choose it next time. The
choices have to be made before the race loads (a car swapped in late is not drawn). The players list shows who is ready, with their cars, and who is still in the menus. Not ready any
more after each race. The number of players the start waits for is the number of ready players (or the number in the settings if you are not ready).

## Chat
On the settings page (Online > *Multiplayer races*), under the players: type a line and press Enter or **Send**. Every player sees it there and, for 15 seconds, over the game (left, above
the minimap), also during a race. Use it to agree the race: the track, the direction, the laps.

## Race
1. Every player starts **the same race**: the same mode, **the same track and direction**, with at least as many opponents as there are other players (the other players take over the first
   opponents). Choose it in Quick Race > Custom Race; Quick Play picks a random track.
2. When your race has loaded, your game connects to the server and **holds the start** (the screen stays on the loading bar, with a message at the bottom) until all the players are there.
   Press **B** (or Back) to stop waiting and start without the others. The wait ends by itself after five minutes.
3. The **server** decides when to start: when everyone the race needs is held at the start it releases them together, with one start time and the slowest game's world clock, so the
   countdown and GO happen at the same time on every machine. Players start in the grid slots in the order they joined.
4. Race. When the race is over (quit to the menu, or finish), your game leaves the server; start the next race the same way.

## Limits
- Other players are listed by name in the standings (during and after the race: up to ten capital letters, digits, space, `-` and `_`), and their names show over their cars (up to 400 m away,
  also through walls). They show on the minimap like the game's own opponents (checked with a stand-in player on one machine). Your own car has to be chosen twice, in the lobby and in
  the car selection.
- Opponent cars look the same model on every machine, but paint, wheels, spoilers and vinyls are the game's random ones for that car, not the owner's own.
- Laps and finishing places are the game's own (computed on each machine from where the cars are); the times in the standings can differ between machines.
- The lobby sets cars and who is ready, not the race: the players agree the track and mode in the chat.
- Basic protection only: the server and every game drop car states that are not finite or are far outside what a car can do, limit one player to 120 states a second, clean player names, and
  disconnect a connection that does not say hello within five seconds. A modified client can still cheat with its own car (each game is trusted with it); there are no accounts or passwords.

## If it does not work
- *Cannot reach the server*: the status line says so. Check the address, that `mp_server` is running, the firewall port, and (on ZeroTier) that the service is up and shows the address you typed.
- *Waiting forever*: the other player's game is not at the start yet, or has a different `Players to wait for`, or an older build (every machine has to be on the same commit; the server and
  the games must have the same protocol version, which changes whenever the messages do: rebuild everything after a `git pull`).
- *"Is not in the same race as you"*: the message at the start says another player is held hundreds of metres from your start line, i.e. a different track or direction. Quit (pause menu) and choose the same race.

## Open world mode (new, 2026-10-23)
Instead of everyone starting the same race from the menus, the game starts straight in the open world and the server runs races from markers.
- **Server**: `build/main/runtime/mp_server 27960 scripts/mp/races.example.txt` (the second argument is the races file: `race <name> laps=N radius=M`, then `point x y z` lines, the first is the start marker; the
  example is a lap of the Ironwood circuit recorded from an AI driver; `scripts/mp/races.test.txt` is a short loop at the world's spawn point for testing).
- **Game**: in Settings > Online > Multiplayer races tick **Open world mode**, choose **your car**, and restart the game. It boots into the open world with your car (no menus), connects to the server and
  shows the race markers (a beam and a ring with the distance) and a panel.
- **A race**: drive to a start marker and **stop in it** for a second and a half. The race opens for 15 s: other players join the same way. Then a 5 s countdown (3.. 2.. 1.. GO!), checkpoints in order (the next
  two are marked), laps, then the places and times. All of this is decided by the server from the positions the games report. Players who are not in the race drive in the same world and can get in the way.
- **Limits**: at most six other cars in the world at once. A player who joins after your world has loaded is drawn with a car of the pool (their own car if it is one of the cars the players on the server had when your world loaded, else another model).
  The game's own HUD for the race and a different car after the world has started are not there yet. Markers and names are placed with the game's own camera projection (Field of View
  and the screen's shape included).
