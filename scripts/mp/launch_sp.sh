#!/bin/sh
# usage: launch_sp.sh <name> [env...]: the game on a throw-away copy of the data folder (windowed, 1x, shadow 800), command channel /tmp/ao/pad.cmd
name=$1; shift
G="$HOME/.local/share/Electronic Arts/Need for Speed Most Wanted"
S="$HOME/.cache/NFS-MostWanted/speedbreaker"
mkdir -p /tmp/ao
cp "$S/build/main/runtime/SpeedBreaker" "$G/SpeedBreaker"
rm -rf /tmp/ao/xdg; mkdir -p /tmp/ao/xdg; cp -a "$HOME/.local/share/speedbreaker" /tmp/ao/xdg/speedbreaker
sed -i 's/^fullscreen = .*/fullscreen = false/' /tmp/ao/xdg/speedbreaker/settings.toml
: > /tmp/ao/pad.cmd
cd "$G"
(env XDG_DATA_HOME=/tmp/ao/xdg NFSMW_INPUT_CMD=/tmp/ao/pad.cmd NFSMW_MOTION_BLUR=0 NFSMW_RESOLUTION=1x NFSMW_SHADOW_SIZE=800 NFSMW_PERF_OVERLAY=0 "$@" ./SpeedBreaker > /tmp/sp_$name.log 2>&1 & echo $! > /tmp/pad.pid)
echo started $name
