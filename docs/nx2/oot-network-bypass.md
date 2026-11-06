# Ocarina of Time: replacing the game's anti-aliasing network

Notes for a pull request that adds the "Ocarina of Time anti-aliasing" setting
(`oot_anti_aliasing`: Game / No sharpening / NVIDIA DLSS). Program id `0400486024DEC000`.

## What the game does

Investigated with frame dumps (`capture_frame` with `sampled`), `EDEN_NX2_COMPUTE_HANDLE_TRACE`
and pixel statistics; details in `docs/nx2/oot-upscaler-network.md`.

- The 3D renders at the output size (2560x1440 docked with the built-in Max Resolution patch),
  with no jitter and no motion vectors.
- After the tonemap pass (fragment shader `bded5ce917183214`, or `6cb06edf14be1d79` indoors) the
  game runs a tensor network on the **tonemapped** image: prepass `8475d671e6967428`, a heavy pass
  (shader `d9ee412f00ce399a`), the network `42593194bb0b2b32` and an output pass
  `da6df3e54c716df2` (native program keys).
- It keeps no history: the network is a single-frame spatial smoother (mean |Laplacian| of green
  0.0042 -> 0.0037) and the output pass then sharpens hard (2.2x the input's edge contrast,
  overshoot up to 1.72 on bright edges). That sharpening is the "filter" users see.
- Only the HUD and a plain sRGB copy follow the output pass.

## The change

In `ShaderCache::GetComputePipeline`'s native path (`src/video_core/shader_cache.cpp`), before the
game-DLSS check, `KeplerCompute::ReplaceOotNetwork` (`src/video_core/engines/kepler_compute.cpp`)
throws `DispatchReplaced` for the prepass and the network, and at the output pass:

- **No sharpening:** blits the tonemapped image into the output pass's target with
  `AccelerateSurfaceCopy` (point filter, same size, format B10G11R11 and block-linear layout,
  taken from the output pass's own texture descriptor at cb0+0xd0).
- **NVIDIA DLSS:** `AccelerateOotDlss` (see below); falls back to the copy.

The images are learned at run time, not hard-coded:

- tonemapped input: render target 0 of the draw whose pipeline has a tonemap fragment shader
  (`RasterizerVulkan::PrepareDraw`, `OotDlss::IsTonemapShader`);
- output and scene depth: texture handles of the output pass (cb0+0xd0) and of the pass that
  follows it (`d897749a07b5f1c3`, cb0+0x30, a Z32 descriptor kept in shared state because the
  passes run on different compute engines).

Nothing is replaced until the tonemapped image has been seen, so menus and the first frame
keep the native path.

## NVIDIA DLSS mode

- **Jitter:** `RasterizerVulkan::UpdateViewportsState` adds a 16-phase Halton (2, 3) sub-pixel
  offset to the viewport of every draw into a Z32 depth target of the scene depth's size. The game
  depth-tests its colour passes against a second depth of that size, so keying on the learned
  depth address alone broke them. The viewport is re-sent whenever the applied jitter changes.
- **Motion vectors:** `host_shaders/oot_motion_vectors.comp` (`vk_oot_motion_vectors.cpp`)
  unprojects each pixel's depth (GL style: NDC z = 2d - 1, not reversed) and reprojects it with
  `prevViewProj * inverse(viewProj)`, read from the vertex stage's cbuf3 (+0x30 and +0x180) at the
  frame's first scene draw (`RasterizerVulkan::CaptureOotCamera`). Camera motion only.
- **DLSS:** `TextureCacheRuntime::OotDlss` runs `StreamlineDlssUpscaler` with LDR colour, no auto
  exposure and `depthInverted = false` (new `Inputs` fields; Fire Emblem's defaults unchanged).

## Results (Ryzen 7 9800X3D, RTX 5090, Kokiri Forest, 1x)

| Mode | fps | Image |
|---|---|---|
| Game | ~41 | sharpened, halos |
| No sharpening | ~50 | clean, aliased edges; pairs with resolution scaling |
| NVIDIA DLSS | ~42-46 | clean, anti-aliased, no ghosting on camera pans |

## Caveats before upstreaming

- Program keys and cb0 offsets are specific to this game build (main
  `E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348`); a game update would need them re-checked.
- B10G11R11 has little precision in dark tones: without the network's smoothing some banding
  shows in deep shadows.
- Moving objects have no motion vectors yet (camera only).
- `EDEN_OOT_NO_SHARPEN`, `EDEN_OOT_DLSS`, `EDEN_OOT_DLSS_JITTER`, `EDEN_OOT_DLSS_JITTER_SIGN` and
  `EDEN_OOT_DLSS_FLIP_Y` are diagnostic overrides; per AGENTS.md, experiment switches should not
  go upstream as-is.
