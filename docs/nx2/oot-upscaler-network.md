# Ocarina of Time upscaler network (findings so far)

Captured in Kokiri Forest, docked, emulator resolution 1x, with
`EDEN_NX2_COMPUTE_HANDLE_TRACE=1` and a sampled `capture_frame`.

With the built-in Max Resolution patch the main passes depth-test against a 2560x1440 D32
buffer, so the 3D most likely renders at 1440p and the network works like DLAA at that size; the
1280x720 RGBA16F input is a half-resolution effects layer (fireflies, particles). The network
runs before bloom, tonemapping and the HUD. Its passes (handles in constant buffer 0; TIC indices
rotate every frame, the offsets do not):

| Pass | Native key / shader | Launch | Handles (cb0 offset: image) |
|---|---|---|---|
| Prepass | `8475d671e6967428` / `a107e12b6771be48` | 80x60, block 192 | +130 color R16G16B16A16F 1280x720; +148 history B10G11R11 2560x1440; +150 R16G16 UINT 640x360; +1fc B10G11R11 1280x720 |
| Network | `42593194bb0b2b32` / `a2fcaa95c5a162f3` | 160x90, block 128 | +090 color; +0a8 history; +0b0/+0c0 R16G16 UINT 640x360; +0b8 A8B8G8R8 2560x1440 and +0c8 R32 UINT view of it; +0d0 R32 UINT 2560x1440 (RGB9E5 output); +0fc B10G11R11 1280x720 |
| Output | `da6df3e54c716df2` / `6bd913e1f34e83eb` | 64x36, block 128 | +0c0 E5B9G9R9 2560x1440 (network result); +0c8 R16G16 UINT 640x360; +0d0 B10G11R11 2560x1440 (scene output used by the rest of the frame); +0fc B10G11R11 1280x720 |
| (after) | `d897749a07b5f1c3` / `fc10c1232f7ac3f3` | 9x5, block 16x16 | +030 Z32 2560x1440 |

A fourth heavy dispatch (shader `d9ee412f00ce399a`, grid 1452, block 96) sits between the
prepass and the network and is not seen by the trace yet.

No per-frame float changes in the network's cb0, so the camera jitter (and possibly the
motion vectors) come from another constant buffer bank, a storage buffer, or the 640x360
R16G16 UINT buffer. Open questions before a DLSS replacement: where motion vectors, depth at
render resolution and jitter live; what the 1280x720 B10G11R11 image is (reactive/mask?);
and how the history resets on camera cuts.

## Resolution scaling

`EDEN_RES_SETUP=5` (1.5x) renders and is visibly sharper, but the scene is darker with less fog
and glow than at 1x, and Kokiri Forest drops from about 41 to 36 fps. Some lighting/effects
buffer or the exposure pass does not scale consistently yet.
