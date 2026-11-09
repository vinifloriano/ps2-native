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

### Fixed: the async-file stall was a SIF timing race (2026-11-08)

How Neversoft's file layer works, from the GH3 ELF and `FILEIO.IRX`:
- The EE sends 0x70-byte requests with user SIF command 1, carrying the request id in the header `opt` word. The send function is `0x120a40`.
- The send function waits for `sceSifDmaStat` to say done (`0x120bc0`), then returns `slot->id`.
- An async open (`0x120180`) stores `0x80000000 | id` as the file's fd placeholder (`obj+0x48`). The completion (`0x11f5c0`, kind 1) replaces it with the real fd.
- On the IOP, FILEIO resolves a placeholder fd through its fd table (`+0x2c` = the open's id). A read whose fd matches nothing stays in its wait queue forever.

The race:
- The runtime handed EE command packets to the IOP handler **inside** `sceSifSetDma`, and let the IOP catch up 64 cycles later. The EE's own DMA counted as busy for `256 + 2·bytes` cycles.
- So FILEIO answered the open while the EE still spun on DmaStat.
- The EE's command-1 handler (`0x1207a0`) had already cleared the request slot (id → -1) when the send function read the id back.
- The open stored fd `0x80000000 | -1 = 0xFFFFFFFF`, overwriting the real fd 0 that the completion had just written.
- The read that followed carried fd -1, FILEIO parked it forever, and the EE spun in `0x11DAB0` with `pending = 1`.
- Found with `PS2_WATCH=0x58d480,0x10` (the file object's pending count and fd) and the end-of-run RAM dump (EE at 0, IOP at 0x2000000).

Fix (`runtime/src/iop/sif.cpp`):
- EE packets for **guest** IOP handlers (cid < 0x80000000) are copied when the transfer starts and handed over by a scheduler event at the end of that transfer. That matches the console, where the IOP's SIF1 interrupt comes when the DMA ends.
- System commands (RPC, bind, SREG), which the runtime answers itself, are unchanged. **Open:** they also arrive early, so check whether any game depends on that before changing them too.
- The pending packets are saved in quick-saves (state version 10, 4–9 still load).
- New test `tests/sif_user_cmd_timing.cpp` (`ninja ps2sifusercmdtest`, run in CI).
- ps2autotests 54/55 and the disc-free unit tests still pass.

Result: GH3 now loads through the Brazucas splash with its progress bar, finishes loading and starts the Activision logo film (`MOVIES\ATVI.PSS`): 2,035 GS primitives where before there was 1.

### Fixed: the intro film stayed black (an SPU2 envelope read too early)

What the film player does:
- The player (`0x2748a0` loop under `StartPreLoadedMovie`) only starts a film once its audio is preloaded: `0x2e9140` needs MSTREAM's status for stream 0 **and** stream 1 to be "ready".
- The film's audio is one stereo SPU2-ADPCM track in the PSS (private stream `0xFF`, `SShd` type 0x10, 22,050 Hz, 2 channels, `0x2000` interleave). The EE demuxes it into two MSTREAM streams, left and right.
- The ready flag is bit 31 of word 4 in the stream's record in MSTREAM's reply (EE `0x2e1bb0` copies it to `0x3EA6C0[stream]`). MSTREAM (`0x9b88` in the module) only lists active slots.
- Until the film starts, the video decoder stops after two pictures (its queue is full), so the screen stays black.

The bug:
- Stream 1's slot was set up (its 8 KB preloaded to SPU2 RAM through IOP DMA channel 7) and keyed on.
- 0.19 ms later MSTREAM's end check (`0x6ae8`) read the voice's ENVX with `sceSdGetParam(… | 0x500)`, got 0, decided the voice had ended, and stopped the stream (`0x11024` → `0x33b4`).
- On the console the attack starts at the sample after KEY ON, so ENVX is already rising by then. The runtime renders the SPU2 in batches of 32 samples (0.67 ms) and only starts a keyed-on voice at the next batch, so ENVX still showed the old level, 0.
- Found with `PS2_WATCH_IOP=63ca8:63cac` on slot 1's active and type bytes, plus a dump of the saved return addresses on the IOP stack.

Fix (`runtime/src/iop/spu2.cpp`):
- KEY ON records its time. An ENVX read before the next batch returns the attack level for the samples that have passed (`attack_so_far`); the batch then starts the voice as before.
- Quick-save format unchanged.
- New test `tests/spu2_envx_keyon.cpp` (`ninja ps2spu2envxtest`, in CI).
- ps2autotests 54/55 and the unit tests still pass.

Result: the Activision, RedOctane and Neversoft logo films play (1.45 million GIF path 3 image uploads where there were 2), followed by the opening film (`INTRO.PSS`).

### Fixed: menus without text, black button prompts, holes in the banners (PATH3 texture order)

The symptom is the same with the Metal GPU renderer and the software renderer (`PS2_GS=cpu`), so it is not a Metal bug. The trace (`PS2_TRACE_DRAWS`, `PS2_TRACE_PATH3`, `PS2_TRACE_DMA`) shows why:
- Every frame the EE starts channel 1 (VIF1: the draws) and, **22 EE cycles later**, channel 2 (PATH3: every texture of the frame, CLUTs and 8-bit textures sent as PSMCT32).
- VIF1's list masks PATH3 at its top. Before each draw that needs the next texture it opens a window (`MSKPATH3 0` then `MSKPATH3 1`), with `FLUSH`, `DIRECT`, `FLUSHA`, `FLUSHE` around it.
- On the console VIF1 is slow, so channel 2's packets are waiting by the first window and each window lets exactly one through. The GIF stays with a PATH3 packet until it ends, so VIF1's next `DIRECT` waits for it.
- Here VIF1 ran its whole list the moment channel 1 started. Every window was empty, and all the textures reached the GS after the frame's draws.
- Textures that share GS memory (the font at `0x2BC0`, the button prompts) were then drawn with whatever was loaded there last, and failed the alpha test (`GREATER 0x20`) or came out black.

Fix (`runtime/src/vif.cpp`, `runtime/src/gs/gif.cpp`, `runtime/src/dmac.cpp`):
- **VIF1 waits** at an empty window, and before a command that uses the GIF (`DIRECT`, `DIRECTHL`, `MSCAL*`, `MSCNT`, `FLUSHA`) while PATH3 is part way through a packet whose data has not come yet.
- **It goes on** when channel 2's data arrives, after 1,000 EE cycles without the vector thread, or before the vector thread's next message that shows the EE moved on (vblank, present, download, fence, reset).
- A window with nothing after it that uses the GIF (the end of the list) does not wait once VIF1's DMA has ended.
- **PATH3** with the mask down lets the packet in flight go and holds the rest of channel 2 as flowing. The next GIF traffic, VIF1 going idle, or the time channel 2 would have taken (2 EE cycles per quadword, a scheduler event) let it out.
- The dry run of VIF1 on the EE beside the vector thread does not wait. Nothing new goes into a state.
- New test `tests/vif1_path3_window.cpp` (`ninja ps2path3windowtest`, in CI).
- ps2autotests 54/55 (including `dma/vif/mskpath3`) and the 11 unit tests pass.

Result: the caution, memory card and save screens, the main menu (Career, Co-op, Quickplay, Multiplayer, Training, Options) and the "Press any button to rock" title render with their text, frames and button prompts. Checked with and without the vector thread.

### Fixed: choosing a difficulty did nothing (the request-id race again, from the SIF DMA's length)

- Quickplay → SELECT DIFFICULTY took up/down but not select. The open that follows (request `0x509`) was answered with fd 1, then the read (`0x50A`) went out with fd `0xFFFFFFFF` and FILEIO parked it.
- The EE's send loop polls `sceSifDmaStat` and calls into the stream library (`0x2ea460`) between polls.
- The runtime said every EE SIF DMA was busy for `256 + 2·bytes` cycles: 480 for a 112-byte request. That is long enough for the IOP to take the command and answer, and for the answer's handler to clear the slot, before the EE read its id back (watched with `PS2_WATCH=0x415188,0x780` plus `--trace-syscalls`).
- On the console the DMAC fills SIF1's 32-quadword FIFO at bus speed, which is over before the `SifSetDma` syscall returns, so the first poll says done.
- Fix (`runtime/src/iop/sif.cpp`): what fits in the FIFO (512 bytes) is done at once; only what goes beyond it is paced, at two EE cycles a byte as before.
- `tests/sif_user_cmd_timing.cpp` now sends 2 KiB of extra data ahead of the packet to keep the "delivered at the end of the transfer" cases, and adds a FIFO-sized case.
- ps2autotests 54/55 and the unit tests pass, and the whole boot was replayed again.

### A song plays

- Quickplay → Easy → setlist (the Brazucas songs: *1406*, *A Sua Maneira*, *Admirável Chip Novo*, *Ana Júlia*) → *1406*.
- It goes through the loading screen and the venue intro ("As made famous by Mamonas Assassinas") into gameplay: the highway, the fret targets, the rock and star power meters, the band and the crowd, with the notes scrolling down.
- The song's sound reaches the SPU2: `PS2_DUMP_AUDIO=<file>` records it, and it gets louder once the song starts.
- **Frets with a normal controller** (pressed one at a time on a green note from a saved state): L2 green (hits the note), L1 red, R1 yellow, R2 blue, Cross orange; strum on the D-pad.
- `docs/apple/gh3-keys.ini` puts them on A S D F G (or 1-5) with the arrows (or Enter) to strum, Space for star power and Esc to pause. It also sets the FSR look at 2x resolution. `scripts/mac_app.sh --keys` installs it.

### FSR 1 picture look (`look = fsr`, F2)

- AMD's FidelityFX Super Resolution 1.0, ported to WGSL from `ffx_fsr1.h` (MIT; the notice is in `fsr1-license.txt`).
- **EASU** (`blit.wgsl`, `fs_easu`) enlarges the PS2 picture (at 1x to 3x, `scale`) to its size in the window, into an RGBA8 texture. **RCAS** (`fsr_rcas.wgsl`) sharpens that into the window. `PS2_FSR_SHARPNESS` sets the RCAS stops (0.25 by default).
- In full screen on a 4K display the 4:3 picture is drawn at 2880×2160. Checked in a 1920×1440 window: 59.9 fps at 2x resolution on the M5 Pro.
- FSR 4 is not possible here: it is a machine-learning upscaler that AMD ships only for its RDNA 4 GPUs through DirectX 12, with no Metal version. FSR 2 and 3 need motion vectors, which a PS2 renderer does not have.

### Open
- Each frame the EE sends user command 3, which no IOP module registers. It is dropped as on the console, so it is probably a debug channel, but worth confirming.
- System SIF commands (RPC) still reach the IOP at once (see the SIF fix above).
- Not yet checked: playing a whole song by hand (note timing against the audio, by ear), the end-of-song screen, film sound quality (the voices start up to one batch late), a USB guitar, whammy and tilt.
- The window title still says "ps2-native-recomp".

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
