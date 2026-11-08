# PS2 → Apple Silicon (Metal) static recompilation: plan

First target: **Guitar Hero III: Legends of Rock (PS2, NTSC-U, SLUS-216.72)**, using the disc `GH3 Brazucas 1.iso`.
Long-term goal: one generic PS2→arm64/Metal toolchain with a small per-game profile, the way `nxnative` works per title.

---

## 0. What we already know (from the disc, 2026-11-08)

| Fact | Value | Why it matters |
|---|---|---|
| Boot ELF | `SLUS_216.72`, 2.6 MB, MIPS R5900, stripped, entry `0x103d08` | Only one executable. No XFF/REL modules like SotC has, so we skip celosia's capture/link stages |
| Build stamp | `Aug 23 2007 18:48:12` | This looks like the stock retail ELF. "Brazucas" only changes data (songs in `DATAP.WAD`, `MUSIC/`) |
| ELF SHA-1 | `15992cec17deb6bf8e3d8fa4a2a5f1ef1b60f47e` | The profile key (like nxnative's NSO build ID). Compare it with a retail dump |
| `.text` | `0x10c000`, 0x21501c bytes (~545k instructions) | Small enough to recompile in one build (SotC was ~16k functions) |
| `.vutext` + 20 `.DVP.overlay.*` sections | VU microcode is in the ELF | **All VU1/VU0 microprograms can be compiled ahead of time from the ELF itself**, with no capture runs needed |
| `.gcc_except_table`, `.mdebug.eabi64` | C++ with exceptions (Neversoft engine) | Many virtual calls and indirect jumps. Runtime discovery runs are still needed |
| Strings: `*_CFunc`, `GuitarEvent_*`, `.qb`, `.pak` | Neversoft "Q" script engine. Script names map to C functions | The CFunc registration tables give us **free function names**, which helps both RE and decomp |
| High linker sections (`.script_heap 0x2900000` … `.end_128k 0x8000000`) | NOBITS, from the dev-kit memory map | Retail uses 32 MB. The runtime must not fault if code probes those addresses |
| SDK libs | `libpad2` (PsIIlibpad2 3000), `libipu`, `libmc2`, `libsd`, sifrpc, cdvd | The same set celosia already implements for SotC (`dbcman`, `libsd`, `mcserv`, MC2) |
| IOP modules (`IOP/`) | `IOPRP300.IMG`, `CDVDSTM`, `DBCMAN`, `DS2U_D`, `SIO2D`, `SIO2MAN`, `MC2_D`, `LIBSD`, `SDRDRV`, `FILEIO`, **`MSTREAM.IRX`** (Neversoft multitrack streamer, 140 KB), `SNSTKREL`, `SNDRV*` | `MSTREAM` drives song playback. Its timing determines whether the game is playable |
| Data | `DATAP.WAD/HED` (PAK archive), `SOUNDS/SFX.WAD`, `MUSIC/**/*.IMF+ISF` (song stems), `STREAMS/**/*.ISF`, `MOVIES/*.PSS` (MPEG-2 + ADPCM) | PSS needs the IPU. IMF/ISF are multitrack streams (guitar stem muted on a miss) |
| Peripherals | Guitar works over the normal pad port (libpad2 + `IsGuitarController`). No USB | GH3 needs no USB stack. GH World Tour (drums, mic) will |

---

## 1. Lessons from the three reference projects

### HenryKun55/celosia (SotC → macOS arm64 Metal + Vita)
- Built on **ran-j/PS2Recomp**: `ps2xAnalyzer` → TOML config → `ps2xRecomp` (C++ per function) → `ps2xRuntime` (kernel/syscall HLE, GS, VU1 AOT, SPU2, IOP HLE in `ps2xIOP`).
- Already runs on **Apple Silicon at 60 fps** with a Metal renderer. Uses `sse2neon` to get the x86 SIMD in the runtime onto ARM.
- Uses **two renderers**: an emulated GS for reference (F9 toggles between them) and a **native renderer fed by the game's own draw data**, which skips VIF/VU1/GS. That gives high resolution, effects and speed.
- **Functional decomp with difftest**: hand-written C++ is proven equal to the recompiled function on the same machine state (`SOTC_DIFFTEST`), hottest functions first. Progress is measured in call share and thread time.
- Tooling worth copying: scripted input (`PS2X_INPUT_SCRIPT`), autotest screenshots, frame recorder/replay, guest profiler, Ghidra export scripts, `celosia` CLI (`setup/run/vu1/recompile/build`).
- Per-game code sits in `game/` (overrides, stub bindings, hot lists), apart from the generic recompiler. Fixes go back upstream.

### exoteric33/ps2-native-recomp (generic, 45 games, Win/Linux)
- **Zero per-game code** as the ideal: fixes go into the tool. Validated by **ps2autotests (54/55)**, deterministic replay with periodic machine fingerprints, and quick-save state compares.
- Code model: 16 KiB memory blocks → one C++ function each, enterable at any jump target. Registers are 128-bit structs. RAM is a flat array, with a slow path for MMIO. Delay slots are folded in. Calls check the return address and unwind to a dispatcher on a mismatch (threads).
- **Exact FPU via integer arithmetic** (bit-exact, slow). One cycle per instruction, with slice checks at loop ends.
- Discovery: static control flow + heuristics, then **headless test runs (Start/Cross on a timer, up to 8 rounds)**. Any miss at runtime is **recompiled live into a dylib and cached**.
- VU: every uploaded microprogram is fingerprinted and AOT-compiled to SIMD C++ with stall timing resolved at translate time. Unknown programs fall back to the interpreter, then get compiled in the background.
- GS: **exact compute rasterizer on wgpu** (8×8 tiles, one thread per pixel, GS memory kept swizzled). Accurate, but slower than PCSX2. wgpu has a **Metal** backend.
- IOP: the **real IRX modules from the disc are interpreted**, with IOP kernel services HLE'd. SPU2 and IPU are rebuilt in C++.
- **Clang-only** (uses clang vector builtins). That makes it portable to arm64 in principle. macOS is "not checked since my changes".
- Not done: USB, network, exact cache/DMA timing.

### seedds/nxnative (Switch → iPhone/iPad/Mac)
- Different problem: Switch code is **already arm64**, so it runs natively. There is no CPU translation, only a **rewriter** for what Apple's kernel forbids, plus **HLE of the OS** (Horizon SVCs) and **graphics API translation** (NVN/Vulkan/GL → Metal via Ryujinx shader translator → SPIR-V → SPIRV-Cross → MSL).
- Habits worth copying:
  - **Per-game profile** directory (`dm/games/<id>/`) keyed by the build ID. Fixed addresses must never run on a build they were not found in.
  - **Root-cause rule**: no stubs, sleeps or clamps without a written cause. Each finding goes once into a game note or a subsystem reference, plus a **symptom index**.
  - **Shader/pipeline prewarm lists**, so there is no stutter on first use.
  - **Packaging**: Mac `.app` and unsigned **IPA** (no JIT on iOS, increased memory limit entitlement). Front ends built on top.
  - `regress.py` runs over several games for every change to shared code.

### Other prior art (x86 → ARM and Metal)
| Project | Lesson for us |
|---|---|
| **N64Recomp / Zelda64Recomp + RT64** | Static recomp + high-level renderer + mod API. RT64 gained a Metal backend, and arm64 macOS builds ship. Shows the "recomp + native renderer" model works |
| **XenonRecomp / UnleashedRecompiled** | PPC → C++. x86 SIMD in generated code was abstracted (**SIMDe**) so ARM builds work. **The emitted code must not hard-code SSE** |
| **PCSX2** | The reference for GS behaviour. Its **Metal GS backend (GSDeviceMTL)** is the best existing GS→Metal mapping (blending, destination alpha, texture cache, depth formats). It runs on Apple Silicon only under **Rosetta 2** (x86 JIT), so it is our *golden reference*, not our base |
| **AetherSX2 / NetherSX2 / ARMSX2** | PS2 on arm64 phones. Proves the FPU clamp modes, VU on NEON and EE timing hacks needed on ARM. Many Neversoft games are known to need specific clamp modes |
| **Play!** | Portable PS2 emulator with arm64 JIT and an IPU/MPEG-2 implementation (ps2-native-recomp already borrowed its VLC tables) |
| **Dolphin / RPCS3 / shadPS4 arm64 ports** | Ran into the **weak memory model**, **16 KB pages**, **W^X / MAP_JIT** and **FMA contraction** problems. Same issues here (see §3) |
| **Rosetta 2 / FEX / Box64** | Use them only for x86-only *tools* (PCSX2, old Windows GH modding tools via CrossOver). **Never ship the game as x86 under Rosetta**: no AVX before macOS 15, and Rosetta is being phased out |

---

## 2. Architecture decision

**Decision (2026-11-08):** this repository (`ps2-native`) is a fork of **ps2-native-recomp (MIT)** with its full history. celosia and nxnative are **references only**, for ideas and workflow, not code.

Why:
- ps2-native-recomp has the broadest **correctness net**: ps2autotests, replay fingerprints, 45 games, live recompile of missed code, and IRX interpretation. That matters for "other PS2 games later". It is clang-only and MIT, and on arm64 it already passes 54/55 ps2autotests (see [gh3-bakeoff.md](gh3-bakeoff.md)).
- celosia is **already arm64 + Metal and 60 fps**, and it implements the exact SDK libraries GH3 uses (libpad2, libmc2, libsd, IPU through ffmpeg). Its native renderer and difftest flow are what makes a port better than an emulator. However, celosia and its PS2Recomp fork are **GPL-3.0**. Copying their code would turn this MIT project into GPL, so we reimplement the ideas (native renderer at the game's draw calls, difftest, frame recorder) instead of the code.
- nxnative is MIT, but it targets Switch (arm64 code run natively). It is relevant for the profile/notes discipline, Metal packaging and iOS constraints, not as code.
- The Phase 1 bake-off still runs GH3 through the PS2Recomp pipeline as a comparison point (what it gets right that we don't). Its results go into [gh3-bakeoff.md](gh3-bakeoff.md).

Layout added to the upstream tree:
```
docs/apple/            # this plan, bake-off notes, later reference/<subsystem>.md + symptoms.md (nxnative-style)
scripts/harvest_cfuncs.py  # Neversoft Q-script CFunc name harvest (GH3, THPS, THUG, THAW)
runtime/src/apple/     # (planned) Metal GS backend, CoreAudio, GameController/HID, app shell
profiles/<SERIAL>/     # (planned) per-game profile: ELF sha1, entry points, notes. Keep it minimal: fixes go into the tool
games/ generated/ build/ isos/   # gitignored by upstream. Disc images, extracted files and generated C++ never get committed
```

---

## 3. x86 → ARM rules (apply from day one)

1. **No x86 intrinsics in generated code.** Emit clang `ext_vector_type` / `__builtin_*` or a small `simd.h` with NEON and SSE backends. Use SIMDe or sse2neon only as a stopgap in the runtime, and write hot paths (MMI `PADD*/PMADD*/QFSRV/PEXT*`, VU ops) against NEON directly.
2. **Turn off FMA contraction:** `-ffp-contract=off` for all generated FPU/VU code. Clang on arm64 fuses `a*b+c` by default. The PS2 FPU and VU `MADD` are **not fused**, and the extra precision changes results (physics, note timing, animation).
3. **PS2 float semantics.** There are no Inf/NaN (they saturate to ±MAX), denormals flush to zero, and rounding is toward zero. Choose per function:
   - *exact* (integer emulation, as in ps2-native-recomp) as the default during bring-up;
   - *fast*: ARM `FPCR.FZ=1`, `FPCR.RMode=RZ`, plus clamps where needed. This is far cheaper on arm64 than on x86 (no MXCSR stalls). Enable it per function once difftest proves equality.
4. **Weak memory ordering.** Every queue between the EE, VU, GS, IOP and audio threads uses `std::atomic` with acquire/release. Code that "worked on x86" through TSO will race on Apple Silicon. Test with TSan.
5. **16 KB pages.** Align the guest RAM and scratchpad mappings and any fastmem guard regions to 16 KB. Map the 32 MB EE RAM (plus mirrors) with `mmap`. Reserve a 4 GB window so any 32-bit guest address is a cheap host access with guard pages.
6. **No runtime JIT on iOS.** The live recompile → dylib path is macOS-only (and needs `MAP_JIT` / hardened-runtime entitlements). For iOS/iPadOS the build must reach **100 % offline coverage**, with an interpreter as the last fallback.
7. **ABI details:** `char` is signed on Apple arm64 (unsigned on arm64 Linux). Write `int8_t`/`uint8_t` explicitly. `__int128` is available for 128-bit GPRs. There is no `long double` extension (it is the same as `double`).
8. **Build scale:** use huge generated translation units with `-O1` or a lower-opt list for giant functions (ps2-native-recomp's `lowopt.cmake`) and ccache, split across `-j` jobs. Use `-ffunction-sections` and a hot-order file (celosia's Vita trick) for i-cache locality.
9. Ship **arm64 only**. Build x86_64 only for CI comparison against PCSX2 traces.

---

## 4. Phases

Each phase ends with a **gate**: a check that must pass before the next phase starts.

### Phase 0: Inventory and reference capture (≈1 week)
- [ ] Extract the disc into `disc/` and record hashes. Get a **retail SLUS-21672** ELF hash to confirm Brazucas uses an unmodified ELF.
- [ ] Install **PCSX2 (x86 under Rosetta)** as the golden reference. Record GS dumps, savestates (EE RAM), and a trace of boot → title → Quickplay song start.
- [ ] Ghidra project with the R5900 loader (*ghidra-emotionengine-reloaded*). Apply SDK signatures (PS2Recomp's `sce_symbol_database`).
- [ ] **Name harvest**: parse the Q-script CFunc registration tables (string → function pointer pairs) to name the engine functions. Neversoft's engine is shared with THPS3/4/THUG/THAW/GH:A, so the names carry over to those games.
- [ ] Document the data formats: PAK/HED, QB, IMF/ISF and PSS. Use existing GH3 PC/X360 community docs for QB and PAK.
- **Gate:** function list ≥ 90 % named or classified (SDK / engine / game). A PCSX2 reference trace exists.

### Phase 1: Toolchain on arm64 and recompiler bake-off (≈2 weeks)
- [ ] Build ps2-native-recomp natively on macOS arm64 (AppleClang, wgpu→Metal, SDL2). Fix any x86 assumptions (§3).
- [ ] Run **ps2autotests on arm64**. The target is the same 54/55 as x86. Every diff is an ARM translation bug: FPU, MMI or VU.
- [ ] Build PS2Recomp/celosia's pipeline and point it at `SLUS_216.72`.
- [ ] For both: how far does GH3 get (logos → menu → song)? Count missing-function hits.
- **Gate:** pick the base. ps2autotests on arm64 match x86.

### Phase 2: Boot to the title screen (≈2–4 weeks)
- [ ] Kernel HLE: threads, semaphores, alarms, and `LoadExecPS2` (must be harmless).
- [ ] IOP: start with **LLE interpretation of the disc IRX modules** (`IOPRP300.IMG` replacement, `MSTREAM`, `LIBSD`, `CDVDSTM`), with IOP kernel HLE.
- [ ] CDVD streaming from the ISO (`CDVDSTM` is used for music streams; latency matters).
- [ ] IPU / MPEG-2 for `ATVI.PSS`, `NS_LOGO.PSS` and `INTRO.PSS`. Use the core's IPU or HLE `libipu` → FFmpeg (celosia's approach).
- [ ] Display the GS through the emulated path (wgpu compute → Metal).
- **Gate:** logos, intro movie with sound, and the main menu, all deterministic under replay.

### Phase 3: Full code coverage (≈2 weeks, in parallel with Phase 4)
- [ ] Scripted discovery runs (`PS2X_INPUT_SCRIPT`-style) that cover every menu, Career (guitar and bass), Co-op, **Battle mode** (boss battles use different code), Practice, the store, options, calibration and every venue.
- [ ] Compile all VU microcode **from `.vutext` and the DVP overlays at build time**, then confirm at runtime that no unknown fingerprint appears.
- [ ] Jump tables, C++ vtables and QB CFunc pointers become explicit entry points in the profile.
- **Gate:** **zero runtime misses** across the full coverage script. This is required for iOS (§3.6).

### Phase 4: Audio, timing and input — what makes it playable (≈3–4 weeks)
This is the GH3-specific core. A rhythm game breaks with any latency or drift.
- [ ] SPU2 (2×24 voices, ADSR, reverb) is already in both bases. Verify it against PCSX2 recordings.
- [ ] Get `MSTREAM` multitrack right: the song stems (`*.IMF`) and the **guitar stem mute/unmute on a miss**, with stems kept in sync.
- [ ] **Clock policy:** the audio device clock is the master. Run the EE to the 59.94 Hz vblank schedule and never time-stretch audio during a song (ps2-native-recomp's stretch would wreck sync, so turn it off in the profile). Present via `CAMetalDisplayLink` at 60 Hz, including on 120 Hz ProMotion screens.
- [ ] **Latency budget:** under 1 frame from input to game and under 20 ms audio buffer (CoreAudio, low IO buffer size). Expose an AV-offset setting, and support the game's own lag calibration.
- [ ] **Guitar input**: libpad2 must report the guitar controller ID and mode so `IsGuitarController` / `GuitarGetAnalogueInfo` pass. Confirm the exact button bits in the decomp: fret buttons, strum on the D-pad, whammy analog, tilt. Host side: `GameController.framework` plus IOKit HID for **PS3/PS4/Xbox 360/Wii RB and GH guitars**, the keyboard (Clone-Hero-style F1–F5 + Enter), and a normal gamepad.
- [ ] Memory card: libmc2 (`MC2_D.IRX`) into a PCSX2-compatible `Mcd001.ps2`, written atomically.
- **Gate:** a **bot-played song**: parse the song's note chart (from the QB/PAK data) into a perfect input script, and require 100 % notes hit on several songs, both stock and Brazucas customs. This end-to-end test catches drift in input, timing and audio at once.

### Phase 5: Metal rendering (≈4–8 weeks, in stages)
- **5A, emulated GS on Metal (exact):** keep the core's compute rasterizer. Profile it on Apple GPUs; tile memory and 32 KB threadgroup memory suit the 8×8 tile design.
- **5B, hardware GS renderer in Metal:** port the concepts of PCSX2's `GSDeviceMTL`: a texture cache keyed by GS address/format, render-target reuse, blend and destination-alpha emulation in MSL, and depth formats. This brings resolution scaling and speed. Keep 5A as the F9 reference toggle (celosia style).
- **5C, native renderer (optional, celosia style):** hook the engine's draw submission (the highway, gems, characters, venue, HUD) and draw from engine data directly. This allows 16:9 without stretching, 4K, a **120 fps highway** (interpolate the highway scroll only, keep game logic at 60), and better AA.
- Prewarm the pipeline-state cache (nxnative style) so venues don't stutter on first load.
- **Gate:** pixel diffs against PCSX2 GS dumps for N reference frames per venue, under the threshold. Steady 60 fps on an M1 base model.

### Phase 6: Functional decompilation of the hot paths (ongoing)
- [ ] Profile it: note tracking/scoring, the highway mesh, the QB interpreter, skinning, and the DMA/VIF walk.
- [ ] Rewrite functions in C++ in `games/SLUS-21672/decomp/`, each **proven by difftest** (same input state, byte-identical output) before it replaces the recompiled version.
- [ ] Track progress celosia-style: % of functions, % of calls, % of thread time native.
- [ ] Natural first targets are the self-contained Neversoft subsystems: the QB VM, PAK/HED loading, memory allocators, math (VU0 macro mode → NEON).
- **Gate:** none, it is continuous. This is the path to an iOS build that runs at full speed and to clean mod hooks.

### Phase 7: Packaging and platforms (≈2 weeks)
- [ ] A `tools/gh3 setup --iso …` CLI: extract → analyze → recompile → VU AOT → build → `.app`. The user brings their own disc and nothing copyrighted is committed.
- [ ] macOS `.app` (arm64), in-app settings (F1 menu like celosia's), quick save and load.
- [ ] iPadOS/iOS: an unsigned IPA (nxnative flow), no JIT, increased memory limit, MFi/USB guitar support. Requires the Phase 3 gate.
- [ ] Mods: load custom songs (Brazucas-style PAK/QB content) from a folder without rebuilding the ISO.

### Phase 8: Generalize to other PS2 games
- [ ] Move all GH3-specific code into the profile. Shared code must pass `regress` on the game set (GH3 plus 4 others) on every change.
- [ ] **Same engine family first**: GH: Aerosmith, GH World Tour, GH5, Smash Hits, Metallica, Van Halen. Also **THPS3/4, THUG/THUG2, THAW** (Neversoft engine). The QB, PAK and MSTREAM work carries over directly.
- [ ] **Needs new hardware**: World Tour and later use **USB drums and mic**, so they need a USB/`usbd` HLE (missing in all three references). Rock Band PS2 likewise.
- [ ] Harmonix-era GH1/GH2/GH80s use a different engine and are separate profiles.
- [ ] The GS, SPU2 and IPU Metal runtime stays generic. Upstream fixes to the chosen core.

---

## 5. Test and debug infrastructure (build in Phase 1, use everywhere)
- **Deterministic replay:** record the pad and RNG seeds, and fingerprint EE RAM, VU and GS state every N frames. The first divergence points at the bug.
- **Difftest harness:** run a native function and the recompiled one from the same snapshot and compare memory and registers.
- **PCSX2 cross-check:** compare savestate RAM at matching frames. Diff GS dumps against our GS.
- **ps2autotests** in CI on arm64.
- **Bot-play song suite** (Phase 4 gate), run nightly.
- **Docs discipline** (nxnative): `docs/reference/<subsystem>.md` entries (Symptom / Cause / Fix / Found on) plus `docs/symptoms.md`, and a "no workaround without a written cause" rule.

## 6. Biggest risks
| Risk | Mitigation |
|---|---|
| Audio/video drift makes songs unplayable | Audio clock is the master, no time-stretch, bot-play gate |
| FPU differences on ARM (contraction, rounding) change note timing or physics | `-ffp-contract=off`, exact mode by default, fast mode only after difftest |
| Indirect calls (vtables, QB CFuncs) missed offline, which breaks iOS | CFunc table harvest + coverage scripts + zero-miss gate |
| GS emulation too slow on base M1 | Stage 5B hardware renderer, then 5C native renderer |
| The upstream core changes under us | Git subtree with small, upstreamable patches. Profile code stays separate |
| Legal | No game data, ELF or generated code from it in the repo. The user supplies the ISO |

## 7. First concrete steps
Done on 2026-11-08 (details in [gh3-bakeoff.md](gh3-bakeoff.md)):
1. ~~Toolchain on arm64~~: `brew install cmake ninja pkg-config sdl2 ffmpeg ccache bash`.
2. ~~ps2autotests on arm64~~: 54/55, same as x86.
3. ~~GH3 through `scripts/port.sh`~~: boots, IOP modules run, then a black screen in the Neversoft async-file wait (`0x11DAB0`).
4. ~~CFunc name harvest~~: `scripts/harvest_cfuncs.py`, 871 names.
5. ~~`-ffp-contract=off` on every build~~ (`scripts/cxx.sh`).

6. ~~Async-file stall~~: a SIF timing race. EE→IOP user command packets now arrive when their DMA is over ([gh3-bakeoff.md](gh3-bakeoff.md)). GH3 loads and reaches the intro film.

7. ~~Black intro film~~: MSTREAM stopped the film's second audio voice because ENVX read 0 right after KEY ON. ENVX now follows the attack from the KEY ON ([gh3-bakeoff.md](gh3-bakeoff.md)). The logo films and the opening film play.

Next:
1. Get from the opening film to the title screen and menus, then into a song.
2. Record PCSX2 (Rosetta) reference captures of boot, menu and the start of one song, to diff against the SIF traffic.
3. Feed the harvested names into the translator's symbol output and the debugger.
