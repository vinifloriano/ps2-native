# Game profiles

`scripts/build_mac_app.sh` reads the disc's `SYSTEM.CNF`, takes its boot file (`SLUS_216.72`) and uses
`docs/apple/profiles/<boot file>.conf` when there is one. A profile is a few `key = value` lines; every key is optional:

| Key | What it does |
|---|---|
| `title` | the app's name in Finder |
| `name` | the build folders, `games/<name>` and `build/native-<name>` |
| `data` | the player's folder, `~/Library/Application Support/ps2-native/<data>` (saves, keys.ini, logs) |
| `keys` | a keys.ini the player's folder starts with (the first build only: later builds keep the player's changes) |
| `pnach`, `pnach_section` | a patch file and the section of it to apply (a code patch is translated with the game) |
| `widescreen` | `yes`: the app starts with a 16:9 picture (for a widescreen patch); `--4x3` on the command line leaves both out |
| `port_options` | more options for `scripts/port.sh`, such as `--rounds 20 --frames 15000` for a game that needs longer exploring |
| `entries` | a file of entry points to add before translating (code only tables of pointers reach) |

A game without a profile is built with the defaults: exploring runs of 3600 frames (one minute), at most 8 of them. When the
build log (`games/<name>/build.log`) ends its rounds with "missing code", the game needs more: try
`port_options = --rounds 20 --frames 15000` in a profile of its own, and see [docs/apple/gtasa.md](../gtasa.md) for what a stubborn
game took.

Profiles hold names, addresses and settings, never anything from the game itself.

| Boot file | Game | Notes |
|---|---|---|
| `SLUS_216.72` | Guitar Hero III: Legends of Rock | 16:9 patch, keyboard as a guitar: [the game's page](../../games/guitar-hero-3.md) |
| `SLUS_786.45` | Grand Theft Auto: San Andreas, "GTA IV" mod | long exploring, 121 constructors: [the game's page](../../games/gta-san-andreas.md) |
