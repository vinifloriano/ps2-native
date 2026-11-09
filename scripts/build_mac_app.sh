#!/bin/bash
# Turns a PS2 disc you own into a Mac app, in one command:
#
#   bash scripts/build_mac_app.sh <disc.iso> [options]
#
#   --title TEXT   the app's name (default: from the game's profile, or the disc's volume name)
#   --icon FILE    a square PNG for the app icon (default: a picture the game showed while it was being built)
#   --out DIR      where the app goes (default: /Applications)
#   --name NAME    the build folder's name under games/ and build/ (default: from the profile, or the boot file)
#   --4x3          leave out a profile's widescreen patch
#   --quick        skip the exploring runs: for a game built before (after updating this repository, say)
#
# It reads the disc, picks the game's profile when there is one (docs/apple/profiles/<boot file>.conf: keys, patches,
# how long to explore), translates the game's code with scripts/port.sh, then packs the app with scripts/mac_app.sh.
# The full output goes to games/<name>/build.log; the screen shows the steps. Works with the bash 3.2 that macOS ships.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

bold=$'\033[1m'; dim=$'\033[2m'; green=$'\033[32m'; red=$'\033[31m'; plain=$'\033[0m'
[ -t 1 ] || { bold=; dim=; green=; red=; plain=; }
step() { printf '\n%s==> %s%s\n' "$bold" "$*" "$plain"; }
note() { printf '    %s\n' "$*"; }
fail() { printf '\n%s✗ %s%s\n' "$red" "$*" "$plain" >&2; exit 1; }

usage() { sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; }
[ $# -ge 1 ] || { usage; exit 2; }
case "$1" in -h|--help) usage; exit 0 ;; esac
disc="$1"; shift
title=""; icon=""; out="/Applications"; name=""; four_three=0; quick=0
while [ $# -gt 0 ]; do
  case "$1" in
    --title) title="$2"; shift ;;
    --icon) icon="$2"; shift ;;
    --out) out="$2"; shift ;;
    --name) name="$2"; shift ;;
    --4x3) four_three=1 ;;
    --quick) quick=1 ;;
    -h|--help) usage; exit 0 ;;
    *) fail "unknown option $1 (see --help)" ;;
  esac
  shift
done

step "1/5  Checking the tools"
[ "$(uname -s)" = Darwin ] || fail "this script makes macOS apps: run it on a Mac"
[ "$(uname -m)" = arm64 ] || note "this Mac is not Apple silicon: the build works, but it is made and tested for M-series Macs"
missing=""
for tool in cmake ninja pkg-config ffmpeg python3; do command -v "$tool" > /dev/null || missing="$missing $tool"; done
command -v sdl2-config > /dev/null || [ -d /opt/homebrew/opt/sdl2 ] || [ -d /usr/local/opt/sdl2 ] || missing="$missing sdl2"
bash4=""
for candidate in /opt/homebrew/bin/bash /usr/local/bin/bash "$(command -v bash)"; do
  if [ -x "$candidate" ] && [ "$("$candidate" -c 'echo ${BASH_VERSINFO[0]}')" -ge 4 ] 2> /dev/null; then bash4="$candidate"; break; fi
done
[ -n "$bash4" ] || missing="$missing bash"
if [ -n "$missing" ]; then
  note "missing:$missing"
  fail "install them with Homebrew (https://brew.sh): brew install cmake ninja pkg-config sdl2 ffmpeg ccache bash"
fi
note "${green}all there${plain} (Homebrew bash: $bash4)"

step "2/5  Reading the disc"
[ -f "$disc" ] || fail "no disc image at $disc"
info="$(python3 -I "$ROOT/scripts/disc.py" info "$disc" 2> /dev/null || true)"
boot="$(printf '%s\n' "$info" | tr '\r' '\n' | awk -f "$ROOT/scripts/boot2.awk" | sed 's/^cdrom0:\\//; s/;1$//' || true)"
volume="$(printf '%s\n' "$info" | sed -n 's/^layer 0: volume \(.*\), base sector.*/\1/p' | head -n 1)"
if [ -z "$boot" ]; then
  note "no SYSTEM.CNF read from it (an archive or a BIN/CUE dump is unpacked by the next step)"
else
  note "volume ${bold}${volume:-?}${plain}, boots ${bold}$boot${plain}"
fi

# The profile: key = value lines (docs/apple/profiles/README.md)
profile="$ROOT/docs/apple/profiles/$boot.conf"
setting() { [ -f "$profile" ] && awk -F'=' -v key="$1" '$1 ~ "^[ \t]*" key "[ \t]*$" { sub(/^[^=]*=[ \t]*/, ""); sub(/[ \t]+$/, ""); print; exit }' "$profile" || true; }
if [ -n "$boot" ] && [ -f "$profile" ]; then
  note "profile: ${bold}$(setting title)${plain} (docs/apple/profiles/$boot.conf)"
else
  profile=""
  note "no profile for this game: building with the defaults (it may need more exploring; see docs/apple/profiles/README.md)"
fi
[ -n "$title" ] || title="$(setting title)"
[ -n "$title" ] || title="${volume:-$(basename "${disc%.*}")}"
[ -n "$name" ] || name="$(setting name)"
if [ -z "$name" ]; then
  base="${boot:-$(basename "${disc%.*}")}"
  name="$(printf '%s' "$base" | tr 'A-Z' 'a-z' | tr -cd 'a-z0-9_-')"
fi
[ -n "$name" ] || fail "cannot make a build name from the disc: give one with --name"
data="$(setting data)"; data="${data:-$name}"
keys="$(setting keys)"
pnach="$(setting pnach)"; section="$(setting pnach_section)"; wide="$(setting widescreen)"
explore="$(setting port_options)"
entries="$(setting entries)"
[ "$four_three" = 0 ] || { pnach=""; section=""; wide=""; }
note "app ${bold}$title${plain}, build folder games/$name, saves in ~/Library/Application Support/ps2-native/$data"

if [ "$quick" = 1 ]; then step "3/5  Translating the game's code to arm64 (no exploring: a minute or two)"
else step "3/5  Translating the game's code to arm64 (the first time takes 5 to 20 minutes)"; fi
mkdir -p "$ROOT/games/$name"
log="$ROOT/games/$name/build.log"
if [ -n "$entries" ] && [ -f "$ROOT/$entries" ]; then
  # entry points the profile knows (code only a table of pointers reaches): added once, the exploring adds the rest
  touch "$ROOT/games/$name/entries.txt"
  added=0
  while IFS= read -r line; do
    case "$line" in ''|'#'*) continue ;; esac
    grep -qxF "$line" "$ROOT/games/$name/entries.txt" || { echo "$line" >> "$ROOT/games/$name/entries.txt"; added=$((added + 1)); }
  done < "$ROOT/$entries"
  [ "$added" = 0 ] || note "$added entry points from the profile"
fi
args=(--native-only)
[ -z "$pnach" ] || args+=(--pnach "$ROOT/$pnach")
[ -z "$section" ] || args+=(--pnach-section "$section")
# shellcheck disable=SC2206
[ -z "$explore" ] || [ "$quick" = 1 ] || args+=($explore)
[ "$quick" = 0 ] || args+=(--no-explore)
note "log: games/$name/build.log"
started=$(date +%s)
set +e
(cd "$ROOT" && "$bash4" scripts/port.sh "$name" "$disc" "${args[@]}") 2>&1 | tee "$log" | while IFS= read -r line; do
  case "$line" in
    "== [1/6]"*) note "reading the disc" ;;
    "== [2/6]"*) note "the program" ;;
    "== [3/6]"*) note "machine code to C++" ;;
    "== [4/6]"*) [ "$quick" = 1 ] || note "playing it for a while to find code the first pass missed" ;;
    "== [5/6]"*) note "vector unit programs" ;;
    "== [6/6]"*) note "compiling" ;;
    round\ *) note "  ${dim}$line${plain}" ;;
    *applied*) note "patch: ${line#applied }" ;;
    *FAILED*|*error:*) note "${red}$line${plain}" ;;
  esac
done
status=${PIPESTATUS[0]}
set -e
[ "$status" = 0 ] || { tail -n 15 "$log" | sed 's/^/    /'; fail "the translation stopped (exit $status): see games/$name/build.log"; }
note "${green}done${plain} in $(( ($(date +%s) - started) / 60 )) min"

step "4/5  Picking an icon"
if [ -z "$icon" ]; then
  shots="$ROOT/games/$name/shots"
  pick="$(python3 -I - "$shots" << 'EOF'
import glob, os, sys
best, score = None, -1
files = sorted(glob.glob(os.path.join(sys.argv[1], 'frame_*.ppm')))
for path in files[len(files) // 3:]:          # past the logos and films at the start
    data = open(path, 'rb').read().split(b'\n', 3)
    if len(data) < 4:
        continue
    px = data[3][::31]
    mean = sum(px) / max(len(px), 1)
    spread = sum((b - mean) ** 2 for b in px) / max(len(px), 1)
    if mean > 25 and spread > score:         # a lit, detailed picture
        best, score = path, spread
print(best or '')
EOF
)"
  if [ -n "$pick" ]; then
    icon="$ROOT/games/$name/icon.png"
    ffmpeg -loglevel error -y -i "$pick" -vf "crop='min(iw,ih)':'min(iw,ih)',scale=512:512" "$icon"
    note "from $(basename "$pick") (give your own with --icon)"
  else
    note "no picture to use: the app keeps the default icon"
  fi
else
  note "$icon"
fi

step "5/5  Packing the app"
pack=(--title "$title" --out "$out" --data "$data")
[ -z "$icon" ] || pack+=(--icon "$icon")
[ -z "$keys" ] || [ -f "$ROOT/games/$name/keys-installed" ] || { pack+=(--keys "$ROOT/$keys"); }
{ [ "$wide" = yes ] && [ -n "$pnach" ]; } && pack+=(--widescreen)
(cd "$ROOT" && bash scripts/mac_app.sh "$name" "${pack[@]}") >> "$log" 2>&1 || { tail -n 10 "$log" | sed 's/^/    /'; fail "packing stopped: see games/$name/build.log"; }
[ -z "$keys" ] || touch "$ROOT/games/$name/keys-installed"   # the player's keys.ini is not replaced again on a rebuild

printf '\n%s✓ %s is ready%s\n' "$green$bold" "$title" "$plain"
note "open it from Finder (Applications), or: open \"$out/$title.app\""
note "F1 shows the keys and the menu · F5 / F8 quick save and load · F11 full screen · F2 picture style"
note "saves, keys.ini and logs: ~/Library/Application Support/ps2-native/$data"
