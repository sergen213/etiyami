# Graphics options

## Graphics selection

The launcher has an **OpenGL / Vulkan** selector before Play. **Gelişmiş seçenekler** opens ray tracing, **FSR 3.1**, **Gölge gücü** (shadow strength, default **0.75**), indirect lighting, exposure, render scale (FSR mode), global roughness and **Kare üretimi** (FSR 3.1.4 frame generation) controls; **OpenGL / Genel** returns to the shared controls. Shadow strength is shared: enhanced OpenGL/GLES casts character shadows, while supported Vulkan RT uses ray-query shadows. The other advanced controls are Vulkan-only and stay saved but dormant in OpenGL/GLES. OpenGL is the default; `--gles` retains the GLES alternative. Backend selection is launcher-only, not a live in-game switch.

The launcher preview always uses the retained OpenGL path (GLES with `--gles`), never Vulkan. Play creates a fresh window/renderer and scene resources for the selected gameplay backend. If Vulkan initialization fails, the launcher returns with an explanation and lets you choose OpenGL; it does **not** silently run gameplay on another backend. GL-only builds still open the launcher and allow a saved Vulkan preference to be changed to OpenGL.

Vulkan requires a compatible **Vulkan 1.3** driver/device, but hardware RT is optional. Enhanced Vulkan rendering includes linear-HDR world lighting and sky, exposure/tone mapping, bloom, temporal lighting denoising, and **AMD FSR 3.1.4** anti-aliasing/upscaling of the world artwork, with optional frame generation on supporting devices. The render-scale row selects the FSR mode: **Native AA** (100%), **Kalite** (67%), **Dengeli** (59%) or **Performans** (50%); the HUD always stays native. With FSR off, reduced render scale uses the plain native-resolution reconstruction. Supported RT devices add ray-query shadows, ambient occlusion, reflections, and one-bounce indirect lighting. RT off or unavailable uses raster rendering with screen-space AO/reflections; RT shadows and indirect lighting are then inactive. Classic graphics bypasses these enhancements, FSR, and reduced-resolution rendering, retaining native-resolution legacy lighting/colour.

Enhanced OpenGL/GLES character shadows follow animated, alpha-cutout actor geometry within a near-gameplay directional depth map; this is not ray tracing or a full-level scenery shadow map. Original alpha/fog compositing is retained; zero shadow strength or classic graphics bypasses them.

The original assets have no authored roughness/metalness maps: roughness is a conservative global dielectric treatment, not replacement PBR artwork. This is hybrid raster/RT, not full path tracing. No 1440p/60-fps performance guarantee or physical ARM64 GPU verification is claimed.

Enhanced-only **parallax occlusion mapping** is enabled by default and can be toggled with `--parallax on|off`. Static opaque primary world materials derive separate relief textures from diffuse luminance (8–24 occlusion steps plus one secant refinement); original assets are not modified. Relief affects primary artwork and shading normals only: silhouettes, geometry, depth, motion data, alpha hits and secondary RT intersections remain flat. Actors, HUD, videos, blended/alpha-tested surfaces and classic rendering do not use it.

**Day/night environment:** Cycle is the default and completes a day → night → day transition in 12 minutes of unpaused gameplay. Day keeps the original lighting; Night darkens the world, uses blue night sky/fog, disables directional shadows, and lights the original `lamba_001` material with warm emission. Traffic heads cycle **red for 4 seconds → yellow for 1 second → green for 4 seconds**. Only the active lens supplies colored Vulkan RT spill; the yellow backplate and the whole traffic assembly do not glow. Stable pseudo-random occupancy lights about one third of building windows; blue-glass regions glow warmly while neutral frames, mullions, walls, and other windows stay dark. Vulkan RT adds downward warm ground pools from street-pole and hanging fixtures, colored local illumination from the active traffic lens, plus occupied window panes, weapon glow, brief muzzle flashes, and lights that follow projectiles. These lights illuminate nearby geometry; they are not merely brighter textures. OpenGL/GLES retains the emissive-only presentation. Level loads continue the same cycle phase; menus pause it, and restarting the executable starts at daylight. Classic graphics always retains the original daylight presentation without discarding the saved time preference. `--time-of-day cycle|day|night` selects the mode directly.


## Useful launch options

Examples below use the downloaded executables; for a source build, prefix them with `./build/` instead of `./`.

```sh
# Direct gameplay, bypassing the settings launcher
./yami-native --skip-intro

# Use GLES instead of the default OpenGL backend
./yami-launcher --gles

# Original lighting without the enhanced world effects
./yami-launcher --classic-graphics

# Lower-cost OpenGL graphics settings
./yami-launcher --renderer opengl --samples 0 --anisotropy 1 --ao 0 --reflections 0 --bloom 0 --sharpen 0

# Print the installed version or all supported options
./yami-native --version
./yami-native --help
```

Effect strengths accept values from `0` to `1`; zero disables that effect. CLI graphics options override saved values, while edits made in the launcher take precedence before Play. `yami-native --launcher` opens the same launcher interface.

```sh
# Select Vulkan before Play, with optional hardware RT
./yami-launcher --renderer vulkan --ray-tracing on

# Vulkan raster path, reduced world resolution, native-resolution HUD
./yami-launcher --renderer vulkan --ray-tracing off --render-scale 0.667 --taa on

# Explicitly retain OpenGL, or GLES, regardless of a saved Vulkan preference
./yami-launcher --renderer opengl
./yami-launcher --renderer opengl --gles
```

| Graphics option | Values / effect |
|---|---|
| `--renderer` | `opengl` or `vulkan`; default OpenGL unless saved otherwise |
| `--ray-tracing`, `--taa` | `on` / `off`; Vulkan only, enabled by default; `--taa` toggles FSR 3.1 |
| `--parallax` | `on` / `off`; enhanced-only generated relief on eligible static world materials, default on |
| `--time-of-day` | `cycle`, `day`, or `night`; enhanced world cycle, fixed original daylight, or fixed night with emissive lamps; default `cycle` |
| `--frame-generation` | `on` / `off`; Vulkan FSR 3.1.4 frame generation, default off; needs FSR on and a supporting GPU |
| `--gpu-timings` | Vulkan only, not saved; records GPU timestamps per frame stage (world raster, acceleration builds, RT classify/effects/Sun, temporal, FSR inputs/dispatch, display, composite, frame generation, UI, present blit) plus CPU wall time from frame start to submit (`cpu`) and between consecutive real-frame presents (`interval`), and prints count/mean/median/p95 ms plus per-frame BLAS/TLAS/instance counters to stderr on exit |
| `--shadows` | `0..1`; shared enhanced GL/GLES character-shadow / Vulkan RT shadow strength, default `0.75`; zero disables shadows |
| `--gi` | `0..1`; Vulkan RT one-bounce indirect strength, default `0.35`; inactive with RT off/unavailable |
| `--exposure` | `0.1..4`; Vulkan HDR exposure, default `1` |
| `--render-scale` | `0.5..1`; Vulkan enhanced world resolution, default `1`; FSR presets `1` Native AA, `.667` Kalite, `.588` Dengeli, `.5` Performans; HUD stays native |
| `--roughness` | `0.05..1`; Vulkan global dielectric roughness, default `0.85` |
| `--ao`, `--reflections`, `--bloom` | `0..1`; shared settings, with backend-specific rendering |
| `--sharpen` | `0..1`; OpenGL/GLES sharpening; on Vulkan it drives FSR 3.1 sharpening (RCAS, `0` disables) |
| `--classic-graphics` | Disable enhanced world processing without discarding saved strengths |

Numeric options reject nonfinite and out-of-range inputs. Ray tracing, FSR, indirect lighting, exposure, render scale, and roughness controls are annotated as dormant in OpenGL/GLES; shadow strength remains active in enhanced graphics. **Keskinlik** applies in both backends: OpenGL/GLES sharpening, or FSR sharpening on Vulkan. Do not reset preferences to change backends. `--ray-tracing on` is a preference, not a claim that the GPU supports RT. FSR needs Vulkan formatless storage-image writes and compute quad subgroup operations; the device log prints `FSR 3.1 available` or `unavailable`, and unsupported devices render without FSR.

