# Phase 0/1 bake-off: GH3 (SLUS-216.72) on Apple Silicon

Date: 2026-11-08. Machine: Apple M5 Pro (15 cores, 24 GB), macOS 27.0.1, Apple clang 21, arm64 native (no Rosetta).

## Inputs

| | |
|---|---|
| Disc | `GH3 Brazucas 1.iso` (fan song pack). Extracted into `disc/` |
| Boot ELF | `SLUS_216.72`, SHA-1 `15992cec17deb6bf8e3d8fa4a2a5f1ef1b60f47e`, PCSX2 CRC `A46E2D44`, build stamp `Aug 23 2007 18:48:12` |
| Data changed by Brazucas | `DATAP.WAD/HED`, `DATAPD.HDP`, `DATAPF.HDP`, `MUSIC/` (the ELF looks like stock retail. Confirm by matching the CRC against PCSX2's GameIndex or a retail dump) |

## Toolchain on arm64

- Homebrew: `cmake ninja pkg-config sdl2 ffmpeg ccache bash` (ps2-native-recomp's scripts need **bash ≥ 4**, and macOS ships 3.2).
- **ps2autotests on arm64: 54 PASS, 1 DIFF, 0 HANG, 0 FAIL** in 53 s. This matches the upstream x86 result. The DIFF is `vu/lower/efu` (ERCPR garbage-input case), which upstream already lists as known. **The Phase 1 toolchain gate passes.**
- Finding (x86→ARM, plan §3.2): `scripts/cxx.sh` adds `-ffp-contract=off` **only together with `-mavx2`**, so arm64 builds get clang's default `-ffp-contract=on`. The VU helpers wrap the product in `norm()` before adding, so no fusion appears there today and the autotests pass. Still, nothing guarantees it. **Fixed in this fork:** `scripts/cxx.sh` now passes `-ffp-contract=off` on every build (worth sending upstream).

## Candidate A: exoteric33/ps2-native-recomp (f7cdb4e, the base of this fork)

`scripts/port.sh gh3 <iso> --native-only` took about 3 minutes end to end.

- Translate: **135 units** (16 KiB blocks). Explore round 1: frame limit reached, **0 new entry points**. VU programs recorded: 1.
- Renderer: wgpu on **Metal** (`Apple M5 Pro on Metal`, 9 shaders in 12–239 ms).
- How far it gets: **black screen** (every screenshot frame 600–3300 is all zero; 1 GS primitive drawn).
- What does work:
  - The EE kernel HLE boots, SIF init works, and the IOP reboot with `IOPRP300.IMG` works.
  - All 9 IOP modules load and run interpreted: `SIO2MAN SIO2D DBCMAN MC2_D DS2U_D CDVDSTM LIBSD FILEIO MSTREAM`.
  - Neversoft's own `FILEIO.IRX` ("FileIO driver version 0.1") registers SIF command 1. `MSTREAM` ("MultiStream V7.3") registers RPC server `0x12345`, and its hardware timer fires (IOP timer HLE works).
- **Where it stops:**
  - EE thread 1 spins in `0x11DAB0`: `while (obj->pending@0x40 > 0) { 0x11d988(); 0x11e560(); }`. This is the Neversoft async-file wait, called from `0x11EFE0`. Its stack: `0011EFE0 0011D630 0011D5BC 001BDA94 0015765C 001841B4 00157688 0030F140 0030B5BC 00182E1C …`.
  - Only **one** `sceCdRead` is ever issued: FILEIO reads 2 sectors at LSN 172740, which is inside `DATAP.WAD`, after `DATAPD.HDP` is read through EE fileio.
  - FILEIO answers with `isceSifSendCmd(1, …)` and the packet reaches the EE (0 packets queued).
  - Then the EE stops asking for reads, and the pending counter stays above zero.
- Open leads, in order:
  1. **`CDVDSTM` init calls `cdvdman #50` (`sceCdSC`), which is missing** and returns 0. `CDVDSTM` also imports #40, #46, #48, #49 (`_sceCdstm0Cb`/`_sceCdstm1Cb`) and #114, and those are not implemented either. If anything in the WAD path goes through `cdrom_stm:`, this is the cause.
  2. The extra-data transfer of FILEIO's reply (`sceSifSendCmd` args 4–6, whose dest and size are on the IOP stack) may land wrong. Dump the EE destination after the reply and compare it with PCSX2.
  3. The EE handler for SIF command 1 runs, but the next request is never queued. Find the function at `0x11e560` (async-file manager update) and see which state it waits for.
- Trace commands that reproduce this:
  `build/native-gh3/ps2run --disc games/gh3/disc.iso --game-dir games/gh3 --frames 900 --trace-syscalls --trace-threads --no-record`
  plus `--trace-io --trace-iop-hle`.

## Candidate B: HenryKun55/celosia → PS2Recomp (025e264, built locally for comparison only, GPL-3.0)

- `ps2_analyzer` (enabled; celosia's own CLI turns it off) on `SLUS_216.72` in 1 s: 94 SDK functions bound to runtime handlers (`scePad2*`, `sceSif*`, `sceCd*` and others), 332 SDK functions without handlers, 0 jump tables detected.
- `ps2_recomp` in 2 s: **~9,460 functions**, 114 MB of C++. Unresolved `JR/JALR` sites were promoted to fallback entries (largest: `sub_002CAA68` with 614 entries).
- celosia's `celosia` CLI is SotC-specific: the XFF capture/link stages and hard-coded stub addresses in `game/scus.toml`. For GH3 only the generic parts apply: analyzer → recomp → `ps2EntryRunner` built with `PS2X_RUNNER_DIRS`.
- Build: HEAD 025e264 does not compile on macOS. `native_renderer_gl.cpp` uses `quadPrograms` and `regionPrograms` outside their `#if defined(PS2X_NATIVE_VITA)` block. With a one-line local patch (declare them on every platform) `ps2EntryRunner` builds in 34 s (46 MB, unity build).
- Run (`SOTC_NATIVE=0`, emulated GS on its Metal GPU rasterizer; raylib window, miniaudio on Core Audio): **black screen**. All 12 screenshots over 60 s are the same empty frame.
- Why it gets less far than candidate A:
  - The runtime passes a host path as the guest's argv[0], so GH3 decides it runs from `host0:`. It never reboots the IOP with `IOPRP300.IMG` and asks for `host0:IOPModules/*.IRX`. PS2Recomp does not resolve those to the disc's `IOP/` folder (a symlink in the working directory is not enough).
  - The modules fall back to the built-in HLE providers (`SIO2MAN SIO2D DBCMAN LIBSD FILEIO`). `MC2_D`, `DS2U_D` and `CDVDSTM` get no provider, and `MSTREAM` is never reached.
  - The HLE `FILEIO` is Sony's protocol, not Neversoft's custom one, so the game's first file command (`sceSifSendCmd cid=0x1`, 0x70-byte packet) is never answered.
- Lesson for this fork (it already does this): give the guest `cdrom0:\<BOOT2>;1` as argv[0] and run the disc's own IRX modules. GH3's Neversoft modules (`FILEIO`, `MSTREAM`) have no SDK equivalent to HLE.

## Verdict

**Base = ps2-native-recomp** (this fork). It builds and passes the tests on arm64 unchanged, runs GH3's real IOP modules, and gets further into the boot. PS2Recomp/celosia is GPL-3.0 and stays a reference for ideas only: the native renderer at the game's draw calls, difftest and the frame recorder.

## Function names

- `scripts/harvest_cfuncs.py` reads the Q-script CFunc registration tables (`{name, fn}` pairs in `.data`): **26 tables, 873 entries, 871 named functions**, written to a local `cfuncs.tsv` (derived from the game, so never committed). Examples: `IsGuitarController` `0x2af460`, `GuitarGetAnalogueInfo` `0x2af8e8`, `GuitarInputLogicPerFrame` `0x2b8ed8`, `ResetWhammyPitchShift` `0x2565e0`, `StartPreLoadedStream` `0x256020`.
- Next: feed these into PS2Recomp's `functions.csv` and Ghidra, then add the vtables and the C++ RTTI/typeinfo strings.
