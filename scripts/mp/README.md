# Multiplayer reverse-engineering and test helpers

Run on a throw-away copy of the game's data, never on the real saves (`launch_sp.sh` copies `~/.local/share/speedbreaker` to `/tmp/ao/xdg`). They were written to live in
`~/.cache/NFS-MostWanted/tools` next to `image.bin` (a dump of the guest image 0x82000000-0x82CD0000 made while the game ran), and refer to that folder.

- `launch_sp.sh <name> [ENV=...]`, `quickrace.sh <name> [ENV=...]`, `view.sh <png> [divisor]`: start the game windowed on the copy, drive the menus to a Quick Play race with the pad command file
  (`NFSMW_INPUT_CMD`), grab a small screenshot of the window (Hyprland: `hyprctl`, `grim`).
- `gmem.py` (read the running game's guest memory through /proc), `findpos.py`, `findcar.py`, `findvel.py`, `localvel.py`, `bodies.py`, `walklist.py`, `ptrs.py`, `dumpobj.py`,
  `statdiff.py`: find the player's car and the game's state words, see docs/MULTIPLAYER.md.
- `disasm.py <addr> [bytes]` (and the older `dis.sh`): disassemble the dump one word at a time (needs `llvm-mc`); `dis.sh` loses its alignment after an undecodable VMX word, use `disasm.py`.
