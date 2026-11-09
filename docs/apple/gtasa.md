# Grand Theft Auto: San Andreas (a "GTA IV" mod, SLUS-786.45)

The disc is San Andreas for the PS2, version 1.03 (`VER = 1.03`), with a Brazilian "GTA IV" mod: its own loading screens, opening
film and Portuguese text. The executable is `SLUS_786.45` (5.7 MB). Like the other games, no game data is in the repository.

```
bash scripts/port.sh gtaiv "<disc>.iso" --native-only --rounds 20 --frames 15000
```

## Where it stands

- From a cold boot, with no input, it plays:
  - the Rockstar logos
  - the mod's loading screens and opening cutscene
  - Carl in Jefferson with the HUD: clock, money, radar, the area name and the Portuguese hint to move him
- The in-game clock ran from 06:43 to 09:27 in a 15,000-frame headless run, which ended at the frame limit.
- Headless it runs about 350 frames a second on an M5 Pro.
- Not checked yet: driving, missions, saving, sound by ear, the window at full speed.
- The film decoder reports one bad code in the opening film (`[ipu] bad code in command 0x28010000`).

## What it took (all generic, see the commits)

1. **Constructors only a table names.** The executable is one read-write-execute block with no section names. Its 121 C++
   constructors (0x657E00 on) come after all other code, and the start-up code calls them through a table at 0x65B400. Each
   exploring run learned one of them. They are listed in `games/gtaiv/entries.txt` (a translator search to the end of the segment
   when there is no `.text` would find them; not done yet).
2. **Overlays at one address.** The game loads eight `.NM` modules (header `MWo3`: magic, number, address, size, .bss size, then
   its constructor table) all to 0x88D880:
   `INIT FILELOAD MC MPEG JPEG CUTSCENE CLOTHES CREDITS`.
   - **Stale module after a swap.** The runtime re-checked a module found in place with one word per call. After `MC.NM` was
     replaced by `MPEG.NM` that word could still match (the headers are alike), so `MC.NM`'s code ran and unwound `main` to
     address 0. A `FlushCache` of the instruction cache, which every load ends with, now drops the modules found in place, and
     the next call checks in full (`forget_window_instances`).
   - **Zero words checked.** The analysis takes the zeros after a module for code (NOPs), and the game later writes data there.
     Zero words are no longer check words.
   - **Eight windows instead of one.** The executable has one empty segment per overlay, all at 0x88D880. `code_windows` and
     `windows.py` moved each to the next 16 KiB, so eight "windows" each captured a slice of the current overlay. That was more
     images than the folder keeps, so captures kept evicting each other. Segments that start at the same address are now one
     window, and a window keeps 16 images (was 8; 32 in all as before).
   - **Left-overs of a bigger overlay.** A small overlay leaves the end of the bigger one before it in memory. The program calls
     the other overlays at fixed addresses, which seeded the analysis into those left-overs, and they differ from run to run. The
     translator now zeroes what lies before an `MWo3` header and ends the image at the overlay's size + .bss
     (`mask_overlay_leftovers`).
   - Test without game data: `tests/window_overlays.py`.
