# AMD FidelityFX SDK subset (FSR 3.1.4 upscaler + frame interpolation, Vulkan)

- Source: https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK
- Tag: `v1.1.4`, commit `c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`
- FSR upscaler version: 3.1.4
- License: MIT, see `LICENSE.txt` (shipped in the Linux installer as `licenses/fidelityfx/LICENSE.txt`)

`include/` and `src/` hold only the files the FSR3 upscaler, frame interpolation, optical flow and the
native Vulkan backend need, at their upstream relative paths and byte-identical to the tag except the
local patches below. The SDK's prebuilt shader blobs, `blob_accessors/` and `ffx_shader_blobs.cpp` are not
vendored: CMake compiles the GLSL passes in `src/backends/vk/shaders/{fsr3upscaler,frameinterpolation,opticalflow}/`
and `tools/embed_fsr_shaders.py` generates the blob accessor. Only the permutations the clamped
capabilities can request are built: upscaler HDR × sharpening (reference Lanczos, render-resolution
unjittered motion vectors, standard depth), one frame-interpolation permutation (render-resolution
unjittered motion vectors, standard depth) and one optical-flow permutation (SDR input), all FP32 and
wave-size agnostic. Any other request fails loudly. The Win32-only `FrameInterpolationSwapchain` and the
`fsr3` wrapper component are not vendored; the engine dispatches optical flow/frame interpolation and
presents generated frames itself.

Local glue (not upstream):

- `yami/ffx_compat.h` — force-included into every `yami_fsr3` source: MSVC CRT shims for GCC/Clang and
  redirection of the backend's direct `vk*` calls to runtime-resolved pointers.
- `yami/yami_ffx_vulkan.h`, `yami/yami_ffx_vulkan.cpp` — `yami_ffx_load_vulkan()`, which resolves those
  pointers through `vkGetInstanceProcAddr` (the game never links libvulkan);
  `yami_ffx_clamp_capabilities()`, which makes every backend interface report wave32, no FP16, no
  synchronization2, no buffer markers and no device-coherent memory, so the SDK requests only the
  generated permutations and features the game's device enables; `yami_ffx_device_proc()`, the
  `VkDeviceContext::vkDeviceProcAddr` that maps the backend's `vkGetBufferMemoryRequirements2KHR` alias
  to the Vulkan 1.1 core entry point.
- `yami/FidelityFX/host/ffx_types.h` — shadows the upstream header to size the opaque contexts for the
  platform's `wchar_t`.

Local patches to upstream source (each marked `yami patch`):

1. `src/backends/vk/ffx_vk.cpp`, `BackendContext_VK::EffectContext` drops `alignas(32)`. The backend
   carves that array from its scratch buffer at 8-byte offsets, so GCC's aligned SSE stores into it
   segfaulted in `CreateBackendContextVK` (MSVC happens to emit unaligned stores).
2. `src/backends/vk/ffx_vk.cpp`, `findMemoryTypeIndex` skips `VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD`
   types unless requested. Upstream picked the last device-local type, which on RADV is the AMD
   device-coherent/uncached one: invalid without the `deviceCoherentMemory` feature and uncached for
   every FSR resource.
3. `src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`: a failed `ffxFsr3UpscalerContextCreate` released
   nothing, leaking the backend context, resources and pipelines it had created. `context->device` is
   now set only once the backend context exists and the create calls the idempotent
   `fsr3upscalerRelease` on failure.
4. `include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`: `rw_luma_history` format
   qualifier `rgba8` → `rgba16f`, the format of the luma-history resources (R16G16B16A16_FLOAT). The
   mismatch made every luma-history load/store undefined in Vulkan, breaking FSR's luma-instability
   (anti-flicker) detection.
5. `src/backends/vk/ffx_vk.cpp`, `CreateBackendContextVK`: the descriptor pool lists 6 pool sizes but
   passed `poolSizeCount = 5`, dropping `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER`, which frame interpolation's
   counters use. Now `_countof(poolSizes)`.
6. `include/FidelityFX/gpu/frameinterpolation/ffx_frameinterpolation_callbacks_glsl.h`,
   `LoadOpticalFlowConfidence`: `ffxFrameInterpolationDispatch` never registers an optical-flow
   confidence resource, so `r_optical_flow_confidence` was never written (D3D12 reads its null SRV as 0).
   Returns 0 without declaring the texture; its only caller computes a weight it never uses.
7. Frame interpolation takes an optional **shading-change mask** and **midpoint selector**:
   `FfxFrameInterpolationDispatchDescription::shadingChange` and `midpointSelect`
   (`include/FidelityFX/host/ffx_frameinterpolation.h`), resource identifiers `SHADING_CHANGE` with a 1×1 zero
   `DEFAULT_SHADING_CHANGE` and `MIDPOINT_SELECT` with a 1×1 one `DEFAULT_MIDPOINT_SELECT`
   (`ffx_frameinterpolation_resources.h`), registered like the distortion field in
   `src/components/frameinterpolation/ffx_frameinterpolation.cpp` (SRV names `r_shading_change` and
   `r_midpoint_select`, defaults when null), bound at bindings 11 and 12 of `ffx_frameinterpolation_pass.glsl` only
   and read by `LoadShadingChange` (maximum over the 6×6 render texels around a UV: the 2×2 bilinear footprint
   dilated by two texels) and `LoadMidpointSelect` (bilinear, as nested `a + (b − a)·f` so equal texels return
   themselves exactly) in `ffx_frameinterpolation_callbacks_glsl.h`. In `computeInterpolatedColor`
   (`ffx_frameinterpolation.h`), where the current game-vector sample is valid (`fCurrColorGame.fBilinearWeightSum > 0`,
   `fDisocclusionFactor.y == 1`), `w = smoothstep(0, 0.3, mask at uv − gameMotion)`; when `w > 0`, after the upstream
   game-vector colour (including its one-sided fallbacks) it becomes `lerp(gameColour, lerp(fPrevColorGame,
   fCurrColorGame, s), w)` with `s` the selector at the same `uv − gameMotion` (forced to 1 where the previous
   sample is outside its frame or disoccluded), and `fGameMvBias = lerp(fGameMvBias, 1, w)` before the
   optical-flow/game-vector blend. Where a surface's shading changed between the two real frames (a shadow moving
   over it, including a walking character's own body), the game vector still places the surface, but blending its
   two shadings doubles the shadow and the optical-flow fallback tears small moving objects; each such point instead
   takes the real frame whose state it has halfway between them, reusing rendered pixels as they are (without a
   selector: this frame's, so such shadows step at the real-frame cadence). With a zero mask the output is exactly
   upstream. This replaces an earlier patch that raised the game-vector bias exponent from 1 to 8, which made the
   optical-flow path win on shaded moving characters and tore them.
