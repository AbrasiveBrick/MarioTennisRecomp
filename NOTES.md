# Mario Tennis (USA) — N64Recomp port notes

ROM: `Mario Tennis (USA).z64`, game code NM8E, SHA-1 `999047f07cec931ffbdcc7b33b8502ef602807ee`,
big-endian, CIC-6102, libultra 2.0I. No existing decomp/recomp to build on (the only public
"decomp" has 14 C files and no symbols), so everything here is derived from the ROM.

## Boot and compression
- Entry 0x80300000 (ROM 0x1000, 0x350 bytes) zeroes RAM, then `func_80300080` LZ-decompresses
  ROM 0x1350 → 0x80031000 (0x2AE10 bytes) and jumps to 0x80031000 with a0 = 0x116C0.
- LZ format (Camelot): 4-byte header (`01` + 24-bit size), flag byte MSB-first, 0 = literal,
  1 = 2-byte ref (12-bit distance, 4-bit n; n=0 → len = next byte + 0x11, n=1 → 2, else n+1),
  distance 0 ends. Implemented in `tools/mt_lz.py`.
- ROM 0x116C0 is a table of 1275 (start, end) ROM offset pairs: the game's file system.
  `func_80035CC0` is the streaming DMA + decompress loader (`func_80035AB0` = decompressor).

## Code layout
| Section | ROM (real) | VRAM | Size | Notes |
|---|---|---|---|---|
| boot | 0x1000 | 0x80300000 | 0x350 | |
| main | LZ @0x1350 | 0x80031000 | 0x2AE10 | CPU text 0x80031000–0x8003DBF0 and 0x80040E60–0x80049A78; RSP ucode in between |
| seg0 | LZ @0x156C0 | 0x800C8000 | 0x1880 | |
| seg19 | LZ @0x958B70 | 0x802EC000 | 0x94F8 | |
| seg31 | LZ @0x963AE0 | 0x800CA000 | 0xA020 | |
| seg35 | LZ @0x96AA30 | 0x80500000 | 0x140C | expansion-pak range? check |
| seg71 | LZ @0x9CD770 | 0x800E8000 | 0x41DB4 | |
| seg86 | LZ @0xA1F530 | 0x800E2000 | 0x59330 | shares slot with seg462 |
| seg285 | LZ @0xBAF500 | 0x802D8000 | 0x18C8 | |
| seg454 | LZ @0xC67B90 | 0x802D8000 | 0x2C60 | shares slot with seg285; maps TLB 0x10000000 |
| seg462 | LZ @0xC6C320 | 0x800E2000 | 0x418A0 | |
| seg703 | LZ @0xD19000 | 0x80228000 | 0x5CF0 | |
| seg78 | LZ @0xA01F60 | 0x80100000 | 0x1E0 | main calls 0x80100000 |
| mod12…mod981 (13 files) | LZ | 0x80200000 | 0x9C00–0x13AD4 | code+data modules with a 3-entry jump vector (+0/+8/+0x10); code extent found by following the vectors |
| entry 209 (file 0xD1) | LZ @0xB82F30 | **0x50000000 (TLB)** | 0x6370 | **not recompiled yet** — see TLB below |

Overlay VRAMs were inferred from internal `jal` targets landing on function prologues and
cross-checked with absolute `j` targets (seg454 was wrong at first; `j` evidence fixed it). The recomp image appends each
decompressed segment at a virtual ROM offset ≥ 0x01000000 (`syms/overlay_map.json`).

## Graphics / audio microcode
- `RSP Gfx ucode F3DEX fifo 2.08` and `xbus 2.08` (F3DEX2 family) — supported by RT64, which
  provides resolution scaling, MSAA, widescreen and high frame rate.
- rspboot 0x8003DA00–0x8003DAD0. Audio ucode text 0x8003DAD0 (0xC60, IMEM 0x04001080), data
  0x8005A8C0, set up by the seg19 audio driver; recompiled with RSPRecomp → `rsp/aspMain.cpp`.
- Gfx ucode table at 0x80049AC4: F3DEX2 fifo text 0x8003E730 / data 0x8005AB70, xbus text
  0x8003FAC0 / data 0x8005AF90.

## libultra
100 functions identified by behaviour (`tools/libultra_names.py`). Still unresolved:
- controller polling (`osContInit`/`osContStartReadData`/`osContGetReadData`) — candidates
  0x800489D0 / 0x80048CB0; their callers are probably in the overlays.
- Controller Pak / Transfer Pak cluster (0x80047114–0x80049A58). The runtime has no Transfer
  Pak support (used for Mario Tennis GB link-up), so these need stubs that report "no pak".
- 64-bit helpers (`__ll_*`, `lldiv`/`ldiv` at 0x80032FA0…) are deliberately left unnamed and
  recompiled as plain code.

## Status (2026-11-07)
**Boots, renders and runs the intro.** Blimp flyover, stadium and the characters walking onto the
court render cleanly at window resolution, with RT64 frame interpolation on. The earlier "vertex
explosions" (and the apparent freeze) came from `osVirtualToPhysical`: the runtime's version
ignores the TLB, so the game's TLB-mapped model data (0x02xxxxxx) was handed to the RSP at wrong
physical addresses. The game's copy (0x80033160) is now `mt_osVirtualToPhysical`, implemented in
`src/game/mt_tlb.cpp` on top of the software TLB.

Launcher: original Mario Tennis splash (`assets/mt-splash.svg`, layout in `assets/launcher.rml`)
and tennis-ball app icon (`icons/`). Mods button removed. `tools/capture_run.ps1` takes window
screenshots (`-NoAutostart` for the launcher).

### How to build / run
- `sh tools/recompile.sh` — regenerates `include/recomp.h`, symbols, spin hooks, RecompiledFuncs,
  `rsp/aspMain.cpp` from the ROM (`MT_CONFIG=mt.trace.toml` for a trace build; see below).
- `tools\build_game.bat` — clang-cl 19.1.7 + Ninja build → `build\MarioTennisRecompiled.exe`
  (run with the project folder as working directory so `assets\` is found).
- Env switches: `MT_AUTOSTART=1` (skip the launcher), `MT_LOG=<file>` (stdout/stderr to file),
  `MT_LOG_LOADS=1` (log data-file loads too).
- Config/saves live in `%LOCALAPPDATA%\MarioTennisRecompiled` (the stored ROM copy
  `mt.n64.us.z64` is what the launcher's ROM picker would create).

### Fixes that got it booting
1. Loader hook reads the ROM offset at loader *entry* (the loader advances its own copy).
2. KSEG1 RDRAM pointers patched to KSEG0 (3 instructions), and KSEG1 folded in the software TLB.
3. Busy-wait loops yield to the cooperative scheduler (`tools/find_spins.py`, auto-generated
   hooks): the idle thread and seg78's wait on the gfx-task counter 0x80062608.
4. Audio ucode jump table: 16 entries; `0x02B0` means IMEM 0x12B0 (12-bit RSP PC).
5. **Software TLB** (`tools/make_recomp_h.py` → `include/recomp.h`, `src/game/mt_tlb.cpp`): the game
   maps segment data at 0x02000000+ (and the code module) through the TLB and dereferences it on
   the CPU; every recompiled memory access now translates non-KSEG0 addresses via a page table
   filled by `osMapTLB`. Unmapped accesses log `[mt] access to unmapped virtual address`.
6. Hand-written replacements in `src/game/mt_libultra.cpp` (second osStartThread copy, pak stubs).

### Trace tooling
`python tools/make_trace_config.py main` writes `mt.trace.toml` (first-call log of every main
function); build with `MT_CONFIG=mt.trace.toml sh tools/recompile.sh`. Ultramodern's scheduler
log can be enabled by switching `debug_printf` in `ultramodern/include/ultramodern/ultramodern.hpp`
(revert afterwards with `git checkout` in `lib/N64ModernRuntime`).

## Shadows (fixed 2026-11-07)
Character shadows are each model drawn a second time through a flattening matrix with othermode L
`A0504A75`: Z compare + update, `ZMODE_XLU`, **`G_ZS_PRIM`** (constant prim depth), env alpha 0x58.
On hardware the strict depth test lets only the first overlapping shadow triangle darken a pixel.
RT64 emulates prim depth as `z = primDepth * w` in the vertex shader, which lands an ulp off after
the perspective divide on some vertices, so overlaps depth-fought at high resolution (patchy,
flickering shadows). Fix: for `G_ZS_PRIM` triangle draws RT64 now uses a viewport depth range of
`[primDepth, primDepth]`, giving exactly the prim depth per pixel/sample with no shader changes.
The change is in `lib/rt64/src/render/rt64_framebuffer_renderer.cpp` (shared library folder) and
saved in `patches/rt64-mario-tennis.patch` (with the stretch option) -- re-apply if RT64 is updated.

Display-list inspection: `MT_DUMP_DL=<first> MT_DUMP_DL_STEP=<n> MT_DUMP_DL_COUNT=<k>` prints
render states and triangle counts per graphics task (`src/game/mt_dldump.cpp`; no-op otherwise).

## Narrow 304x224 layout (boot logos, front-end) -- centred 2026-11-07
The game's video mode is 384 wide (custom VI mode: width 384, xScale 0x299, hStart 0x080-0x2D5;
it swaps `framebuffer + 0x80` with origin 0x280, i.e. exactly one row in). The 3D game uses the
full 384x224 viewport, but the boot logos and the seg71 front-end (menus, player/character/court
select) draw into a 304x224 layout in the left 304 columns. RT64 shows the whole 384-wide buffer,
so those screens sat ~10% left of centre, and parts drawn with other viewports/scissors showed
up as an offset "second copy" in the left margin.
Fix, per frame in `src/game/mt_dlfix.cpp` (runs before every graphics task): if the frame uses
a 304/344-wide scissor, every such scissor becomes 40..344, every viewport centred at x=152
(trans.x 608) is re-centred at 192, texture/fill rectangles under the narrow scissor move +40 px,
and a small display list at 0x807FFC00 that clears both 40 px margins is spliced in after the
frame's first clear. (The earlier seg71 viewport patch and scissor instruction patches are kept;
the pass is idempotent with them.) Verified: logos, title, main menu, play mode, player and
character select.

## Display options
- Resolution (Settings > Graphics): **Auto** renders at the window / full-screen resolution (e.g.
  1080p, 1440p, 4K), **Original** is native, **Original 2x** doubles it; **Downsampling** (2x/4x)
  supersamples on top of those two, plus MSAA 2x/4x/8x. Fixed **720p / 1080p / 1440p / 4K**
  render at that many lines whatever the window size (RT64 Manual, multiplier lines/240: 3, 4.5,
  6, 9; width follows the aspect ratio, so 1440p at 16:9 is 2560x1440). They are new
  `Resolution::Res720p..Res2160p` values in N64ModernRuntime (JSON "720p".."2160p"), mapped in
  `set_application_user_config` (`src/main/rt64_render_context.cpp`); `ui_config.cpp` binds
  `res_fixed` (Auto or fixed) to grey out Downsampling. Verified 1440p in the intro.
- Aspect Ratio: **Original** (4:3), **Expand** (widescreen 3D, wider field of view) and **Stretch**
  (4:3 picture scaled to fill the window). Stretch is new: `AspectRatio::Stretch` in
  N64ModernRuntime (`patches/n64modernruntime-mario-tennis.patch`), `RT64::VIRenderer::stretchToWindow`
  in RT64 (`patches/rt64-mario-tennis.patch`), mapped in `src/main/rt64_render_context.cpp`, button
  in `assets/config_menu/graphics.rml`. In Stretch mode the narrow 304-wide menu layout is
  cropped to its content and stretched too: `mt_dlfix.cpp` calls `rt64_set_horizontal_crop(40/384,
  344/384)` on narrow-layout frames (and resets it otherwise); RT64's VI renderer widens its viewport
  so only that span covers the window.
- Rumble works (confirmed on a real controller). `recomp::update_rumble` (src/game/input.cpp) runs on
  the VI thread every VI; it used to call `SDL_GameControllerRumble` right there, and that write
  (milliseconds on many pads, plus the controller-list lock) delayed VI messages, so every shot that
  rumbled dropped a frame. Now the VI thread only computes the strength (32 steps) and a worker
  thread sends it when it changes.
- `MT_PROF=1` (with `MT_LOG=<file>`) logs late gfx tasks (> 40 ms apart) and slow streaming loads
  (`src/game/mt_prof.cpp`). Busy-wait hooks call `mt_spin_yield`, which wraps `yield_self_1ms`.
- Settings pages were trimmed for this game: Controls show N64 button names (`include/recomp_input.h`)
  with generic icons; General keeps Rumble Strength, Joystick Deadzone and Background Input; Sound
  keeps Main Volume (Zelda-only options removed from the `.rml` files).

## Launcher look
Sunny court theme (picked by the user from three mockups): `assets/mt-splash.svg` is original
artwork (sky, sun, clouds, hills, hedge, grass court, ball); the old night-stadium art is kept in
`build_assets/mt-splash-night-backup.svg`. Title outline/shadow and pill-shaped menu buttons are
styled in the `<style>` block of `assets/launcher.rml`. Title and buttons use Luckiest Guy
(`assets/LuckiestGuy-Regular.ttf`, Apache 2.0, licence in `assets/LuckiestGuy-LICENSE.txt`, from
google/fonts), loaded in `src/ui/ui_state.cpp`, uppercase because its lowercase is small caps.
Settings menus keep the stock fonts.

## Save data
16 Kbit EEPROM. With 4K the game probes, rejects it, never reads/writes and shows "The saved data
has been erased due to corruption or damage" every boot. Saves: `%LOCALAPPDATA%\MarioTennisRecompiled\saves\mt.n64.us.bin`.

## Window code modules (0x10000000)
Seventeen small modules (entries 17, 33, 99-106, 147, 163, 178, 200, 774, 965, 989; < 8 KiB, same
entry stub) are linked at 0x10000000 and run through seg454's sliding 2x4 KiB TLB window over
copies loaded side by side at 0x8023x000. Recompiled as sections `win*` (no relocations); the
loader hook registers each copy at its buffer, and `LOOKUP_FUNC` translates TLB-mapped function
pointers to the physical buffer address (include/recomp.h), so whichever copy is mapped is called.

## TLB-mapped code
Entry 209 (file 0xD1) is linked at 0x50000000 and recompiled as relocatable section `tlb209`
(177 HI16/LO16 relocs from `tools/tlb_relocs.py`); the loader hook registers it at 0x5000xxxx and
at its RAM buffer. With the software TLB in place the relocation is belt-and-braces.

## Next steps
1. A full match now plays (Mario vs Luigi, hard court). Next: longer play, other modes/courts, rumble.
2. Settings menus still list Zelda-only options (targeting, autosave, analog camera); remove them
   and set tennis-friendly control defaults.
3. Watch for `[mt] access to unmapped virtual address` lines during play.
4. ~~Shadow flicker~~ **fixed** (see "Shadows" below).

## Live recompilation build (release exe without game code) -- prototype 2026-11-07
- `tools\build_live.bat` → `build-live\MarioTennisRecompiled.exe`, CMake `-DMT_LIVE_RECOMP=ON`. No
  RecompiledFuncs: at game start `src/game/mt_live.cpp` LZ-decompresses the code segments from the
  player's ROM, builds an N64Recomp context from `src/game/mt_live_data.inc` and recompiles all ~2190
  functions with N64Recomp's live (sljit) generator (~0.5 s), then registers the section table.
- `mt_live_data.inc` is generated by `tools/make_live_tables.py` from syms/, overlay_map.json and
  mt.us.toml (addresses, names, relocs, patches, hook bodies only). Rerun it after changing any of those.
- Library patches: `patches/n64recomp-mario-tennis.patch` (live generator: software-TLB address
  translation via `translate_address`, `lookup_uncompiled_functions`, "@event:N" hooks) and the
  runtime patch (`recomp::set_rom_loaded_callback`, `section_addresses` allocated in register_overlays).
- Status: boots through the intro to the court with characters. Not yet tested: a full match, saves,
  performance vs the offline build.
- Audio: live builds run the game's own audio microcode (NAudio variant) from RDRAM with an RSP
  interpreter (`src/game/mt_rsp_interp.cpp`, using the runtime's vector unit). `rsp/aspMain.cpp` is only
  compiled into dev builds. Dev check: `MT_AUDIO=compare` runs both per task and reports DMEM
  differences (3000 tasks, 0 differences on 2026-11-07); `MT_AUDIO=interp` uses the interpreter alone.
  NB: rspboot leaves $1 = 0xFC0 (OSTask in DMEM); the interpreter sets it.
- Release package: `release\MarioTennisRecompiled-Windows.zip` (exe, SDL2/dxc DLLs, assets,
  recompcontrollerdb.txt, LICENSE, README.txt). No game code or data. `release/` is git-ignored.
- Status: user played a full match on the live build. Final package smoke-tested (boots).

## Title-screen demo fixes (v0.1.1, 2026-11-08)
- Window modules 108, 473, 1042 (entry stub `j 0x10000130`) were missing from gen_symbols.py; the demo
  uses them ("Failed to find function at 0x8023x000").
- RT64 `RDP::loadTileOperation`: inverted load tiles (lrt < ult / lrs < uls) wrapped to ~4 billion rows,
  stalling the renderer 12+ s per frame in replay / smash cut-scene effects (looked like a freeze with
  music playing). Clamped to the RDP's range. `loadWord` also masks addresses to 24 bits.
- Runtime: SP/DP task completions are retried instead of dropped when the game's queue is full (audio and
  graphics tasks complete on separate threads).
- Diagnostics (all MT_PROF): freeze watchdog prints each game thread's blocked queue and host-thread
  call stacks (dbghelp + .pdb) after 4 s without a gfx task; RT64 reports slow display lists/fullSync
  phases; MT_LOG_TIMERS logs osSetTimer. A crash handler always prints the faulting exe offset
  (`llvm-symbolizer --obj=build\MarioTennisRecompiled.exe --relative-address 0x...`).
- Hard freeze on the 2nd return to the title screen (after 3 demos): the game's allocator (func_80038040)
  looped forever on heap 9, whose first block header at 0x80238000 had been overwritten with 0x00010001.
  The title screen draws a 304x224 frame at 0x229CC0 that spans it; RT64 writes rendered frames back to
  RDRAM only after the whole GPU round trip, after the game had already built the heap there (on hardware
  the RDP finishes within ~1 ms). Fix in RT64 `State::fullSync`: snapshot the RDRAM each framebuffer pair
  will write over, and at write-back keep every word the CPU changed since (`mtCopyNativeToRAMMerged`;
  the snapshot is updated with RT64's own writes so later pairs in the workload aren't affected).
  Reporting SP completion after rendering (MT_LATE_SP=1, experiment) did not fix it.
- Box-shaped / missing player shadows, black speed board, black box in the smash double replay: images the
  game draws and reuses as textures (64-wide CI8 and 16-bit shadow silhouettes, 32/48-wide board digits,
  the replay's captured frame at 0x238040) came out wrong through RT64's GPU tile copies. They now go
  through RDRAM (`mtUseRDRAMForFramebuffer` in rt64_rdp.cpp: no tile copy, but the TMEM region is still
  tagged so RT64 syncs the image to RDRAM before it's sampled). The four 384-wide screen buffers
  (0x382000/0x3AC000/0x3D6000/0x000400) are reused ~1000x/s and keep GPU copies; routing those through RDRAM
  as well cost ~10x more late frames. Cost now: ~1 ms/frame (14-17 vs 11 late frames per 2 min of demos).
  MT_GPU_COPY_ALL=1 / MT_RDRAM_ALL=1 / MT_NO_GPU_COPY=1 switch behaviour for comparison.
- More MT_PROF diagnostics: heap-9 integrity check after every load and frame ("HEAP BROKEN"), game
  function names for unmapped accesses and stuck threads (`mt_live_describe`), 8 MiB RDRAM dump on the
  first freeze, RT64 write-back / framebuffer-as-texture logs, 5-second fullSync phase totals.
- Widescreen edge pop-in **fixed** (see "Widescreen culling" below).

## Widescreen culling (2026-11-08)
Objects (crowd, Bob-ombs, Koopas, players) were culled to the game's 4:3 view, so with Aspect Ratio
Expand they popped in/out at the left/right edges. The game's only object visibility test is
`func_800346A0(x, y, z, radius)` in main (34 callers): a sphere-vs-frustum test against the camera
block at 0x80062B88 (+0x00 projection, +0x40 view matrix, +0x80/+0x84 x/y projection factors,
+0x88/+0x8C viewport centre; filled by func_800341D4 / func_80034404). Returns 0 behind the camera,
1 outside left/right, 2 outside top/bottom, else the (negative) depth.
Fix: two hooks in mt.us.toml (before 0x8003473C / 0x8003475C, right after it loads +0x80 into $f0)
multiply the horizontal factor by `mt_cull_x_scale()` = (4/3) / window aspect in Expand mode, 1.0 in
Original/Stretch or when the window is 4:3 or narrower (`src/game/mt_widescreen.cpp`, refreshed every gfx
task from mt_fixup_gfx_task). The camera block itself is untouched, so screen-space placement
(func_800349EC / func_80034838: sprites, markers) is unchanged. Verified at 1680x720 (21:9) in the
clay-court demo: the Bob-omb row on the sideline now runs to the window edge (the baseline dropped it
outside the 4:3 area). MT_PROF late frames 16 vs 17 (baseline) over 20-130 s of demos.

### Menu 3D cut at the 4:3 edge (2026-11-12, GitHub issues #4/#5)
The screen buffers are 384 wide and the VI shows all 384 columns; the game draws inside its 304-pixel safe area
(x 40-344). Match 3D uses a full-width scissor (0-384) and gets RT64's wide viewport, but menu 3D (Toad on
Player/Character Select, Play Mode) uses the 40-344 scissor, so RT64 kept it on the 4:3 viewport and neither
widened its projection nor its scissor: models were sliced at the old edge. Fix (RT64 patch,
`rt64_framebuffer_renderer.cpp` + `rt64_projection_processor.cpp`): perspective calls on a 384-wide buffer whose
scissor is exactly 40-344 get the wide viewport, a widened projection and a full-buffer scissor in Expand mode.
`MT_WIDE_OLD=1` restores the old behaviour for comparison. The menu backgrounds are flat 4:3 pictures (drawn
as 2D tiles in 40-344), so they keep side bars; 3D models now extend into them. Matches were unaffected
(already full-width).

### Opening flyover shown in 4:3 (2026-11-12)
In the intro the camera flies to the airship, through its hull and down onto the stadium. In Expand mode the
overhead stadium shot showed the airship's underside and red band at the sides (real geometry just outside the
4:3 frame). The whole flyover is now pillarboxed: `mt_widescreen_flyover_check` (`src/game/mt_widescreen.cpp`,
called per gfx task) reads the camera block's view matrix (0x80062B88 +0x40). It switches on at the airship
approach (camera at the airship's position, y about -1500, looking slightly up), stays on while the camera
follows the flyover path (head-on banner, drop through the hull, overhead shot, descent centred on the court),
and switches off when the camera turns to follow Mario at court level. RT64 patch: `VIRenderer::pillarbox`
(`rt64_set_pillarbox`) narrows the presenter's scissor to the centred 4:3 part. Menus and matches never match the
approach, so they are unaffected. Note: that camera block does follow the picture; the "no rotation" phase is
the head-on banner view, not the overhead shot.
- Fix (2026-11-18): skipping the intro with Start left the 4:3 crop on, because the title/menu camera still
  matched the descent test, so menu 3D (Toad) was cut at the 4:3 edge again. The crop now also ends on any
  narrow-layout frame (title, menus; `narrow_frame` from mt_dlfix.cpp) and after 16 s at most.

## Graphics API option and Slang shaders (2026-11-08)
- Settings → Graphics → **Graphics API**: DirectX 12 (default on Windows) or Vulkan; takes effect on the next
  start. Old "Auto"/"Metal" values load as the platform default (`src/game/config.cpp`). If the chosen API can't
  start, `gfx_thread_func` (ultramodern events.cpp) retries with the other one and shows a message.
  RT64 has no OpenGL backend (D3D12, Vulkan, Metal only).
- Settings → Graphics → **Shader**: Off / CRT-Royale / CRT-Royale Fast (fake bloom) / CRT-Lottes; applied on
  Apply (`set_application_user_config` → `RT64::mtSetShaderPreset`). Presets live in `slang-shaders/` (copied
  next to the exe; not `shaders/`, which holds the UI's HLSL). The two CRT-Royale presets point their blur
  passes at `../blurs/blur9fast-*.slang`.
- Runtime: librashader 0.12.1 (`librashader.dll`, built from source; loaded with `librashader_ld.h`, so the game
  runs without it). `src/rt64ext/rt64_mt_shader.cpp` is compiled into rt64 (CMakeLists `target_sources`) to reach
  the native objects; RT64's `PresentQueue::threadPresent` draws the VI output into a 240-line BGRA image with the
  window's aspect (VIRenderer `targetWidth/Height`), runs the filter chain onto the swap chain, then draws the UI.
  Chains are created deferred (uploads recorded into RT64's command list; librashader never submits). After a
  frame the D3D12/Vulkan command list's cached heaps / pipeline layouts / framebuffer are reset so RT64 rebinds.
- Vulkan bug found: RT64's swap chain textures never set `VulkanTexture::imageFormat` (VK_FORMAT_UNDEFINED), so
  librashader built a render pass for an undefined format and NVIDIA's driver divided by zero
  (`nvoglv64.dll`). Fixed in rt64_vulkan.cpp. Render-pass mode is used (RT64's device is Vulkan 1.2 without
  dynamic rendering).
- Timing (2 min of demos, late frames): CRT-Royale D3D12 13, Vulkan 8; Royale Fast 4; Lottes 2.
- Diagnostics: `MT_SHADER=<preset>` overrides the menu; `MT_SHADER_TEST=1` builds the chain without drawing,
  `=2` skips librashader. Crash reports now name the module and print the call stack (dbghelp; librashader's
  symbols need its .pdb renamed to `librashader_capi.pdb`).

## Local multiplayer (2026-11-18, GitHub issue #1)
- Settings → General → **Local Multiplayer** (On by default; general.json `multiplayer_mode`). On: player 1 = keyboard
  + first controller, players 2-4 = the next controllers in connection order (SDL instance ID; `cur_controllers` is
  now sorted). Off: every controller drives player 1 (the old behaviour).
- **On + Keyboard** (`OnKeyboard`): controllers are players 1..n in connection order and the keyboard is player n+1
  (one controller: controller = P1, keyboard = P2; no controller: keyboard = P1). `recomp::mt_port_source(port)`
  holds the whole port mapping (present / keyboard / which controller) for all three modes.
- Bug found in testing: the controller count came from `cur_controllers`, which is only rebuilt when the game polls
  input; the game asks which ports are connected (osContInit) before its first poll, so it saw 0 pads and refused
  2-player modes ("Not enough controllers"). `get_connected_controller_count` now counts SDL's opened controllers
  (`controller_states`). Log line on change: `[mt] local multiplayer: N controller(s), mode M`. Verified with a real
  DualSense + keyboard in On + Keyboard: 2 Players opens Player Select and the keyboard moves the 2P cursor.
- The game re-checks controllers when a multiplayer mode is chosen ("Not enough controllers for a two-player game"
  with only the keyboard), so controllers can be connected while it runs.
- `src/game/input.cpp`: `mt_player_present(port)` decides which N64 ports report a controller (also used by
  `get_connected_device_info`); a thread-local controller filter (`set_input_controller_filter`) makes the shared
  binding tables read one controller per port; rumble is tracked per port and sent to that player's controller.
  `src/game/controls.cpp` `get_n64_input` serves ports 0-3 (keyboard only on port 0).
- Runtime fix (`patches/n64modernruntime-mario-tennis.patch`): `__osContGetInitData` wrote `*pattern = 1 << n`
  instead of `|=`, so only the last connected port was reported.
- Testing aids: `MT_PLAYERS=n` reports n players without extra controllers; `MT_INPUT_P2` scripts player 2 like
  `MT_INPUT`. Verified: with MT_PLAYERS=2 the main menu's 2 Players mode opens Player Select with 1P and 2P cursors,
  and MT_INPUT_P2 moves only the 2P cursor. Not yet verified with two physical controllers.
- Known limit: a controller that reconnects gets a new, later slot. Duplicate devices (e.g. Steam Input exposing a
  virtual copy of a pad) would count as two players; Local Multiplayer Off restores the merged behaviour.

## Expand = game widescreen hack (2026-11-18)
- Aspect Ratio **Expand** no longer uses RT64's own widening. The game itself renders a wider view: a hook before
  0x80033CAC (mt.us.toml) replaces the aspect func_80033C34 builds the 3D camera with (0x3FAB6DB7, 1.34; the
  widescreen GameShark code 81033CA6 3FE4 sets 16:9) by `mt_wide_aspect_bits()` = original x window aspect / (4:3)
  (src/game/mt_widescreen.cpp). RT64 gets AspectRatio Original with `VIRenderer::stretchToWindow`
  (rt64_render_context.cpp), so the picture fills the window; menus use Stretch's narrow-layout crop.
- The game culls for the wider view itself, so the old cull-factor hooks (mt_cull_x_scale) and RT64's menu-3D
  widening only apply with `MT_WIDE_RT64=1`, which restores RT64's Expand for comparison.
- The intro flyover 4:3 crop (rt64_set_pillarbox) still applies in Expand.
- Verified at 16:9 (intro, menus, match) and 21:9 (menu, match). 2D (HUD, menus) is stretched by window/(4:3).
- Known limit: menu 3D (Toad, players on Character Select) does not use func_80033C34's camera, so it is stretched
  with the 2D (mild at 16:9, obvious at 21:9).

## Bonus characters (2026-11-18)
- Donkey Kong Jr. and Shy Guy are normally unlocked by winning Star Cup tournaments. Their unlock bits are byte
  0x800653C8 (bit 0 DK Jr., bit 1 Shy Guy; GameShark "Have D.K. Jr. & Shy Guy" 800653C8 0003).
  `src/game/mt_bonus.cpp` sets both bits every graphics task, so they are always selectable (and saved).
  Verified: Character Select shows both in place of the two star slots.
- Launcher version label (`version_string` in src/main/main.cpp) bumped from 0.1.0 to 0.3.0.

## 16:9 game hack trial (2026-11-18)
- The widescreen GameShark code `81033CA6 3FE4` changes the aspect func_80033C34 builds the 3D camera with:
  lui/ori at 0x80033CA4/A8 load 0x3FAB6DB7 (1.34); the code makes it 0x3FE46DB7 (1.78). Anamorphic: the game
  renders a 16:9 view (and culls for it) into its 4:3 picture; Aspect Ratio Stretch spreads it over a 16:9
  window. 2D (HUD, menu text and backgrounds) is stretched 33%.
- Implemented as a hook before 0x80033CA8 (mt.us.toml) that sets $at = 0x3FE40000 when `mt_wide_hack()` is on.
  Trial switch only: `MT_WIDE_HACK=1` (release\MarioTennisRecompiled-Debug	ry-16x9-hack.bat). Verified: main
  menu, Play Mode and a match render 16:9 with Stretch.

## Launcher splash art (2026-11-18)
- The launcher background is now `assets/mt-splash.png` (pixel-art "Tennis" picture supplied by the project owner).
  The UI only loads images it is given the bytes of (and decodes PNG/DDS, not JPEG), so `ui_launcher.cpp`
  registers the file as `?/mt-splash` before loading launcher.rml, which shows it with `<img>` at full window
  width, vertically centred. The text title is hidden (the art has its own logo). lunasvg can't draw embedded
  `<image>` elements, so the art can't go inside mt-splash.svg (kept, unused).
