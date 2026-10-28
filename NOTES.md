# LittleBigPlanet 3 → native PC

Source: own dump of CUSA00473 (US), base v1.00 + patch v1.28 (final). Game files stay
in `CUSA00473*/` and must never be committed or shared.

## Layout
- `tools/ps4_imports.py` – lists a PS4 ELF's imports, grouped by library
- `tools/ps4_nid.py` – computes NIDs from function names (`python tools/ps4_nid.py scePadReadState`)
- `tools/resolve_imports.py` – names every import (via shadPS4's table) and marks shadPS4 coverage
- `analysis/eboot_named.md` – the full named import checklist for eboot.bin
- `runtime/` – the PC runtime (CMake). Build with `build.bat`.
- `third_party/shadPS4` – reference source (GPL-2.0-or-later), shallow clone of 0c9ca52
- `third_party/SDL3` – SDL 3.4.16 prebuilt (VC)

## Findings
- eboot.bin (v1.28) imports 646 functions from 51 Sony libraries; 641 named, 475 in shadPS4.
- Bundled modules are loaded as-is, not reimplemented: libc.prx, libSceFios2.prx, spu.prx,
  the FMOD plugins. Between them they need libkernel (+ libSceLibcInternal, libSceDiscMap,
  libSceSysmodule).
- spu.prx is the PS3 SPU job system carried over to x86.
- libkernel: 74/75 in shadPS4; the gap is `__stack_chk_guard` (a data symbol).
- Real gaps (~30): GameCustomDataDialog, MoveTracker, NpInGameMessage, ContentSearch,
  Coredump (all stubbable) and JpegDec (needs a real decoder).
- libScePad: only 7 functions used; all implemented (`runtime/src/hle/libScePad.cpp`).

## Loader (`build/lbp3.exe CUSA00473-patch CUSA00473 analysis/nid_names.txt`)
- Maps eboot.bin + the 7 bundled modules, links them to each other and to HLE, applies
  relocations (36,910). Report saved to `analysis/link_report.txt`.
- Imports: 692 satisfied by game modules (eboot's libc calls go to the bundled libc.prx),
  7 by HLE (pad), 590 stubbed functions + 10 stubbed variables (516 unique names).
- Stubs log the first 3 calls (`[stub] lib :: name`) and return 0.
- Doesn't execute anything yet. Needed before calling the entry point:
  libkernel basics (memory, threads, mutexes, files), TLS (PS4 code uses the fs segment,
  which Windows doesn't let us point at our own block), module init/entry calling.
- Weak plain-named imports (module_start/module_stop) resolve to null.

## Running (`.\run.ps1 [-Seconds N]`)
Builds, runs `lbp3.exe ... --run` with a timeout, prints the end of the log.
Full logs: `analysis/run_stderr.txt`. Prefixes: `[stub]` unimplemented call (first 3
per function), `[game]` the game's own printf output, `[kernel]` runtime notes.
Crashes print module+offset, registers, game return addresses and the last 16 stub calls.
`python tools/disasm.py CUSA00473-patch/eboot.bin <hex offset>` disassembles around an address.

Status (2026-10-09): Fios2 and libc start, eboot entry runs, the game initialises
VideoOut/Gnm and sits in its main loop submitting (empty) frames without crashing.
Caveat: much of that is stubs saying "OK": scePthreadCreate doesn't start threads yet,
semaphores/event queues return immediately, nothing is drawn.
- `[game] CMD_SET...` lines look like LBP's PS3-style command layer reporting
  unsupported render state.
- PS4 module init (DT_INIT) is usually at vaddr 0; `hasInit` tracks presence.
- TLS: 1164 `mov r64, fs:[0]` in eboot rewritten to `gs:[0x1480+slot*8]` (TEB TLS slot).

Threads (2026-10-09): real threads, keys, once, mutexes, condvars, rwlocks, semaphores
(sce + POSIX), event flags, event queues (timers, user events), clocks/sleep are in
(`hle/kernel_pthread.cpp`, `kernel_sync.cpp`, `kernel_time.cpp`). Fake 60 Hz flips with
flip events in `hle/videoout.cpp` so the frame loop paces like real vsync.
- The game starts 13 threads: Orbis::Gfx::Callback, bringup, PlayGo Thread,
  GameDataWrapper Orbis Thread, JobManagerWorker0-3, HttpJobManagerWorker0-1,
  Streaming Deity Priest / High Priest (both exit early), ...
- `run.ps1` passes `--dump-after=N`: prints where every thread is in game code, then exits.
- Current blocker: main thread polls a wait object (eboot+0x5ce40, 100 ms timeout) forever,
  most likely "loading finished". File functions (sceKernelOpen/Read/Stat/Mkdir...) are
  still stubs, so nothing ever loads.

Files + boot (2026-10-09):
- VFS (`core/vfs.cpp`): /app0 = mods/ > patch > base; /temp0 /data /download0 under user/;
  /savedata0.. mounted per save folder (user/savedata/CUSA00473/<dir>/).
- File functions in `hle/kernel_fs.cpp`; first open of each path is logged as `[fs]`.
- libSceLibcInternal imports (Fios2) are aliased to the game's own libc.
- sceKernelLoadStartModule finds the already-loaded module and runs its init once.
- Game memory must live below 0xFC_0000_0000 (libc's mspace rejects higher addresses):
  MapHost uses VirtualAlloc2 with address limits [4 GB, 0xFB_FFFF_FFFF].
- GPU scaffolding: PM4 walker (`hle/pm4.cpp`) executes only memory-writing packets
  (EVENT_WRITE_EOP, RELEASE_MEM, EVENT_WRITE_EOS, WRITE_DATA, follows INDIRECT_BUFFER) and
  raises sceGnmAddEqEvent interrupts. The game's Orbis::Gfx::Callback thread runs per-frame
  callbacks when EOP labels hit 1, in ring order - one missed label stalls everything.
- Gnm functions that write into reserved command-buffer space (Set*Shader, markers,
  hardware init, WaitFlipDone) fill it with one NOP (`hle/gnm_packets.cpp`). Leaving it
  unwritten desyncs the PM4 walk. Real register writes come with rendering.
- Implemented: user/system services, app content (no DLC), PlayGo (all installed), save data.

Boot sequence: `bringup` runs ~70 named steps (FIOS, TitleID, ... GfxMemPools, Fonts,
VirtualMachine, Share, SPURS, NGfx, NHUD, Input, LoadingScreen, GameData, Audio, Pipeline,
SceneGraph, Costume, Softbody, InventoryCollection, Fibres, AnimFibres, ProfileCache, ...).
Log them with `.\run.ps1 -Break eboot.bin+0xab095` then `python tools/boot_steps.py`
(break at 0xab092 to see a step before it runs). Each step: [rbx] name, +0x10 fn, +0x18 cleanup.

Offline mode (2026-10-09): PSN/network behave like a disconnected console
(`hle/np_offline.cpp`, `hle/net_offline.cpp`): signed out, NetCtl disconnected, sockets
exist but never receive (EWOULDBLOCK) and sends fail (ENETUNREACH). Stubs returning "OK"
made the game spin (recvfrom was polled 397 million times).
- ProfileCache was waiting for the player to confirm an on-screen prompt.
  `LBP3_AUTOPRESS=1` taps Cross every 2 s (debug aid until we can see the screen).
- Steps after it: PodComputer, RequestInitialLevel, ScreenshotInventory, LegalBlock,
  Network (75).
- Also offline-correct now: libSceHttp/libSceSsl (requests fail with NETWORK), libSceRtc
  (real clock), sceNpGetOnlineId/GetNpId return a local ID (LBP3_PLAYER_NAME or the
  Windows user name) while sceNpGetState stays SIGNED_OUT.
- `LBP3_AUTOPRESS=cross,circle` alternates buttons.

Network step (75) analysis - eboot+0x1920 loops until network manager (0x130d5b8)
reports a state other than 2 (eboot+0x35fb30 polls 26 sub-tasks; two stay at 2).
- Sub-task eboot+0x35f2d0 runs "flows": table at eboot+0xe1c990 (rows of action ids,
  0x68 bytes per row), current flow [obj+0x24c], step [obj+0x250]; action jump table at
  eboot+0x365b10 inside eboot+0x3606f0.
- Stuck in flow 2, step 5, action 3 (handler eboot+0x360ae4): waits while the player's
  online ID ([obj+0xb8]) equals "OfflinePlayer" (sceNpCmpOnlineId). Its 50 s timeout code
  is only reached after that check, so it never fires. The game names the player
  "OfflinePlayer" because we're signed out.
- Flow 2 ("sign in") is requested at eboot+0x34e985 right after a message box built from
  localized text id 0x9252aeda (text lookup eboot+0xc4040, message box eboot+0x5279c0).
  So a network message is on screen and some answer leads to the sign-in flow.
  Needs the screen to see which prompt/answer a real offline console takes.

Debug tools:
- `--break=module+0xoffset` (run.ps1 -Break a,b): logs registers + memory at rdi/rax/rbx,
  then continues (int3 + single-step re-arm, `core/debugger.cpp`).
- Thread dumps symbolize runtime frames (DbgHelp) and list the most-called stubs.
- Mutex diagnostics: timed-lock give-ups and refused unlocks print owner + lock site.
- `tools/disasm.py` labels calls to imported functions with their names.

Next: identify the ProfileCache dependency; then VideoOut window + Gnm rendering.

## Graphics (started 2026-10-09)
- `video/presenter.cpp`: SDL window (1280x720, resizable, vsync) on its own thread,
  opened by sceVideoOutOpen. Closing it exits the game.
- sceVideoOutSetBufferAttribute/RegisterBuffers implemented: LBP3 registers 2 display
  buffers, 1920x1080, pixel format 0x80000000 (B8G8R8A8 sRGB), tiling mode 0 (tiled).
- Each submit+flip shows that buffer index; the presenter uploads it as linear BGRA.
  Nothing is drawn into it yet (draw packets are skipped), so the window shows black or
  junk.
- PM4 reader now shadows context registers and counts draws: during boot each frame is
  13-18 draws + 1 dispatch rendering directly into the display buffers (CB_COLOR0_BASE
  alternates 0x160800000 / 0x160ff8000, one 1920x1080 tiled frame apart). That's the
  2D boot/message screen.
- sceGnmSetPsShader / sceGnmSetVsShader now write the real SPI register packets (ported
  from shadPS4); other Set*Shader / Update*Shader are still NOP-filled.
- The PM4 reader shadows SH registers too and dumps every new PS/VS binary seen at a draw
  to `analysis/shaders/<stage>_<addr>.bin` (cut at s_endpgm). The boot screen uses
  1 VS + 6 PS (300-860 bytes each) - first inputs for the shader recompiler.
- Renderer dependencies fetched (2026-10-09) as shadPS4 submodules at its pinned commits,
  under third_party/shadPS4/externals/: vulkan-headers, vma, glslang, sirit (+ its
  SPIRV-Headers), robin-map, xxhash, ext-boost, fmt, magic_enum, half.
- Host: Vulkan loader 1.4.341 in System32; GPUs: RTX 2060 (use this) + AMD iGPU.
- Shader recompiler integrated (2026-10-09): `runtime/cmake/shader_recompiler.cmake`
  builds shadPS4's src/shader_recompiler unmodified (C++23) plus fmt, sirit, Zydis,
  xbyak. Stand-ins for its emulator hooks: `runtime/shim/core/*.h` (settings all off,
  Signals -> vectored handler, empty process.h) and `runtime/src/gpu/shadps4_glue.cpp`
  (log/assert backends, GetUserPath -> analysis/shaders).
- `gpu/shader_translate.cpp`: at a draw, builds Shader::Info + fragment RuntimeInfo from
  our SH/context register shadows (AmdGpu::ShaderProgram overlays SH 0x8..0x1B) and runs
  TranslateProgram + EmitSPIRV. All 6 boot-screen pixel shaders translate to SPIR-V 1.6
  (analysis/shaders/ps_<hash>.spv, valid headers). Profile is a placeholder until a
  Vulkan device is queried (causes harmless denorm/signed-zero warnings).
- Build note: clang-cl for C too (Zydis is C); `-Wno-unused-command-line-argument` for
  deps that add /MP with warnings-as-errors.
- video_core integrated (2026-10-09): `runtime/cmake/video_core.cmake` builds shadPS4's
  src/video_core (all but vk_presenter, vk_swapchain, renderdoc, host_passes/) plus
  vulkan-headers, VMA, glslang (for the build-time host shaders incl. the GPU detiler
  tiling.comp), miniz, tracy headers (profiling compiled out). More stand-ins in
  runtime/shim: sdl_window.h (headless WindowSDL), imgui/renderer/{imgui_core,
  texture_manager}.h, absl flat_hash_map/set -> std::unordered_*, settings with all
  shadPS4 defaults, sceKernelIsNeoMode -> 0. common/thread.cpp left out (needs
  shadPS4's kernel TCB).
- `gpu/renderer.cpp` creates Vulkan::Instance headless (device, no swapchain) and leaks it
  on purpose (destroying it during static teardown crashes). `lbp3.exe --gpu-test`
  brings the device up: "NVIDIA GeForce RTX 2060 (driver 610.74)".
- Only what's referenced gets linked so far; wiring Liverpool/Rasterizer will surface
  the remaining hooks: core/memory.h (TryWriteBacking, CopySparseMemory, ClampRangeSize,
  SetRasterizer, IsValidGpuMapping, GetAddressSpace -> page protection), core/platform.h
  (IrqC for EOP/flip interrupts), core/debug_state.h, VideoOut driver flip hooks.
- Next steps, in order:
  1. Detile display buffers (GCN 2D tiling) so CPU-visible content shows correctly.
  2. Port shadPS4's PM4 command processor (video_core/amdgpu/liverpool.cpp) and register
     state so draws/dispatches are understood.
  3. Vulkan renderer + GCN shader recompiler (shadPS4 shader_recompiler -> SPIR-V via
     sirit), texture cache, render targets; present the rendered image in our window.
  4. Fill the Gnm Set*Shader / hardware-init functions with the real register writes
     (currently NOPs in hle/gnm_packets.cpp).

- FIRST PICTURE (2026-10-09): Liverpool command processor + Vulkan rasterizer run the
  game's command buffers; the boot screens render correctly and animate.
  - videoout: submit+flip waits for the GPU idle, patches the prepareFlip NOP
    (PatchFlipRequest, ported), and the flip completes on the GfxFlip IRQ. On that IRQ
    the display buffer image is read back from the texture cache
    (gpu::ReadbackDisplayBuffer, uses scheduler Finish -> slow) and shown in our window.
  - Write tracking (fixed flicker/stale text): AddressSpace::Protect does real
    VirtualProtect; every game mapping (direct, flexible, mmap) is registered with the
    rasterizer (gpu::MapMemory -> RegisterMemory + MapMemory; queued until Init). Fault
    handlers get EXCEPTION_POINTERS*. File reads use a bounce buffer because ReadFile
    into a protected page fails instead of faulting.
  - Debug aids: `tools/screenshot.ps1 -Seconds N -Out x.png [-AutoPress cross] [-Count k]`
    grabs the window; `LBP3_DUMP_FRAMES=skip:count` writes quarter-size BMPs to
    analysis/frames/.
  - BUILD GOTCHA: from bash, `cmd //c build.bat` silently fails ("not recognized");
    use `cmd.exe /c "<full path>uild.bat"` from PowerShell and check the exit code.
  - Open: some glyphs of the "Connecting..." title go missing in online-mode runs
    (texture invalidation?); readback via Finish is slow; Es/Gs/Hs/Ls still NOP fills.

## Network step (75), with the screen visible (2026-10-09)
- Screens: "Important Information" (save icon notice) -> "Connecting to
  LittleBigPlanet(TM)3 online..." and it stays there.
- Poller eboot+0x35fb30 runs 26 sub-tasks; the only blockers are 0x35ec50 (the flow
  runner 0x35f2d0) and 0x35ecb0 (waits on it).
- Flow 2 is the default start-up flow (set at 0x35fae9): actions [13,14,43,44,2,3,45,...].
  Action 2 (0x360960) proceeds when obj+0xb8 == manager player (mgr 0x130d5b8 +0x2f8).
  Action 3 (0x360ae4) returns idle while they're still EQUAL (0x1b640 = CmpOnlineId==0);
  only when they differ does it run its timeouts -> message 0xadb29176/0x5507532d -> flow
  0x13. So it waits for the player identity to change.
- "OfflinePlayer" is a static placeholder ID (0x1190cd0). Task 0x35e6f0 sets the manager
  player to it when connection helper 0x33cc60 returns 1/3. 0x33cc60 is a state machine
  ([obj+0x1a8]): state 2 polls sceNetCtlGetState; disconnected -> mgr+0x31c = 0xb, state
  0x17, returns 3 (offline). With the LAN "up" it goes to state 0xa and waits on the
  error-dialog manager (0x8c1c00 -> sceErrorDialog Open/UpdateStatus/Terminate, then
  sceSystemServiceGetStatus overlay flag).
- Implemented: one-shot NP state callback on sceNpCheckCallback (like shadPS4);
  hle/dialogs.cpp: sceErrorDialog as a Windows message box (OK or gamepad Cross
  dismisses; LBP3_AUTOPRESS dismisses it within 2 s).
- Experiment switches: LBP3_NP_STATE=signedin, LBP3_NET=online (IP obtained, servers
  unreachable). With both: error dialog 0x80550006 (some NP call still says signed
  out) -> dismissed -> 0x33cc60 returns 3 -> OfflinePlayer -> same idle in action 3.
- Next: find what makes the manager player differ from the flow's snapshot on a real
  console (which NP call returns SIGNED_OUT in signed-in mode; make the signed-in
  account consistent: GetNpId/CheckNpAvailability/auth fail with a *server* error
  instead), or what else advances flow 2 step 5.

## Past the Network step (2026-10-09, later)
- Why it hung: flow 2's action 14 snapshots the player, action 2 submits a local session,
  action 3 idles while the player is unchanged. The step only advances from server
  message handlers (0x36bcc1, 0x36bfdc, 0x36ca9c, 0x36cd18 do step++), which never run
  with LBP3's servers gone; the timeout path was unreachable behind the "unchanged" check.
- Fix: `core/patches.cpp`, a byte-checked compatibility patch list applied after
  relocation (skipped if bytes differ). Patch 1: eboot+0x360b01 `jne idle` -> nop, so
  action 3 uses the game's own timeout -> its "can't connect" message -> offline flow 0x13.
- Then the boot continues: FMOD audio threads, .fev sound banks, IME keyboard, voice
  chat, and the live-action intro cinematic plays with subtitles.
- Silent 0xC0000005 exits = stack exhaustion (256 KB VoipThread + our host code): game
  threads now reserve >= 1 MB and SetThreadStackGuarantee(64 KB) so overflows get reported.
- Also added: NP "signed in" mode is now consistent (LBP3_NP_STATE=signedin + LBP3_NET=online:
  availability/plus/parental/auth succeed, only game servers fail); the default stays
  signed out + disconnected, which works with the patch. HTTP send / DNS failures take
  LBP3_NET_FAIL_MS (default 3000) like a real timeout. sceErrorDialog implemented.
- Audio (hle/audioout.cpp): each sceAudioOut port is an SDL3 stream on the default device
  (SDL resamples/downmixes; PS4 non-std 7.1 reordered to SDL's). Output blocks to keep ~2
  periods queued, pacing FMOD. LBP3 opens 4 ports: 1024f stereo s16, 1024f 7.1 float
  (FMOD), type 2 mono float, 768f stereo s16. LBP3_VOLUME=0..100.
- Audio confirmed working by ear (2026-10-09).
- spu.prx crash: it keeps 32-bit copies of pointers to eboot globals; the PS4 always maps
  the main executable at 0x400000, so the loader now does too (module.cpp).
- Pacing: sceVideoOutSetFlipRate implemented (LBP3 asks for 30 Hz). Flips lock to the
  window's real vsync when the refresh divides evenly (60/120 Hz); the user's 144 Hz
  falls back to a timer (30 fps on 144 Hz alternates 4/5 refreshes; VRR fixes that).
  Presenter uploads only new frames; logs frame-gap stats every 300 frames.
- Intro video tearing (Bink, CPU-decoded via the job system): torn images were really
  rendered (top rows new, bottom old). Gone when each GPU submission completes before
  returning (renderer.cpp, default now; LBP3_GPU_ASYNC=1 opts out). Costs ~2.5 ms/frame.
  Proper fix later: find which fence the decoder trusts and honor it with the async CP.
- Patch 2: eboot+0x360b5b skips the long connect timeout (servers are gone).
- Next: voice ports (sceVoice still stubbed); what follows the intro.

## Loading screen after the intro (2026-10-10, in progress)
- Boot steps all finish (last: 77 "Keyboard"); the loading screen is the first level load.
- Reads: archives open fine (base/craftworld/Chunk1/patch _001.farc); reads stop after a few
  FIOS async reads of base_001.farc and FMOD's bank loads (lbp3_sfx_spots, sfxbank_compressed,
  lbp2_sfx). All worker threads idle afterwards -> something never "completes".
- Fixed so far:
  - Loopback networking (net_offline.cpp): LBP3 runs the Pod as a local session, host and client
    over UDP 127.0.0.1:3658 (POSIX socket type 6). Datagrams to 127.x are delivered to the bound
    socket; epoll reports readable sockets. Handshake (12/11/52/134/89-byte msgs) + keepalives work.
  - Identity: while signed out, sceNpGetNpId/GetOnlineId now fail with SIGNED_OUT like the console
    (they returned the Windows user name, giving the player two identities: "mewpe" and the
    game's "OfflinePlayer").
  - Skip intro: --skip-intro (play.bat SKIP_INTRO=1 default) serves the startup logo/intro
    .bik files as gamedata/videos/empty_video.bik (vfs.cpp).
- Ruled out: PlayGo, DiscMap, FIOS archive lookup, event flags (SPU jobs run), EOP interrupts,
  FMOD thread (idle after its loads), save data (main LBPxSAVE stays empty until later),
  IME keyboard step.
- Still hot while stuck: sceNpCmpOnlineId (~23k/s, via the "is local/offline player" helper at
  eboot+0x5d7b10..0x5d7b78), epoll create/wait/destroy (~3k/s, game host thread).
- Debug aids added: LBP3_COUNT_CALLS=<secs> (counting thunks on every HLE import; dump prints the
  top calls since the reset), LBP3_TRACE_READS=N (.farc/.fsb reads with thread), LBP3_TRACE_EF,
  LBP3_TRACE_ONLINEID, thread exit logging, dump snapshots stacks with ReadProcessMemory (old
  dump deadlocked on a thread holding a CRT/exception lock), tools/capture.ps1 (screenshot of
  the running game).
- Save backups from the debugging: user/savedata/CUSA00473_backup_1009, CUSA00473_test_1010.

## Why the first level never starts (2026-10-10, found)
- Main frame loop (fn ~eboot+0x6700): `loading++; while (levelResource->state(+0x45) == 0)
  { pump resources; networkManager.Update(7) /*0x34a760*/ }`. The level comes through the
  network manager's flows: currently flow 16 [14,42,46,20,...], stuck at step 3 = action 20
  (0x3620a1): waits for 0x36c150 "is the local player in the session?" (60 s timeout -> error
  0x6b3bbc4e, then retries).
- 0x36c150: session = 0x3af8c0(sessionMgr = [mgr+8], hostId = obj+0xdc ("OfflinePlayer")),
  then scans its 4 player slots (0xb0 each) for the local ID (mgr+0x2f8). The session manager
  (8 entries x 0x300, key at +0, valid flag +0x2f8) is EMPTY: the local player never joins.
  Host/client handshake over loopback works (12/11/55/12/11/89-byte msgs, then keepalives), but
  no join follows. Suspects: sceNpSignaling (stubbed), the join message content, P2P vports.
- Identity: while signed out, sceNpGetNpId/GetOnlineId now return "OfflinePlayer" (LBP3's own
  placeholder) — the network manager uses it on failure, but session code (0x3ba2e6) used an
  empty ID on failure; a Windows user name mismatched too.
- EXPERIMENT LBP3_PATCH_SESSION=1 (patches.cpp, env-gated): 0x36c150 -> always true. Then the
  game saves a play save (LBPxSAVEx12x349/playfart_12_349_1), compiles new shaders, black screen
  with save icon for ~5 min, then the level loading screen with tips, still loading at 10 min.
- Fixed on the way: socket numbers are small and reused (64..1023); they shared a counter with
  epolls (LBP3's select() makes ~3k/s) and FD_SET overflowed (crash at eboot+0x37bdd8). File fds
  skip 64..1023.
- Loading-screen decision: 0x5171a0 (shown when fade 2 done etc.; suppressed by
  [[mgr+0xa0]+0x5e0], set by 0x343b20 when [obj+0x5d4]==1 or [+0x580]==3 and fade 6 done).
- New debug aids: LBP3_BREAK_DELAY=<s> (arm breakpoints late), duplicate-address breakpoint
  fix, LBP3_SAMPLE frame-pointer chains, LBP3_WATCH with "*off+add" derefs, tools/timeline.ps1.

## The local session join (2026-10-10, in progress)
- Session manager = [mgr+8] = eboot+0x13243a0 (8 entries x 0x300). The "OfflinePlayer" entry IS
  created (0x3ac240 state 1 by 0x3ad61f, 4 by 0x3ae993) and then removed: 0x3aea50 asks
  0x377050(gameHost, entry) = "is this ID among the host's connected peers?" (peer list built by
  0x36e590 from the host's 4-slot player table at eboot+0x1323680; slot = ID +0, owner ID +0x24).
  False -> 0x34e9f0 (player-left cleanup) -> 0x3ad7c0 removes the entry (state 3, then 0).
- Hardware watchpoint (LBP3_HWWATCH=1323680): the host player table is only ever zeroed
  (eboot+0xbb3071, constructor). The host never registers ANY player.
- Loopback traffic: client and host share one P2P socket (type 6) bound 0.0.0.0:3658 vport 3658,
  sending to 127.0.0.1:3658 vport 3658: 12/11/55 then 12/11/89, then 11/12-byte keepalives.
  Receive side: recvfrom at 0x378c98, connection matched by IP (+0x34, or local IP global
  eboot+0x1418020) and port (+0x38), then 0x3795b0. Loopback now preserves the sender's vport.
- Experiments (env-gated in patches.cpp): LBP3_PATCH_SESSION (0x36c150 -> true) reaches the level
  loading screen with tips after ~5 min of black; LBP3_PATCH_PEER (0x377050 -> true) crashes at
  0x3aec3b reading the (missing) host peer record. So the host must really register the player.
- Next: follow 0x3795b0 -> message dispatch on the host; find the join/hello handler and why it
  doesn't add the player (auth/ticket? sceNpSignaling? local IP = 0 while NetCtl disconnected?).
- Tools: LBP3_HWWATCH=<eboot off> (write watchpoint on all threads, logs writer + fp chain).

## Game files (`tools/farc.py`)
- 5 .farc archives (6.5 GB): 92,428 files identified by SHA-1; `output/orbisguids.map`
  names 97% of them. Patch archives/map override base.
- `python tools/farc.py extract extracted [--filter 'gamedata/scripts/*']` unpacks to real
  paths (hash-verified); `list` writes an index (`analysis/farc_files.txt`).
- Unnamed hashes go to `extracted/_unnamed/`.
- The runtime reads `mods/` before the dump for every /app0 path the game opens directly.
- The game probes /app0/patch0.farc .. patch19.farc and /app0/gamedata/data/craftworld.ini
  at boot: likely native hooks for extra archives and config overrides (mod routes to test).
- Archive contents are opened through the game's own FARC code; overriding single files
  inside archives needs a hook in its resource loader (later).

## Keyboard controls (Oct 26)

- input/actions.{h,cpp}: named game actions bound to keys in user/config/input.ini (written
  with defaults on first run): A/D walk, W/S layer back/front, Space jump, Left Shift grab,
  E Pod computer, R Popit, arrows/Return/Escape/Backspace/Tab for menus. The user wants
  real per-action behaviour, not key->button emulation. For now each action's behaviour
  feeds the game's frame input (left stick, Cross, Square...) because that's the only
  entry into the character controller we know; replace actions one by one with direct
  calls once the game's functions are found (open Popit vs open computer are both Square
  now, so R near the computer opens the computer). Player 1 is always connected (keyboard).
- Oct 27, E/R reworked (self-tested in the Pod, remote "action <name> <ms>" holds an
  action): input/game_state.cpp asks the game through the functions behind its script
  natives: AnyPoppetActive__ -> eboot+0x979800(), IsStartMenuOpen__ -> 0x7c3320(),
  GetPodComputerInUse__ -> 0x6da320(). E calls RequestHostUsePod__b's function
  0x6da180(true) directly (opens the computer from anywhere in the Pod, never the Popit).
  R presses Square once per key press (Circle if the Popit is open) with the Pod script's
  RequestHostUsePod__b(true) blocked for 1 s (native hooked via core::HookNative), so it
  never opens the computer. Neither acts while a menu/Popit (E) or menu (R) is open.
  Gap: in reach of the computer the game won't open the Popit (not the engine routine at
  0x53e770, which only looks at a flag at +0x3d3; GetPodCompLink__ isn't the in-reach
  flag either), so R does nothing there.
- Script natives are fn(self, ret, args[]) (8-byte argument slots); many are thin
  wrappers over argument-free C++ functions callable from the input path (a game thread).
  analysis/natives.txt (LBP3_DUMP_NATIVES=1) lists all 3916 with addresses.
- Next: mouse over UI elements needs the game's UI layout / hit testing (RE).
- Boot sometimes stalls 30-40 s after "Vulkan device" with main busy in game code
  (libc.prx+0x30018 from eboot+0x3d1b09 / 0x3cf5e1 / 0x7f0bfc); other runs boot in ~14 s.
  LBP3_PERF_AFTER=seconds takes an F10 report that long after launch.

## Input
- SDL3 backend (`runtime/src/input/gamepad_sdl.cpp`): XInput, DirectInput, DS4/DualSense
  incl. motion, touch pad and light bar. Hot-plug; slots filled in discovery order.
- Mapping is positional: south/east/west/north = Cross/Circle/Square/Triangle,
  Back or touch pad click = touch pad button, Start = Options.
- Verified with `build/padtest.exe` on a ShanWan pad (Xbox 360 mode).
- TODO: check motion axis directions in game; integrate gyro into orientation if LBP3 uses it.

## Licensing
GPL-2.0-or-later (approved 2026-10-09) so shadPS4 code can be reused.

## Toolchain
- VS 18 Community, clang-cl 22.1.3 (required: game-facing functions use the System V ABI),
  bundled CMake + Ninja. `build.bat` sets everything up.

## Why the first level never starts (join chain, Oct 2026)

All loopback packets are delivered and decoded in order; the transport is fine
(packets are obfuscated by eboot+0x247a0 before sendto). The missing piece is the
local player never being added to the game host's player table
(eboot+0x1322760+0xd10, 4 slots of 0xb0):

- Flow 16 action 14 creates the local session entry (0x3af7b0); action 46 makes it
  current (0x3ad330 -> sessionMgr+0x1808) and sends message 0x18 "update my info".
- Host message switch 0x39f250: 0x18 only edits an existing slot (0x3a7c60). Players
  are only added by host message 0 (0x3a2b60 -> 0x3a3c70) or 0x17 (0x3a6f50).
- The client never sends message 0 or join request 2 (sent by 0xb9e970, triggered from
  session msg 1 handler 0x3731f0 or event path 0x3753a0). None of these fire.
- So the host never broadcasts 0x16 (0x3a88d0), the local entry's slots stay empty,
  0x377050 fails, and the entry is removed (0x3aea50 -> 0x34e9f0), clearing +0x1808.
- The level-load job (list at 0x12cc4e8, polled at 0x6fe7) is also handled by network
  code, so patching 0x36c150 only moves the stall to the loading screen.
Open question: what normally sends message 0 for the host's own local player.

## Reaching the Pod (Oct 2026)

Two causes, both fixed:

1. **Old network patches broke the host join.** Flow 2 action 2 sends host message 0
   (join) to the local player's own host; the patches at 0x360b01/0x360b5b skipped the
   wait in action 3, so flow 19 tore the connection down before the host thread read the
   join. Without them (now opt-in via LBP3_PATCH_NET_TIMEOUT) the session forms: host
   gets messages 0, 2, 4, 0x17, broadcasts 0x16, and the game shows its own "problem with
   your network connection" dialog, then continues offline.
2. **libSceFiber was stubbed.** The Pod load runs jobs as fibers (0x12200 polls
   sceFiberRun until each returns 4); the stub left garbage, so the main thread spun
   forever. runtime/src/hle/fiber.cpp now implements it (adapted from shadPS4), moving
   the TEB stack bounds onto the fiber stack while a fiber runs.

Frame rate: the ~3 fps was the GPU thread spending ~80% of its time in VirtualQuery
(MemoryManager::ClampRangeSize, called per buffer bind). Committed regions are now cached
(video_core_glue.cpp, cleared on gpu::MapMemory/UnmapMemory); the game runs at its 30 fps
cap. LBP3_SAMPLE now also unwinds host stacks and accepts host thread names (e.g.
LBP3_SAMPLE=GpuCommandProcessor:200).

Dark Pod: LBP3 clears alpha of its scene buffer with a compute "masked fill"
(dst = (dst & ~mask) | (value & mask), mask ff000000) that reads and writes the target as
a raw buffer. shadPS4 only copied an aliased image into a buffer for read-only formatted
buffers, so the fill started from stale zeros and wiped the RGB scene; only bloom showed.
buffer_cache.cpp now also syncs written buffers from an exactly aliased GPU-modified image.

Debug tools added along the way: LBP3_DUMP_TARGETS=<flip> (every render target as BMP in
analysis/targets/, with sizes, formats, brightness) and LBP3_TRACE_DRAWS=1 (with the dump:
every draw/dispatch of that frame with targets, sampled images, shader hashes, compute
buffers and small constant buffers). Readbacks (LBP3_READBACKS=relaxed) no longer deadlock:
TryWriteBacking uses WriteProcessMemory and the fault dispatcher runs handlers unlocked.

## Tutorial (Introduction) slowness and white-out (Oct 2026)

- LBP3 fills 256x128x64 RGBA8 lighting volumes (0x172930800, 0x174930800, R8 0x17e422000, 0x18993d000)
  one slice per draw, then samples them as 3D textures. shadPS4 saw each slice render as a 2D
  array of "slice+1" layers (ExpandImage 1->2->...->64 with copies), then freed the volume when
  it was sampled ("Image overlap resolve failed"), every frame: ~200 ms/frame. Fixed in
  texture_cache.cpp (a cached volume's depth satisfies a slice render target's layers) and
  image.cpp (barriers on a volume always cover the whole image). Tutorial now ~37 ms/frame.
- TryWriteBacking is a plain memcpy again (WriteProcessMemory made every GPU fence write a
  syscall); the readback deadlock it worked around is fixed by the unlocked fault dispatcher.
- shadPS4 warnings are rate-limited per call site (200/frame of SanitizeCopyLayers before).
- The white-out (fixed): the post pass ps 33c89263 adds light scattering sampled from the
  volume 0x172930800, which is built from two additive (ONE+ONE) 256x8192 RGBA16F
  accumulators at 0x180940000 and 0x181940000 (64 slices unrolled). The frame starts with a
  compute masked fill (cs 5c5e1be6) zeroing 64 MB from 0x180940000, but
  TextureCache::InvalidateMemoryFromGPU only flagged images starting exactly at the written
  address, so the second accumulator kept every previous frame's sum (values up to ~8000)
  and the fog saturated. It now also flags images lying entirely inside the written range.
- After that fix the user reports the Pod, menus and planet map run smoothly. Open next:
  (1) the Introduction chugs when moving forward (likely streaming in new level parts:
  shader compiles, texture uploads, or level loading on the main thread; take an F10 perf
  report while walking); (2) the Newton abduction cutscene freezes on a frame of lamps and
  never resumes (game still responsive? check the main/fiber threads with F10 and for a
  stuck video, audio or streaming wait).
- (1) fixed Oct 14: walking in the Introduction ran at ~41 ms/frame (main thread 80% in
  WaitGpuIdle). The GPU thread rebuilt each lighting volume every frame: LBP3 renders it
  as a 64-layer 2D array and samples it as 3D, same guest size, and ResolveOverlap's
  "equal size, one is 3D" branch expanded (copied, freed, recreated) the image each way.
  A render-target request for slices of a cached volume now reuses the 3D image (2D-array
  views of it are valid for attachments). Also the display readback recycles its pixel
  buffers through the presenter instead of allocating 8 MB per frame. Now a locked 33.3 ms
  while walking. The [video] status lines print avg/worst frame time and frames over 40 ms.
- Newton lamps / thruster freezes (Oct 14): not crashes. F10 showed the GPU thread spinning
  in a WAIT_REG_MEM loop and 'main' blocked in our synchronous SubmitGfx (WaitGpuIdle): the
  game satisfies that wait only after its submit returns (console submits are async). Now
  SubmitGfx/WaitIdle also return while the graphics queue is blocked on a WAIT_REG_MEM
  (Liverpool::WaitGpuIdleOrBlocked; LBP3_SYNC_STRICT=1 restores the old wait), and a wait
  stuck over 1 s logs its address/value/ref/mask. Needs the user to confirm both spots.
- Still froze after that: the stuck wait was memory 0x1e1303c80 == 1, written by async
  compute, and sceGnmMapComputeQueue / sceGnmDingDong were stubs. Implemented (port of
  shadPS4's, gpu::MapComputeQueue/DingDong in renderer.cpp; the game maps pipe 0 queue 3).
  Compute-queue WAIT_REG_MEMs also release the submit wait (queues_blocked_on_wait counter).
- Thrusters fixed by that. Placing an active missile crashed a second later: the gfx
  command processor met garbage (NextPacket "packet length exceeds remaining submission
  size", then a type-0 packet -> UNREACHABLE). The DCB passed pm4::FindOverrun at submit,
  so it changed or was incomplete when read (likely the guest writing it after submit,
  now that submits return while a queue waits on memory). ProcessGraphics now logs the
  spot (address, dword offset, next words) and drops the rest of that buffer instead of
  aborting; unknown type-3 opcodes too. The Newton "pieces floating" crash log was lost
  (play.bat now keeps play_log.1-3.txt). Idea from the user: a debug log of objects
  placed / properties changed in create mode.
- Those "bad packets" were nearly all `00000000 00000000` followed by valid packets: zero
  dwords are type-0 packets (register 0, one word), filler that hardware ignores. Skipping
  the rest of each buffer dropped draws (the LBP logo in the Bunkum flight cutscene) and
  likely a label write (rocket freeze: WaitRegMem stuck on 0x1e1303c18). Type-0 packets are
  now stepped over in ProcessGraphics and ProcessCompute (warning if the register isn't 0).
- Rockets fixed by the type-0 change. The Bunkum Heroes intro still froze after Swoop's
  introduction: WaitRegMem stuck on 0x1e1303c80 again, after real misparses (type-0 writes
  at 0xa/0x504/8193 regs, overruns in 0x1de62xxxx buffers). The zero pairs always follow a
  7-dword packet at a buffer's start (WAIT_REG_MEM size). Theory: async-compute shaders
  write the label and/or command words; shadPS4 parses PM4 and tests waits on the CPU from
  guest memory, so shader writes stay invisible without readbacks. Readbacks (relaxed) are
  now on by default (no measurable cost in the Introduction); LBP3_READBACKS=precise|off.
  Type-0 packets log the buffer head (first 6, and any with a nonzero register).
- Readbacks didn't help (stuck on 0x1e1303c78 next) and lengthened loads: off again. The
  buffer heads are c0055800 = ACQUIRE_MEM (7 dwords) + a zero type-0 filler: harmless.
  The game's main thread then waits on its own semaphore (eboot+0x210d8). Labels rotate
  (…c18/c78/c80), each waited == 1. Now a stuck WAIT_REG_MEM (gfx or compute) prints the
  recent label writes within 0x1000 of it (gfx EOP/EOS/WRITE_DATA, compute WRITE_DATA/
  RELEASE_MEM, MEM_SEMAPHORE signals; 64-entry history). MEM_SEMAPHORE waits also release
  the submit wait now and report when stuck over 1 s.
- With the MEM_SEMAPHORE change the intro got past Swoop and the titans; it then sat on a
  white checkpoint screen at a steady 30 fps (no stuck wait; main in WaitGpuIdleOrBlocked
  as usual). Just before: misparses again, type-0 "packets" 0x20000001 (8193 regs at 1,
  seen in an earlier session too) and 016f8300. In one earlier case the words before the
  garbage were an ACQUIRE_MEM body (82c40040 00008000 00000000 016f8300 00000000
  0000000a) without its header, so something earlier is mis-sized. Type-0 warnings now
  print the previous packet and the next words.
- Oct 16: crashed before the first Bunkum level with no crash report; the Windows event log
  had exception 0x80000001 (STATUS_GUARD_PAGE_VIOLATION) in VCRUNTIME140 memcpy, right
  after a savedata write. Game thread stacks were reserved and committed on demand
  (STACK_SIZE_PARAM_IS_A_RESERVATION); PS4 code doesn't probe stacks, and the fiber code
  swaps the TIB stack bounds, so a guard page could be hit outside normal growth. Now
  CommitWholeStack() (kernel_pthread.cpp) commits each game thread's stack at start (and
  the main thread's), keeping 2 pages reserved at the bottom; the crash handler also
  reports EXCEPTION_GUARD_PAGE. Boot measured 14 s to the Pod (user saw ~30 s).
- Stack fix held (no crash). Checkpoint hang explained by the label history: a gfx buffer
  (0x1de61ba84, dword ~4536) misparses at `c0001000 c0026900 00000323 ffbaeff3 00000000
  c00b6900 00000318 0171fe80 0ef000ef ...` (a count-0 NOP then a valid SET_CONTEXT_REG and a
  CB0 setup at 0x171fe8000, 1920x1080); taking the NOP as header + 1 word swallows the
  next header, and the rest of the buffer is lost. Then compute waits on 0x1e1303d10 to
  reach 0x20f8 (stale at 0x207a; gfx would have updated it) and gfx waits on c70 == 1
  (compute would set it): circular. Per the PM4 spec the NOP has one body word, so either
  the parser was already off or the words are stale. The first nonzero type-0 in a buffer
  now logs the last 8 headers parsed and the 32 words before it.
- Next run: no misparse, still the white checkpoint. F10 (now also dumps all threads and
  shows a toast when done): main in sceKernelWaitSema (eboot+0x210d8 via 0x5ce51),
  Orbis::Gfx::Callback waiting on the GPU equeue. Compute waited on 0x1e1303ce0 == 0x898
  (value 0x81a); compute's own RELEASE_MEMs write ...9 values to cd8..d10, so the ...8
  stage comes from another writer and none was in the history. shadPS4 runs DMA_DATA
  writes on the GPU copy only (FillBuffer/CopyBuffer), invisible to CPU-side waits.
  DMA_DATA immediate fills to memory are now also written to guest memory (MirrorDmaFill,
  gfx and compute); GDS->memory and memory->memory DMAs are recorded in the label history.
- ROOT CAUSE of the misparses (Oct 16): ProcessGraphics executed a packet and only then
  re-read header->type3.NumWords() to step. Executing an EOP raises an interrupt that wakes
  the game's Orbis::Gfx::Callback thread, and a WAIT_REG_MEM lets the game run; the game
  then reuses that command memory (the work is "done" from its view), so the step size came
  from new contents. Context dump: EOP packets every 6 dwords (5594..5624), parser stepped
  5618 -> 5623. Now the size read before execution is used (gfx count, CondExec skip, CE
  loop); compute already did. This likely explains every "type 0" garbage case before.
- Oct 26: still the checkpoint hang, now a deterministic misparse 10 dwords before the end
  of ~7158-dword buffers (four buffers, same contents): ACQUIRE_MEM A (84c44000 00007e90 ...
  01779380 ...) has a complete ACQUIRE_MEM B (88c40000 1 0 0 0 a) inside it. No parser
  could read that, so the GPU must have read it earlier on hardware: the game rewrites
  command memory after submitting it. gpu::SubmitGfx now parses a per-submission snapshot
  (ring of 64; LBP3_COPY_CMD_BUFFERS=off disables). shadPS4's own IsCopyGpuBuffers copy
  resets only in its sceGnmSubmitDone, which our runtime doesn't call, hence our own.
  Indirect buffers inside a DCB are still read from guest memory.
- With the snapshots: no misparse or stuck wait at all, yet still the white checkpoint.
  The game keeps rendering at 30 fps with music, but never opens any next-level file:
  it's waiting to be told to move on. Suspect the local session's level-change flow
  (in-level "continue to next level" may use a different flow from the map's flow 16).
  net_offline now logs the first message of every new size (hex + text). Next run: F10
  at the checkpoint for the thread dump + the message log.
- Loopback session messages are encrypted (sizes say nothing). The F10 dump's stub counts
  showed sceNpBandwidthTestGetStatus polled ~3100 times (once a frame for the ~100 s on the
  checkpoint); its stub never reported "finished". Implemented sceNpBandwidthTest
  InitStart/GetStatus/Shutdown/Abort in np_offline.cpp (finished at once, 20/100 Mbit/s).
  The test already starts at boot.
- Oct 27, self-reproduced (nav.py legend; refs map_needle/map_legend/legend_stuck): The
  Legend of Bunkum (cutscene on the Prologue map, between Introduction and Needlepoint
  Peaks) ends on the white checkpoint at ~106 s after the map press. Workaround: pause ->
  skip loads Needlepoint Peaks. Ruled out: GPU (no stuck waits/misparses), readbacks,
  bandwidth test, PlayGo, HTTP/NP, video, voice-line streams (all opened and closed in
  order), and sounds (LBP3_PATCH_SOUND_NEVER_PLAYING forces the is-playing test at
  eboot+0x3ea8c0 to false: still stuck). Script natives in the stuck state
  (LBP3_COUNT_NATIVES=1, F10): only ~43, mostly per-player HUD: IsScoreBoard__Q5Thing 9/frame,
  GetNumPlayers, DoesPlayerExist, GetYellowheadFromPlayerNumber, PerFrameUpdate (5/frame),
  IsActiveLevelLink, GetLevelSlotID. No Sackboy is spawned at the checkpoint; a level ends
  when players reach the scoreboard. Next: why the player doesn't spawn after the movie
  (engine side; compare with the Introduction's spawn).
- Oct 27 (later): LBP3_NATIVE_TIMELINE=1 (logs natives' first call / stop, once a second)
  shows no script activity at the movie's end: nothing new after the last voice line, no
  GotoScoreboard (msg 0x4d), nothing cutscene-related. LBP3_TRACE_IS_PLAYING=1|force (hook
  on GetIsPlaying__Q11AudioHandle): every polled sound reports 0, so not an audio wait.
  Engine facts: IsCutSceneLevel = level's world part [+0x60] == 2; in such a level
  IsCutSceneActive/MovieActive just mean play mode (game+0x108 == 0), so the letterbox stays
  forever; the cutscene camera manager is PWorld+0x810 (+0x8 = current camera, +0x2c/+0x34
  floats). Breakpoints at the hang (eboot+0x492f10, rdi = manager): +0x8 == 0, i.e. the last
  cutscene camera has finished. The level should then end through its own logic (a camera's
  CUTSCENE_CAM_FINISHED output wired to something), which never happens. Clue: the game
  stats cs01_10..cs01_160_newton.mp3 at load but only 10..100 play, so the cutscene stops
  early (lines 110-160 never start); the next camera/sequence isn't triggered.
  WORKAROUND (default on, LBP3_CUTSCENE_AUTOSKIP=off disables): game_state::CutSceneWatchdog
  (called on pad reads) calls SkipCutSceneLevel's function (eboot+0x7c5d80, what the pause
  menu's skip uses, msg 0x51) once a cutscene level has had a camera and then none for 5 s.
  Verified: Needlepoint Peaks loads with Sackboy. Real cause still open (what should start
  the part with lines 110-160).
- Oct 28, user: slowdown when popping prize/score bubbles. The play log's slow stretches
  (avg 55-80 ms) all had 16-28 "early releases" per 30 frames: each cost the 20 ms grace
  period (a queue waits on memory the guest writes only after its submit returns). Now
  EndBlockedWait (liverpool.cpp) learns such wait addresses: if the guest had to be released
  to end a wait and no command stream wrote that address meanwhile (label write history),
  later waits on it release the guest at once; an address a command stream writes is
  forgotten again. A first version that learned every timed-out address (without the
  GPU-write check) made things much worse (2 s hitches). The [video] line counts learned
  addresses. Test run: 8 learned in the Needlepoint intro, then steady 33.3 ms walking.
  Bubbles themselves not reproduced by script (Sackboy can't make the first ledge).
- Oct 28, user: at the end of Needlepoint Peaks (bouncing between two islands on a flowing
  jelly river) Sackboy got launched far out of the level onto the create-mode floor; after
  popping, the respawn island was stationary with nowhere to go. It happened during the slow
  stretches above (game catching up with 2 sim steps per frame?). Recheck after the fix.
- Game files moved out of the project (for GitHub): ..\LBP3 Game Files holds CUSA00473,
  CUSA00473-patch, extracted, itemzflow. lbp3.exe without dir arguments finds it via
  LBP3_GAME_DIR, game_dir.txt (first line) or ..\LBP3 Game Files (main.cpp GameDir;
  tools/gamedir.py for scripts); names file moved to tools/nid_names.txt. .gitignore
  excludes game files, build/, analysis/, user/, mods/ and tools/nav_refs/*.png.
- tools/remote_start.ps1 passes "--..." arguments to lbp3.exe (e.g. --break=eboot.bin+0x..).
- New tools: LBP3_COUNT_NATIVES (core/native_trace.cpp wraps the 3916 natives found via
  {fn, "Name__Sig"} pairs in the relocation table), LBP3_FIND_REFS=text (relocated slots
  pointing at strings), tools/find_refs.py (code refs to a string), LBP3_TRACE_FILE=text
  (every read of a file), LBP3_DUMP_SOUNDS (sound instances still playing), audio file
  closes logged, LBP3_PERF_AFTER=seconds.
- CAMERA SNAPS, Oct 27 (measured with LBP3_TRACE_SIM=1: sim steps per flip via
  GetSimFrame__'s function eboot+0x7c79e0, 60 per line; tools/camera_check.py measures
  scroll per presented frame). The game's sim/game-frame counters advanced 0 or 2 per
  flip in bursts ("1202012102111020201121"). Causes and fixes:
  1. Main cause: the early submit release (WaitGpuIdleOrBlocked) let the main thread
     run ahead of its frame's flip; LBP3_SYNC_STRICT=1 gave almost all 1s. Now the
     release waits out short GPU waits and only fires after a queue has been blocked
     20 ms ("early releases" counted in the [video] line; 0 in normal play).
  2. Flip pacing restarted its grid at every late frame (drift): now a fixed grid; up
     to half a vblank late flips at once, later waits for the next vblank.
  3. sleep_until overshoot: sleep to 2 ms before the slot, then spin.
  4. Main thread + "game host thread" read a flip-anchored monotonic clock: reads within
     8 ms of the last grid slot return the slot's time, later reads run 8 ms behind
     (LBP3_FRAME_CLOCK=off disables). Effect smaller than 1.
  Left: a rare isolated 0 step (~1 per 100 flips, occasional short burst).
- Camera snapping is game-side: recording frames while walking ("frames N" remote command
  writes analysis\frames + times.txt) and phase-correlating the scroll shows steady
  ~2-3 px/frame with a periodic double step followed by two near-zero steps (about every
  90 frames), while hand-over times stay 33-34 ms. Hitches of 100-440 ms gave a normal
  single step, so the step is capped. Looks like a beat between two clocks / uneven
  measured dt. LBP3_TRACE_TIME=1 logs clock and sleep callers with two frame-pointer
  levels: timer wrappers eboot+0x1b1b0 (ns) / 0x1b3f0 (float s); the 30 Hz sleeper at
  0x3f6057 is voice chat (usleep 32 ms), not it. Next: find the frame dt (main update
  0x34a..., candidates 0x78d421, 0x92f929, 0x39e827, 0x382a44).
- Camera judder: frames were handed to the window right after readback and only then paced,
  so they arrived 28-39 ms apart. Now WaitFlipSlot runs before PresentPixels. The user's
  monitor is 144 Hz, which 30 fps doesn't divide (frames alternate 4 and 5 refreshes); VRR
  (G-Sync) or a 120/60 Hz mode avoids that. If the camera still snaps, look at the game's
  clock use (frame delta vs. flip timing).
- Remaining: occasional 100-400 ms hitches in the Introduction with no texture-cache
  activity ([video] lines now count new/expanded images per interval); one earlier run had
  many 0.3-2 s hitches that did not reproduce.
- tools/nav.py: the computer menus remember the last highlighted entry and wrap around
  (a session ending in Create reopens there; blind Cross went into My Moon). Home and Play
  now compare the menu-list crop with the reference and press Up until the top entry is
  highlighted; a map-like screen never gets a blind Cross. Possible later: a direct level
  jump (hook the map's play-level call, LBP3_BOOT_LEVEL) to skip menus entirely.
- Create mode works (Oct 11 user report), with three issues:
  (3) running in one direction, the camera keeps snapping back a little and catching up with
  Sackboy. Could be uneven frame pacing, or a game-time vs. frame-time mismatch (check flip
  timing / the game's delta time).
  (4) the create-mode sky looks yellow/beige where it should probably be blue (a colour or
  format channel issue in the sky/backdrop pass? compare a PS4 screenshot).
  (5) switching a Thruster on (tweak menu, "Currently On: Yes") froze the game on that frame
  and the PC's fans got louder. Likely the rocket particles: a GPU hang/spin or a runaway
  loop on the GPU thread. Take F10 at the freeze, check remote_stderr for asserts or warning
  floods, and F9-trace the frame just before switching it on.
- Debug aids: F9 traces now list z read/write addresses and each texture's dfmt/nfmt/dst_sel;
  per-draw dumps (analysis\dump_draws.txt) save every render target at each bound address
  plus the depth buffer (min/avg/max), and LBP3_DUMP_RAW=1 writes the raw texels too.
- Remote control for test runs: tools/remote_start.ps1 (LBP3_REMOTE_INPUT=1), then write
  commands to analysis\input_cmd.txt ("square 400", "wait 5000", "lstick 1 0 800", "f9",
  "f10"). The first batch after loading is eaten by the network dialog; start with a press.
  Path to the Introduction from the Pod: cross, square, cross (Play), cross (Adventure),
  cross (Prologue planet), cross 600 (Introduction).
