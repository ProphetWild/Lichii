# Overnight log (2026-11-05 → 06)

Running notes while the owner sleeps. Queue: DLSS polish → Frame Generation → performance
(frametime spikes) → visual bugs (distant tree flicker/darkening; white flicker when the window
is not focused). DLSS 5 dropped for now; 4K DLAA shelved.

## Done before the night

- `00e4af1f0f`: per-object motion vectors, 4K Quality output (1440p → 2160p, preset M), camera
  matrix search, scene-frame boundary.
- `59b00d702a`: Lichii build-83 merged (perf switches now default to its hard-wired set); game
  time no longer scaled by CPU Clocks (the 1.7x speed-up), optional "Time boost"; 4K DLAA hidden.
- PR to zortos293/Lichii for the sharpening-network bypass: separate session `lichii-sharpen-pr`.

## DLSS polish: foliage

- **What draws the foliage.** With mesh-shader draws skipped (`EDEN_NX2_NO_MESH_FILE`), the
  tree canopy, bushes, grass and most plants vanish, so the foliage is mesh shaders.
- **Mesh pipelines** (`EDEN_NX2_MESH_PIPELINE_LOG=1`): most scene mesh pipelines have no task
  stage (workgroups come straight from the draw); a few prepass-like ones have one.
- **Object motion for mesh pipelines without a task stage**
  (`EmitOotMotionMeshVertex`, `vk_pipeline_cache.cpp` policy): history slot = draw identity +
  workgroup × max vertices + vertex.
  - First result: wrong vectors (32+ px on foliage with a still camera). The mesh programs cull
    and compact their output vertices, so an index can hold another vertex next frame.
  - Fix: each history entry also stores a signature (FNV-1a of the vertex's non-position
    attribute words); last frame's position is used only when it matches. Garbage gone, but
    with the signature over all attributes almost nothing matched. With only the first or last
    attribute, matches came back with 32+ px motion: a workgroup draws different vertices each
    frame (the meshlet list is most likely rebuilt by a culling pass).
  - Result: mesh history is opt-in (`EDEN_OOT_MESH_MOTION=1`), committed in `11d159695e`;
    foliage keeps camera motion. The proper fix is evaluating the program a second time with
    last frame's constants (design in `oot-motion-vectors-plan.md`).

## Frametime spikes (performance)

New diagnostics, emulator-wide, all off by default:
- `EDEN_NX2_HITCH_LOG=1` with `EDEN_NX2_GPU_PROFILE=1`. Each presented frame much longer than the
  recent median (> 1.6x and > median + 6 ms) logs a "Hitch:" line with:
  - the presenting thread's zone time during that frame;
  - the present path (acquire, record/submit, present call);
  - guest JIT work (`Dynarmic::g_jit_stats`: blocks compiled, compile time, full cache flushes);
  - HLE service time and the slowest request (`Common::g_hle_request_stats`);
  - the CPU time of the guest core threads (`Common::GuestCoreClock`, `QueryThreadCycleTime`).
  A summary every 600 frames: hitch count, worst, median frame, median guest core time per frame.

Findings (Kokiri Forest, walking and turning, 4K Quality DLSS):
- The median frame is about 21-23 ms. Hitches are 31-68 ms, a few per 10 s while moving; worst
  ~570 ms right after loading.
- **Right after loading:** 1,000-2,200 JIT blocks per frame (12-22 ms of compiling). While
  walking, new code sometimes adds 6-13 ms to a frame. Most hitches have no JIT at all.
- **Services:** under 1 ms per hitch frame; the slowest request is a 0.1 ms nvsrv ioctl. Not
  file I/O.
- **Our present path:** ~0 ms (no acquire/present stall). The guest's own present stats agree:
  it is never blocked on the present fence, but its frame-to-frame gap peaks at 50-100 ms. The
  game delivers frames late.
- **Guest cores:** about 60 ms of CPU per normal 22 ms frame (2.7 cores busy); during a hitch
  60-80 ms over ~45 ms (about 1.4 cores). In a hitch the game's threads wait rather than work.
  The most likely wait is GPU progress: the front GPU thread spends ~46% of its time draining
  into the back thread at semaphore acquires (`split drain`, site "acquire", ~4,500 per second).
- Next: per-frame front-thread zones and syncpoint/event wait times on the guest side, to
  confirm that the hitch frames are the ones where the front waits on the back. Then fix the
  acquire drains (most drained acquires report "release ran": the payload is already in memory).
- **Acquire drains are mostly genuine dependencies.** Per second, about 2,400 acquires are met
  right after the drain and about 12,000 checks find the last release already executed without
  meeting the value yet. The front waits for values the back has not produced. Getting overlap
  back needs a change to how the split synchronizes (e.g. suspending the channel instead of
  draining, which `PendingAcquire` already does for unmet acquires), not a skip.

## Visual bugs

- **White flicker when the window is not focused:** not reproduced. With Notepad in front, the
  presented-image flash watch saw 0 flashes in 3,600 frames, and the image was clean. Background
  auto-pause and auto-mute are off. The process already opts out of Windows' background timer
  throttling (`SetHighQoS`), so that is not it either. Next time: launch with
  `EDEN_NX2_FLASH_WATCH=<dir>` and reproduce; the report names the first abnormal pass.
- **Distant trees flickering/darkening:** with a still camera, the flash watch on every pass sees
  0 flashes in 60 s, with or without DLSS jitter. While walking and turning, the first abnormal
  output of most detected flashes is a full-screen one-draw `R32_FLOAT` 2560x1440 pass (shaders
  `93ce19b8882d2d6b` / `3b3abaa80bda4ddc`, probably AO or a shadow mask), but camera motion also
  changes content legitimately. Needs the owner to show where it happens.

## DLSS Frame Generation: blocked on binaries

- **Missing files:** our Streamline folder (`%APPDATA%\eden\streamline`) has `sl.interposer.dll`
  and the NVIDIA app's `nvngx_dlss.dll` only. DLSS-G needs `sl.dlss_g.dll`, `sl.reflex.dll`,
  `sl.pcl.dll` and `nvngx_dlssg.dll` from the Streamline 2.14.16 SDK release (NVIDIA's GitHub).
  Today's log also shows `NvLowLatencyVk.dll` failing to load from the nvidia-app folder.
  Downloading them needs the owner's OK.
- **Plan** (see `dlss-roadmap.md`):
  - the emulator already has a frame-generation hook in `RendererVulkan::Composite` (LSFG,
    `present_manager.MaxExtraFrames()`), which shows where generated frames go;
  - DLSS-G instead takes over presentation through Streamline's proxies (manual hooking of the
    swapchain calls), needs Reflex markers per frame and an HDR10 PQ swapchain while it is on;
  - the HUD-less colour is the tonemapped image the network replacement already knows
    (`OotDlss::Targets().tonemapped_gpu`), and the camera matrices come from `CaptureOotCamera`.

### Frametime spikes, continued

- **Front thread in hitch lines.** The GPU (front) thread's zones are now in the hitch line
  (`PublishFrontZones`). In a hitch frame the front does about one normal frame's worth of work:
  split drain ~10-15 ms, macro ~5 ms, resume pending ~4 ms, as at its usual per-second rates. It
  is not stuck. Back, front and guest cores are all partly idle.
- **Hitch durations are quantized:** ~32-38 ms and ~47-60 ms, i.e. 2x and 3x 16.7 ms. The game
  waits for a 60 Hz tick when a frame runs long.
- **What does not change it:**
  - `EDEN_NX2_VSYNC_HZ=120` with `EDEN_NX2_PRESENT_HZ=60`, and both at 240: same hitch counts and
    durations. The emulated vblank and the present hold are not the quantizer.
  - `EDEN_NX2_PERF_OFF=frames_ahead`: same.
- **Conclusion:** the 60 Hz rhythm is the game's own (an internal timer or wait). At ~45 fps it
  misses often. The cure is throughput (frames done in time), i.e. the back thread (~76% busy)
  and the front/back synchronization.
- **Next:** a profiling build with symbols (`tuned/build-prof.cmd` → `build\zen5-prof`, same
  optimization plus `/Z7`, PDB) so `EDEN_NX2_SAMPLE_PROFILE` + `tools/nx2/sample_report.py` can
  show which functions the back and front threads spend their time in.
