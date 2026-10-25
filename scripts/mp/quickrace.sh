#!/bin/sh
# usage: quickrace.sh <name> [env...]: the game on a throw-away data copy, through the menus into a Quick Play race (single player); prints when the race should be on.
# Menu steps worked one by one on 2026-10-18 (START, START, RIGHT, RIGHT, A, A, A); check the screen with view.sh afterwards.
T=~/.cache/NFS-MostWanted/tools
name=$1; shift
pgrep -x SpeedBreaker >/dev/null && { echo "a game is already running"; exit 1; }
$T/launch_sp.sh $name NFSMW_STUB_CONTINUE=1 "$@" >/dev/null
timeout 150 sh -c "until [ \"\$(grep -c '^\[perf\]' /tmp/sp_$name.log)\" -ge 8 ]; do sleep 3; done"
c() { echo "$1" >> /tmp/ao/pad.cmd; sleep ${2:-4}; }
c "1 START" 14; c "1 START" 9; c "1 RIGHT" 4; c "1 RIGHT" 4; c "1 A" 6; c "1 A" 8; c "1 A" 45
pgrep -x SpeedBreaker >/dev/null && echo "race should be on" || echo "GAME EXITED"
