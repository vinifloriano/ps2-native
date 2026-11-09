#!/bin/bash
# Packs a game that scripts/port.sh built (build/native-<name>/ps2run) into a macOS app: double-click to play, no Terminal.
#
#   bash scripts/mac_app.sh <name> [options]
#
#   --title TEXT     the app's name in Finder and the menu bar (default: <name>)
#   --out DIR        where the .app goes (default: /Applications)
#   --icon FILE      a square PNG for the app icon (optional)
#   --disc FILE      the disc image (default: games/<name>/disc.iso, the one port.sh used)
#   --link-disc      point at the disc image instead of putting a copy in the app (a copy is an APFS clone when it can be:
#                    instant and with no extra space on the same volume; otherwise a full copy)
#   --keys FILE      a keys.ini for this game (docs/apple/gh3-keys.ini for Guitar Hero III): the player's folder starts with it,
#                    and an existing keys.ini there is kept as keys.ini.bak and replaced
#
# The app holds the runner, the SDL libraries it needs from Homebrew (so it keeps working after `brew upgrade`) and the disc.
# What changes while playing (memory card, quick saves, logs, vector programs recorded for the next build) goes to
# ~/Library/Application Support/ps2-native/<name>, never into the app. The app is signed ad hoc for this Mac only; it holds a
# translation of the game's code and the game's disc, so it is not for sharing (see README, "Your own disc").
# Works with the bash 3.2 that macOS ships.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

usage() { sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; }
[ $# -ge 1 ] || { usage; exit 2; }
case "$1" in -h|--help) usage; exit 0 ;; esac
name="$1"; shift
title="$name"; out="/Applications"; icon=""; disc=""; link_disc=0; keys=""
while [ $# -gt 0 ]; do
  case "$1" in
    --title) title="$2"; shift ;;
    --out) out="$2"; shift ;;
    --icon) icon="$2"; shift ;;
    --disc) disc="$2"; shift ;;
    --link-disc) link_disc=1 ;;
    --keys) keys="$2"; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac
  shift
done
[ "$(uname -s)" = Darwin ] || { echo "mac_app.sh makes macOS apps: run it on a Mac" >&2; exit 1; }
if ! [[ "$name" =~ ^[A-Za-z0-9._-]+$ ]] || [[ "$name" == .* ]]; then echo "game name '$name': letters, digits, . _ and - only" >&2; exit 2; fi
case "$title" in */*|.*|"") echo "title '$title': no / and not starting with a dot" >&2; exit 2 ;; esac

runner="$ROOT/build/native-$name/ps2run"
game="$ROOT/games/$name"
[ -x "$runner" ] || { echo "no $runner: build the game first (bash scripts/port.sh $name <disc> --native-only)" >&2; exit 1; }
[ -n "$disc" ] || disc="$game/disc.iso"
[ -e "$disc" ] || { echo "no disc image at $disc (--disc FILE)" >&2; exit 1; }
disc="$(cd "$(dirname "$disc")" && pwd)/$(basename "$disc")"
[ -L "$disc" ] && disc="$(python3 -I -c 'import os,sys; print(os.path.realpath(sys.argv[1]))' "$disc")"  # port.sh links the image it was given
[ -d "$out" ] || { echo "no folder $out" >&2; exit 1; }
[ -z "$keys" ] || [ -f "$keys" ] || { echo "no keys file $keys" >&2; exit 1; }

app="$out/$title.app"
# Built beside the destination, so the final move is a rename on the same volume (the disc's clone stays a clone).
work="$(mktemp -d "$out/.ps2-mac-app.XXXXXX")"
trap 'rm -rf "$work"' EXIT
bundle="$work/$title.app"
contents="$bundle/Contents"
mkdir -p "$contents/MacOS" "$contents/Frameworks" "$contents/Resources/game"

echo "== runner"
cp "$runner" "$contents/MacOS/ps2run"
chmod 755 "$contents/MacOS/ps2run"

# Libraries from outside the system (Homebrew's SDL2, which is sdl2-compat and loads SDL3 at run time from @loader_path) go into
# Frameworks, and the runner is pointed at them there.
for lib in $(otool -L "$runner" | awk 'NR > 1 { print $1 }' | grep -vE '^(/usr/lib/|/System/)' || true); do
  base="$(basename "$lib")"
  cp -L "$lib" "$contents/Frameworks/$base"
  chmod 644 "$contents/Frameworks/$base"
  install_name_tool -id "@rpath/$base" "$contents/Frameworks/$base" 2> /dev/null
  install_name_tool -change "$lib" "@executable_path/../Frameworks/$base" "$contents/MacOS/ps2run" 2> /dev/null
  echo "bundled $base"
  case "$base" in
    libSDL2*)
      # sdl2-compat finds SDL3 beside itself: copy the one Homebrew has
      sdl3=""
      ! command -v brew > /dev/null || sdl3="$(brew --prefix sdl3 2> /dev/null)/lib/libSDL3.0.dylib"
      for candidate in "$sdl3" /opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib /usr/local/opt/sdl3/lib/libSDL3.0.dylib; do
        if [ -f "$candidate" ]; then
          cp -L "$candidate" "$contents/Frameworks/libSDL3.dylib"
          chmod 644 "$contents/Frameworks/libSDL3.dylib"
          install_name_tool -id "@rpath/libSDL3.dylib" "$contents/Frameworks/libSDL3.dylib" 2> /dev/null
          echo "bundled libSDL3.dylib (for sdl2-compat)"
          break
        fi
      done ;;
  esac
done

echo "== disc"
if [ "$link_disc" = 1 ]; then
  ln -s "$disc" "$contents/Resources/disc.iso"
  echo "linked $disc"
elif cp -c "$disc" "$contents/Resources/disc.iso" 2> /dev/null; then
  echo "cloned $(basename "$disc") (APFS clone, no extra space)"
else
  cp "$disc" "$contents/Resources/disc.iso"
  echo "copied $(basename "$disc")"
fi

# What the game folder of port.sh starts the player's folder with: the entry points the exploring runs found (the live translator
# adds to them while playing).
[ ! -f "$game/entries.txt" ] || cp "$game/entries.txt" "$contents/Resources/game/entries.txt"
if [ -n "$keys" ]; then
  cp "$keys" "$contents/Resources/game/keys.ini"
  data="$HOME/Library/Application Support/ps2-native/$name"
  if [ -f "$data/keys.ini" ] && ! cmp -s "$keys" "$data/keys.ini"; then
    mv "$data/keys.ini" "$data/keys.ini.bak"
    echo "kept the old keys as $data/keys.ini.bak"
  fi
fi

echo "== launcher"
cat > "$contents/MacOS/launch" << EOF
#!/bin/bash
# Starts the game in a window. The player's folder keeps the memory card, quick saves, keys.ini, reports and the vector
# programs recorded for the next build; log.txt is the last run's output.
here="\$(cd "\$(dirname "\$0")" && pwd)"
res="\$here/../Resources"
data="\$HOME/Library/Application Support/ps2-native/$name"
mkdir -p "\$data/vu1" "\$data/vu0"
[ -f "\$data/entries.txt" ] || [ ! -f "\$res/game/entries.txt" ] || cp "\$res/game/entries.txt" "\$data/entries.txt"
[ -f "\$data/keys.ini" ] || [ ! -f "\$res/game/keys.ini" ] || cp "\$res/game/keys.ini" "\$data/keys.ini"
cd "\$data"
exec "\$here/ps2run" --disc "\$res/disc.iso" --window --game-dir "\$data" --memcard "\$data/memcard.ps2" "\$@" > "\$data/log.txt" 2>&1
EOF
chmod 755 "$contents/MacOS/launch"

icon_key=""
if [ -n "$icon" ]; then
  echo "== icon"
  set_dir="$work/AppIcon.iconset"
  mkdir -p "$set_dir"
  for size in 16 32 128 256 512; do
    sips -z $size $size "$icon" --out "$set_dir/icon_${size}x${size}.png" > /dev/null
    sips -z $((size * 2)) $((size * 2)) "$icon" --out "$set_dir/icon_${size}x${size}@2x.png" > /dev/null
  done
  iconutil -c icns "$set_dir" -o "$contents/Resources/AppIcon.icns"
  icon_key="  <key>CFBundleIconFile</key><string>AppIcon</string>"
fi

xml_title="$(printf '%s' "$title" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g')"
cat > "$contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$xml_title</string>
  <key>CFBundleDisplayName</key><string>$xml_title</string>
  <key>CFBundleIdentifier</key><string>local.ps2-native.$name</string>
  <key>CFBundleExecutable</key><string>launch</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$(git -C "$ROOT" rev-parse --short HEAD 2> /dev/null || echo 0)</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>LSArchitecturePriority</key><array><string>arm64</string></array>
  <key>LSApplicationCategoryType</key><string>public.app-category.games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>GCSupportsControllerUserInteraction</key><true/>
$icon_key
</dict>
</plist>
EOF

echo "== signing (ad hoc, this Mac only)"
for f in "$contents"/Frameworks/*.dylib; do [ -e "$f" ] && codesign --force --sign - "$f" > /dev/null 2>&1; done
codesign --force --sign - "$contents/MacOS/ps2run" > /dev/null 2>&1
codesign --force --sign - "$bundle" > /dev/null 2>&1 || echo "note: codesign of the bundle failed; the app still runs on this Mac"

if [ -e "$app" ]; then
  echo "replacing $app"
  rm -rf "$app"
fi
mv "$bundle" "$app"
echo "== $app"
echo "Open it from Finder or with: open \"$app\""
echo "Its log and memory card: ~/Library/Application Support/ps2-native/$name"
