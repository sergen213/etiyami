# ETI Yami Native — Linux

A native C++20/SDL3 reconstruction of **ETI Yami**, using the original game's artwork, models, scripts, audio, and videos. The **public v1.1.0 release** includes an original-style settings launcher, retained OpenGL/GLES rendering with enhanced character shadows, selectable Vulkan with optional hardware ray tracing, widescreen support, and automatic launcher/engine updates. No Wine, DirectX installer, or Windows codec installation is needed.

**The original game assets are not included in this repository or release downloads. You need a legitimate copy of the original game.** Linux releases are available for x86_64 and ARM64; Windows/macOS releases are deferred.

Installer-created uninstall scripts and shortcuts require **installer 1.0.3 or newer**, available in [the public v1.0.3 release](https://github.com/sergen213/etiyami/releases/tag/v1.0.3); the 1.0.2 installer does not provide them.

Installer **1.0.4** fixes the Linux portal-picker freeze affecting **Choose ISO**, **Change...**, and picker cancellation/error responses. The corrected AppImage and `.run` installers are publicly available in [v1.0.4](https://github.com/sergen213/etiyami/releases/tag/v1.0.4), published from source commit `d0d7d8209e40a130a7890a6a1f47966a07149d31`: all four jobs in [run 37566309522](https://github.com/sergen213/etiyami/actions/runs/37566309522) passed native Ubuntu 22.04 x86_64/ARM64 builds, glibc-2.35 packaging, and installer self-tests. This CI package proof is separate from the local source GUI checks below, not a public GUI installation or live portal-popup test. Redownload the corrected installer if you have an older copy; the game's automatic launcher/engine update does **not** replace an old downloaded installer.

**v1.1.0 is publicly available:** [the stable graphics release](https://github.com/sergen213/etiyami/releases/tag/v1.1.0) passed native Ubuntu 22.04 x86_64/ARM64 builds and six-asset digest-gated publication in [run 37699716224](https://github.com/sergen213/etiyami/actions/runs/37699716224). Anonymous downloads of all six assets matched GitHub SHA256; the downloaded x86_64 AppImage installed the original ISO and the installed game passed real OpenGL/GLES and Vulkan RT-on/off gameplay checks. ARM64 has native CI/package proof, not physical GPU execution. The latest/download links below now resolve v1.1.0. OpenGL remains the default and the choice for older hardware; Vulkan and hardware RT are optional.

**v1.1.1 is an unreleased source candidate**, not the latest public download. It revises Vulkan artwork/illumination history and ray-position reconstruction, rotates the existing eight TAA sample locations over a 64-frame cycle, and makes Desktop shortcuts optional while preserving Apps / application-menu installation. Source verification is complete for the documented scope: full RT-on/off GPU checks, **17/17 Vulkan-enabled CTests**, **15/15 retained GL-only CTests**, all four actual-game Vulkan levels and half-scale RT-off, including inspected level-2 64-frame frozen/animated/moving captures. Native release CI, downloaded-package and publication verification remain pending. The v1.1.0 public-package evidence below is separate from candidate source verification.

[Install](#install-on-linux-recommended-appimage) · [Uninstall](#uninstall-an-installer-created-game) · [Manual ZIP](#advanced-manual-zip-installation) · [Controls](#game-controls) · [Saves](#saves-and-settings) · [Updates](#automatic-updates) · [Source build](#build-from-source) · [Troubleshooting](#troubleshooting) · [Checks](#checks-and-technical-details)

## Install on Linux (recommended: AppImage)

Download the installer for your CPU from [the latest public release](https://github.com/sergen213/etiyami/releases/latest). No GitHub account or token is required.

| Computer | Recommended installer | Shell fallback | Advanced manual package |
|---|---|---|---|
| Intel/AMD 64-bit Linux | [x86_64 AppImage](https://github.com/sergen213/etiyami/releases/latest/download/yami-setup-linux-x86_64.AppImage) | [x86_64 .run](https://github.com/sergen213/etiyami/releases/latest/download/yami-setup-linux-x86_64.run) | [x86_64 ZIP](https://github.com/sergen213/etiyami/releases/latest/download/yami-linux-x86_64.zip) |
| 64-bit ARM Linux | [ARM64 AppImage](https://github.com/sergen213/etiyami/releases/latest/download/yami-setup-linux-arm64.AppImage) | [ARM64 .run](https://github.com/sergen213/etiyami/releases/latest/download/yami-setup-linux-arm64.run) | [ARM64 ZIP](https://github.com/sergen213/etiyami/releases/latest/download/yami-linux-arm64.zip) |

Requirements: **x86_64 or ARM64 Linux with glibc 2.35 or newer**, your distribution's compatible graphics drivers and C++ runtime, and an OpenGL 3.3 or GLES 3-capable desktop. Installers bundle application libraries and the offline ISO extraction tool, **not** glibc, `libstdc++`, `libgcc`, or graphics drivers. Standard Linux paths such as `/bin/sh` and `/usr/bin/env` and ordinary per-user XDG application-menu support are required. This is not universal distribution support: Alpine/musl and non-FHS systems are not supported by these binaries merely because they can open an AppImage. No Wine, root access, separately installed 7-Zip, or manual asset extraction is needed.

Published x86_64 and ARM64 packages have native Ubuntu 22.04 CI build and static architecture/runtime-policy verification. Physical GPU/gameplay verification has been performed on x86_64 Linux; ARM64 CI/package checks do **not** establish physical ARM GPU compatibility or verification across every desktop/distribution.

### 1. Run the installer

For an x86_64 download saved in `~/Downloads`:

```sh
chmod +x "$HOME/Downloads/yami-setup-linux-x86_64.AppImage"
"$HOME/Downloads/yami-setup-linux-x86_64.AppImage"
```

For ARM64, substitute `arm64` for `x86_64`. The AppImage uses a static runtime with automatic extraction fallback when FUSE is unavailable; it does not require installing FUSE2. To explicitly run without FUSE:

```sh
"$HOME/Downloads/yami-setup-linux-x86_64.AppImage" --appimage-extract-and-run
```

Alternatively, the `.run` download opens the same installer:

```sh
chmod +x "$HOME/Downloads/yami-setup-linux-x86_64.run"
"$HOME/Downloads/yami-setup-linux-x86_64.run"
```

The `.run` wrapper needs the ordinary shell, `tar`, `gzip`, and coreutils supplied by standard Linux installations. Run either format as your normal desktop user, **never with sudo**.

### 2. Choose your ISO, install, and launch

1. Click **Choose ISO** and select your legally owned original Turkish Yami game ISO. The installer reads its MSI/CAB data natively; it never executes the disc's Windows setup or codec/DirectX installers.
2. Wait for the original artwork and bitmap font to load from that ISO into a private temporary preview.
3. Keep the default installation location, `${XDG_DATA_HOME:-$HOME/.local/share}/etiyami`, or click **Change...** to select a parent folder; the installer creates `etiyami` inside it.
4. Click **Install game**, then **Launch game**. Subsequently open **ETI Yami** from **Apps / the application menu**, which is the primary launcher path. **Uninstall ETI Yami** is registered there too, using the original game icon.
5. Desktop files and their visibility depend on your desktop environment: some show no Desktop icons; others require you to trust/allow launching a file. The installer does not enable icons, install desktop extensions, or change your desktop settings. **Unreleased v1.1.1 candidate change:** Desktop play/uninstall `.desktop` files are secondary and published only when a usable location is available; a missing/disabled/unusable Desktop or an unrelated Desktop-file collision can be skipped while completing the core installation and application-menu repair. The published v1.1.0 installer can still refuse an unavailable Desktop; this fix is not included in the linked downloads.

A `.desktop` file is application-launch metadata, **not a shell script**: select the application in Apps, or open its file with your desktop's launcher support; do not run `sh eti-yami.desktop`.

The ISO can remain anywhere you can read it; you do **not** need to move it beside the installer or game. It is **not needed after installation**. Installation works offline once you have downloaded the installer. Allow 640 MiB free temporary space for the artwork preview and 1.5 GiB plus the native engine on the installation volume for extraction/staging.

**Tab / Shift+Tab** moves between installer controls, **Enter / Space** activates them, and **Escape** cancels a preview/installation or closes an idle installer. Cancellation cleans private staging rather than publishing a partial fresh installation.

Running Install again against an inventory-bearing installer-owned installation preserves its existing assets, engine, settings, and saves and repairs its application-menu entries, Desktop files, and uninstall support; it is not an engine downgrade/replacement. An older installer-owned installation without ownership inventories needs the original ISO once to establish which assets belong to the installer; files without proven ownership are kept. Unrelated folders, symlink destinations, and unrelated shortcuts are never overwritten. Core/application-menu conflicts fail safely. **In the unreleased v1.1.1 candidate**, optional Desktop failures that safely restore the previous state do not prevent installation; incomplete rollback instead fails with retained recovery backups and a manual-recovery error. Published v1.1.0 installers still require Desktop publication. Use the launcher's automatic updates for new engine versions.

An older launcher can reject graphics settings saved by a newer engine with `Unknown native graphics field`. If it cannot open to update itself, extract the matching newer release's **entire platform ZIP** into the existing installation directory, keeping its libraries beside the executables. Keep `game/`, saves, settings, and shortcuts; do not reset preferences to work around the version mismatch.

### 3. Play

Launch opens the settings launcher. Choose **OYNA / PLAY**, then **Yeni Oyun / New Game** in the game's main menu. Subsequently use **ETI Yami** in Apps / the application menu (or a Desktop file where supported); the downloaded installer is no longer required.

## Uninstall an installer-created game

Close the game, launcher, and updater, then open **Uninstall ETI Yami** from **Apps / the application menu** (or its optional Desktop file where supported). This shortcut runs the installed `uninstall.sh` in your desktop's terminal, not another AppImage or a graphical uninstaller. The script invokes a small private native removal helper using only the host C/C++ runtime.

The terminal shows the exact installation path and asks for confirmation; answering no or pressing Enter cancels without removing files. After confirmation, only files and shortcuts recorded as installer-owned are removed. Saves, settings, custom files, and any other unknown/unproven files are kept. Only empty recorded child directories are pruned; the installation root itself is always kept, even when empty, and any remaining files are reported.

Neither the original ISO, the downloaded installer, an Internet connection, nor sudo is needed. You can also run the installed script from an existing terminal:

```sh
/bin/sh "${XDG_DATA_HOME:-$HOME/.local/share}/etiyami/uninstall.sh"
```

For a custom location, use that installation's exact `uninstall.sh` path. A manual ZIP installation does not gain uninstall ownership records or these shortcuts automatically. For an older installer installation, rerun the new installer with your original ISO first; missing or invalid ownership records cause removal to refuse rather than guess.

## Advanced: manual ZIP installation

Use the ZIP only if you already have an extracted original asset tree or want to manage the installation yourself.

Extract the **entire ZIP**, keeping its libraries beside the executables. Use a directory you can write to so automatic updates can install without administrator access. For an x86_64 download saved in `~/Downloads`:

```sh
mkdir -p "$HOME/Games/etiyami"
unzip "$HOME/Downloads/yami-linux-x86_64.zip" -d "$HOME/Games/etiyami"
cd "$HOME/Games/etiyami"
```

For ARM64, substitute `yami-linux-arm64.zip` in the extraction command.

### 2. Supply the original assets

Copy your original game's complete `game/` directory beside the executables. The layout should include:

```text
etiyami/
├── yami-launcher
├── yami-native
├── yami-updater
├── lib*.so*                  # Keep all supplied application libraries
└── game/
    └── data/
        ├── menu/menulist.xml
        └── ...               # All other original data, not just the menu
```

Alternatively, leave the original files where they are and pass `--asset-root` when launching. The asset root is the folder **containing `data/`**, not `data/` itself:

```sh
./yami-launcher --asset-root "/path/to/original/game"
```

See [extracting from the original installer](#extracting-from-the-original-installer) below if you have the MSI/CAB rather than an extracted game directory.

### 3. Start the launcher

```sh
./yami-launcher
```

Choose your settings, then click **OYNA / PLAY**. In the game's main menu, choose **Yeni Oyun / New Game** to start.

The v1.1.0 launcher retains the 13 shared settings from release 1.0.4 and the in-game **Ayarlar / Settings** screen: brightness, sensitivity, aiming mode, music/effects volume, lighting, ambient occlusion, reflections, bloom, sharpening, MSAA, texture filtering, and fullscreen. It stays windowed while you configure settings; the saved fullscreen choice applies when you press Play. In the unreleased v1.1.1 candidate, sharpening applies only to OpenGL/GLES; its saved preference is not reset when Vulkan is selected.

Keyboard navigation: **Tab / Shift+Tab** selects a control, **arrow keys** adjust it, **Enter / Space** activates it, and **Escape** closes the launcher. Mouse buttons apply changes on release. Closing saves settings without starting the game.

![Original-style settings launcher](analysis/native-launcher-github-updated.png)

### Graphics selection in v1.1.0

The v1.1.0 launcher adds an **OpenGL / Vulkan** selector before Play. **Gelişmiş seçenekler** opens ray tracing, TAA, **Gölge gücü** (shadow strength, default **0.75**), indirect lighting, exposure, render scale, and global roughness controls; **OpenGL / Genel** returns to the shared controls. Shadow strength is shared: enhanced OpenGL/GLES casts character shadows, while supported Vulkan RT uses ray-query shadows. The other advanced controls are Vulkan-only and stay saved but dormant in OpenGL/GLES. OpenGL is the default; `--gles` retains the GLES alternative. Backend selection is launcher-only, not a live in-game switch.

The launcher preview always uses the retained OpenGL path (GLES with `--gles`), never Vulkan. Play creates a fresh window/renderer and scene resources for the selected gameplay backend. If Vulkan initialization fails, the launcher returns with an explanation and lets you choose OpenGL; it does **not** silently run gameplay on another backend. GL-only builds still open the launcher and allow a saved Vulkan preference to be changed to OpenGL.

Vulkan requires a compatible **Vulkan 1.3** driver/device, but hardware RT is optional. Enhanced Vulkan rendering includes linear-HDR world lighting and sky, exposure/tone mapping, bloom, temporal denoising/TAA, and 50–100% world render scale with native-resolution reconstruction and HUD. Supported RT devices add ray-query shadows, ambient occlusion, reflections, and one-bounce indirect lighting. RT off or unavailable uses raster rendering with screen-space AO/reflections; RT shadows and indirect lighting are then inactive. Classic graphics bypasses these enhancements, TAA, and reduced-resolution rendering, retaining native-resolution legacy lighting/colour.

Enhanced OpenGL/GLES character shadows are already available in public v1.1.0 and retained in the candidate. They follow animated, alpha-cutout actor geometry within a near-gameplay directional depth map; this is not ray tracing or a full-level scenery shadow map. Original alpha/fog compositing is retained; zero shadow strength or classic graphics bypasses them. The unreleased v1.1.1 Vulkan candidate separates deterministic raster artwork from multiplicative illumination and additive reflection history, with actor-pose reprojection, moving-shadow rejection, dynamic artwork invalidation, ordered untracked-overlay replay and per-sample mixed-coverage history guards. Artist history transports four positively weighted antialiasing errors evaluated at corresponding physical artwork points, not the whole textured base. TAA rotates the same eight sample locations across eight-frame epochs with a 64-frame repeat, without changing sample amplitude; this removes the tested four-pose phase locking, not a guarantee of independence from arbitrary motion. Factorized camera-relative reconstruction stabilizes large-coordinate ray positions. Normal-source GPU/CTest and scoped actual-game verification passed; native release CI, downloaded-package and publication proof remain pending. Existing public v1.1.0 Vulkan RT effects remain available on supported devices.

The original assets have no authored roughness/metalness maps: roughness is a conservative global dielectric treatment, not replacement PBR artwork. This is hybrid raster/RT, not full path tracing. No 1440p/60-fps performance guarantee or physical ARM64 GPU verification is claimed.

## Game controls

| Input | Action |
|---|---|
| W / Up, S / Down | Move forward / backward |
| A / Left, D / Right | Move left / right |
| Mouse | Camera and original menu cursor |
| Ctrl or left mouse button | Fire |
| Space | Special action |
| E or Enter | Interact |
| Tab | Objectives |
| Escape | Game menu |
| F11 | Toggle fullscreen |

Mouse capture is released when the game loses focus.

## Saves and settings

Original assets are read-only. Native checkpoints and `game.ini` are stored separately, using SDL's per-user preference directory. On Linux this is normally `${XDG_DATA_HOME:-$HOME/.local/share}/ETI/YamiNative/`.

To choose a different location:

```sh
./yami-launcher --save-dir "$HOME/Games/etiyami-saves"
```

Keep this directory **outside the original asset tree**. The launcher and game share the same settings file; automatic updates preserve the asset and save-directory choices.

## Automatic updates

Start **`yami-launcher`** to check for new releases in the background. A newer matching Linux package is downloaded over HTTPS, checked against GitHub's SHA256 digest, installed by the trusted local update helper, and the launcher restarts automatically. Updates contain engine/launcher files and application libraries, **not original assets or saves**.

- **No GitHub account, authorization code, or token is required** for public releases.
- Keep all three executables and their supplied libraries together in a writable installation directory.
- Offline operation or an unavailable update leaves the installed game playable.
- Use `./yami-launcher --no-updates` to disable network checks.
- Direct `yami-native` gameplay does **not** check for updates; use the launcher to update.

## Build from source

The AppImage installer is the easiest way to play. To build on Arch Linux / CachyOS:

```sh
sudo pacman -S --needed git gcc cmake ninja pkgconf sdl3 libepoxy libxml2 ffmpeg curl libarchive json-c vulkan-headers glslang python

git clone https://github.com/sergen213/etiyami.git
cd etiyami
```

Supply the original assets as described above, placing `game/` at the repository root, then build and launch:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/yami-launcher
```

For other distributions, install equivalent development packages: a C++20 compiler, CMake 3.20+, Ninja, pkg-config, SDL3, libepoxy, libxml2, FFmpeg (`libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`), libcurl 7.85+, libarchive, and json-c. Vulkan additionally needs Vulkan SDK headers, **glslangValidator or glslc**, and Python 3. Shaders compile to embedded SPIR-V at build time; no external shader files/compiler are needed at runtime. FFmpeg must include the Indeo 5/JPEG/TGA/BMP decoders and PNG encoder. **SDL2 is not a substitute for SDL3.** The Linux release workflow builds its dependencies on Ubuntu 22.04; distribution packages may be too old for a source build.

`YAMI_ENABLE_VULKAN` defaults on when the SDK headers and a shader compiler are detected, otherwise off. Explicitly request it with `-DYAMI_ENABLE_VULKAN=ON` (missing prerequisites are a configuration error), or build without any Vulkan build dependencies:

```sh
cmake -S . -B build-gl -G Ninja -DCMAKE_BUILD_TYPE=Release -DYAMI_ENABLE_VULKAN=OFF
cmake --build build-gl --parallel
./build-gl/yami-launcher
```

Vulkan is loaded dynamically through SDL only when that renderer is created; OpenGL startup does not require a Vulkan loader. The loader and graphics drivers remain distribution-owned runtime dependencies, not bundled application libraries.

### Build the Linux ISO installer

The Linux source build also provides `yami-setup`, the stdlib/POSIX-only `yami-remove`, and a sibling `uninstall.sh`. Building `yami-setup` also builds the removal helper and copies the script beside it. Unlike release installers, a source invocation needs an engine directory containing the three native executables and their required libraries, plus a trusted `7zz` executable:

```sh
cmake --build build --target yami-setup
./build/yami-setup --engine "$PWD/build" --archiver "/absolute/path/to/7zz"
```

Select your original ISO in the GUI; pre-extracting `game/` is not required for this path. Source builds use your installed development/runtime dependencies rather than the release's bundled library set.

The remover defaults to `yami-remove` beside `yami-setup`. If using a different build location, pass `--remover "/absolute/path/to/yami-remove"`; its directory must also contain the matching `uninstall.sh`. Both release installer formats carry this pair offline; the manual engine ZIP remains unchanged.

### Extracting from the original installer

The repository's `extract_game.py` supports the original Turkish Yami MSI/CAB release. Place your legitimate **`Yami.msi`** and **`Data1.cab`** at the repository root, then on Arch/CachyOS run:

```sh
sudo pacman -S --needed python 7zip cabextract
python3 extract_game.py
```

This reads the installer tables and CAB without running a Windows installer, validates the extracted files, and creates `game/`. It refuses to overwrite an existing `game/`. It is specific to this original release, not a general MSI extractor. Installer archives and original data are excluded from Git; do not commit or redistribute them without the appropriate rights.

## Useful launch options

Examples below use the downloaded executables; for a source build, prefix them with `./build/` instead of `./`. Vulkan selection, RT and the advanced graphics options are already available in public v1.1.0; candidate-only behavior is explicitly marked below.

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
./yami-launcher --renderer vulkan --ray-tracing off --render-scale 0.75 --taa on

# Explicitly retain OpenGL, or GLES, regardless of a saved Vulkan preference
./yami-launcher --renderer opengl
./yami-launcher --renderer opengl --gles
```

| Graphics option | Values / effect |
|---|---|
| `--renderer` | `opengl` or `vulkan`; default OpenGL unless saved otherwise |
| `--ray-tracing`, `--taa` | `on` / `off`; Vulkan only, enabled by default |
| `--shadows` | `0..1`; shared enhanced GL/GLES character-shadow / Vulkan RT shadow strength, default `0.75`; zero disables shadows |
| `--gi` | `0..1`; Vulkan RT one-bounce indirect strength, default `0.35`; inactive with RT off/unavailable |
| `--exposure` | `0.1..4`; Vulkan HDR exposure, default `1` |
| `--render-scale` | `0.5..1`; Vulkan enhanced world resolution, default `1`; HUD stays native |
| `--roughness` | `0.05..1`; Vulkan global dielectric roughness, default `0.85` |
| `--ao`, `--reflections`, `--bloom` | `0..1`; shared settings, with backend-specific rendering |
| `--sharpen` | `0..1`; unreleased v1.1.1 candidate: OpenGL/GLES only, preference retained but inactive in Vulkan |
| `--classic-graphics` | Disable enhanced world processing without discarding saved strengths |

Numeric options reject nonfinite and out-of-range inputs. Ray tracing, TAA, indirect lighting, exposure, render scale, and roughness controls are annotated as dormant in OpenGL/GLES; shadow strength remains active in enhanced graphics. **In the unreleased v1.1.1 candidate**, **Keskinlik** is explicitly captioned inactive in Vulkan and cannot be adjusted there; `--sharpen` is also dormant there, and the OpenGL/GLES preference remains saved. The additional candidate Vulkan artist-unsharp pass was removed, not substituted for the original artwork. Do not reset preferences to change backends. `--ray-tracing on` is a preference, not a claim that the GPU supports RT.

## Troubleshooting

- **Original assets not found:** check that `game/data/menu/menulist.xml` exists and that the complete original data was copied. Use `--asset-root "/path/to/game"` if it is stored elsewhere.
- **Permission denied when launching:** if your extractor lost executable permissions, run `chmod +x yami-launcher yami-native yami-updater` in the extracted directory. Do not run the game as root.
- **Update cannot install:** move the whole installation to a per-user writable directory, such as `~/Games/etiyami`; keep its libraries and update helper together.
- **EGL/OpenGL startup error:** ensure your host graphics drivers and C++ runtime are current. The withdrawn 1.0.0 package could conflict with newer Mesa; repair that installation by extracting the latest fixed package into the same directory. Do not add its old bundled C++ libraries to `LD_LIBRARY_PATH`.
- **Vulkan startup error:** update your distribution's Vulkan driver/loader, or select **OpenGL** in the returned launcher / pass `--renderer opengl`. Public v1.1.0 supports Vulkan; Vulkan 1.3 support and required device features are necessary, while RT support is optional. A GL-only source build intentionally cannot start Vulkan.
- **Wayland-specific issue:** Linux prefers Wayland when available. To explicitly try X11 on a desktop that provides it, run `SDL_VIDEO_DRIVER=x11 ./yami-launcher`.
- **Poor performance:** reduce MSAA, filtering, or effects in Ayarlar, or use the lower-cost CLI example above.

## Checks and technical details

From a source checkout with the original `game/` data and a running graphical desktop:

```sh
ctest --test-dir build --output-on-failure
./build/yami-native --smoke --skip-intro --no-updates
```

```sh
# Optional Linux installer regression using your own original ISO
cmake --build build --target check_setup_install check_setup_gui
./build/check_setup_install "/path/to/original.iso" "$PWD/build" \
  "/absolute/path/to/7zz" "$PWD/build/yami-remove" "/path/to/new-isolated-check-directory"
./build/check_setup_gui "/path/to/original.iso" "/absolute/path/to/7zz" "/path/to/new-frame-directory"
```

The Linux `uninstall` CTest runs the standalone helper/script safety checks using isolated temporary installations. The local 1.0.4 installer verification passed **15 CTests**; `setup_gui` runs headlessly with SDL's dummy video/software renderer and simulates inline portal callbacks for ISO/folder selection, cancellation, and errors. A separate source GUI check on actual Wayland/software rendering read the original ISO, displayed its artwork-ready preview, and cancelled an active preview with responsive exit; it did not exercise a live portal popup. Earlier local verification covered real-ISO installation/repair, actual application-menu/Desktop terminal shortcuts, saved/unknown-file preservation, trusted update ownership, and both installer packaging self-tests. Public v1.0.3 passed Ubuntu 22.04 x86_64/ARM64 build/package checks; downloaded x86_64 AppImage and `.run` installers also passed real installation, terminal cancellation, repair, and confirmed removal without deleting saves or unknown files. See the technical notes for measured evidence and security boundaries. ARM64 packaging is verified, not physical ARM GPU/gameplay execution.

Historical v1.1.0 source verification passed **17/17 Vulkan-enabled CTests in 1.97 seconds** and **15/15 GL-only CTests in 1.76 seconds**, on an RX 7900 XTX, RADV Mesa 26.2.4, COSMIC Wayland; fresh pre-tag repeats passed **17/17 in 3.66 seconds** and **15/15 in 6.22 seconds**. Real GPU checks passed RT on/off, all four OpenGL/GLES × 4×/zero-MSAA combinations, artwork-preserving illumination denoising and camera-motion history, moving-shadow rejection, and sample-aligned MSAA metadata (excluded-normal contamination fell from 446 pixels to zero). Stochastic AO fluctuation fell from 5.84772 spatial-only to 1.7038 with temporal filtering. Original-game smokes and inspected captures passed all four levels across the two backends; Vulkan RT-off at 0.75 render scale kept acceleration-build/ray-query counters **0 / 0**. These results predate the latest candidate shaders and are separate from the public-package proof below. No new Khronos validation run was performed for that revision; earlier validation and authored-colour fixture evidence remain historical in the technical notes.

Earlier source launcher checks also passed Vulkan→OpenGL handoff with every Vulkan-only preference retained, and GL-only Vulkan-selection failure→visible recovered GL launcher→explicit OpenGL game-window handoff. The desired-Vulkan preview caption **Oyunda kontrol** correctly defers GPU capability checking until Play. The failure check acknowledged only the owned application's exact error notification through a scoped test hook; its constructor failure, recovered window/banner and initialized game window were real, but the host modal dialog itself was not exercised. See the technical notes for this verification boundary.

Public v1.1.0 package verification independently checked all six downloaded ZIP/`.run`/AppImage assets for both architectures. The downloaded Ubuntu-built x86_64 AppImage installed into isolated storage without source overrides; all 28 installed engine/library files matched the public ZIP. Ten installed-game paths passed at **1600×900 on Wayland**, including GL/GLES levels 1–4, Vulkan RT/TAA/4× MSAA levels 1–4, Vulkan RT-off/TAA-off at 0.75 render scale, and a GL zero-shadow comparison. Captures were inspected for original artwork detail, shaped actor shadows/self-shadow, cloud/fog compositing and native HUD. The first zero-shadow comparison lost focus; a fresh-save retry after idle passed without bypassing focus checks. This does not establish physical ARM64 GPU behavior, full campaign completion, performance guarantees, a live portal picker, full launcher handoff or an older-version automatic-update transaction. See the technical notes for the exact release/source and verification boundaries.

Separate x86_64 **unreleased v1.1.1 candidate** checks covered real-ISO installation with a regular file at the Desktop location (**5.47 seconds**, application-menu entries created with a skip warning), standalone install/repair/rollback/uninstall checks (**13.24 seconds**) and `desktop-file-validate` on four entries. The actual installed v1.1.0 Apps entry also launched through GIO on GNOME 51 Wayland, with a scoped AT-SPI **ETI Yami** window visible/showing and real GPU activity; this was not a framebuffer-capture check.

Normal-source candidate verification passed full `--require-rt` and `--rt-off` GPU checks and **17/17 Vulkan-enabled CTests in 316.50 seconds**; the combined build/RT-off/CTest command completed in **608.39 seconds**. GL-only source verification passed **15/15 CTests in 15.00 seconds** on GNOME Wayland; later Vulkan-only changes retain that GL/GLES scope. All unchanged strict artwork-border limits passed at native/half scale and 4×/8× MSAA after permanent epoch rotation. The latest half-scale moving border maximum was **0.232422**, below the unchanged **0.882215** bound, versus **1.03125** before rotation; physical-reference depth stayed **17.6443**, retained depth **17.5215** and contrast **0.993263**. RT-off acceleration-build/ray-query-frame counters were **0 / 0**. The technical notes retain scoped and failed-before causal evidence, including the static-silhouette checker correction; temporary probes are removed and no artwork tolerance was waived.

Latest-source original-game Vulkan smokes passed levels **1, 3 and 4** with RT/TAA and **8× MSAA at 2560×1440**, plus level 1 with RT off and half-scale rendering; all exited 0 after 123 fixed ticks with meaningful movement. Level 2 completed paired full-effects/neutral runs with **64 frozen, 64 animated and 64 moving frames each**, with all corresponding trace fields matched and all frame panels inspected. No conspicuous false actor grids, double-body image or long artwork trails were seen in the inspected regions; genuine shaped shadows and some localized mottling remained. Strict ±5% artwork comparisons apply to frozen regions, not all moving frames: fixed crops lose portions of the actor and later cross shutter/window geometry. This is scoped source verification, not zero-noise, whole-campaign visual parity, physical ARM64 GPU or public-package proof. Native release CI, package/publication and Khronos validation evidence remain unclaimed.

The smoke command exercises real menu input, New Game, gameplay, and rendering; it needs an actual focused window. Keep the mouse/keyboard idle and do not switch windows during this automated check: concurrent physical input or focus loss intentionally fails it. By default it uses a new temporary save directory. Existing checks cover assets, scripts, gameplay, audio, settings, rendering, and safe update installation. Full campaign progression has not been manually played end-to-end.

The optional installer check performs real extraction and isolated install/cancellation/safety/shortcut checks. Its final directory argument must not already exist; it creates an installation there and needs the same free space as an ordinary install. These commands are instructions, not a claim that your ISO or desktop has already been verified.

The optional GUI check needs a running graphical desktop, your own original ISO and archiver, and a frame directory that does not already exist. It saves local preview/cancellation framebuffers and performs no installation; callback delivery is simulated even in this graphical mode. Keep those original-artwork captures private.

For reconstruction notes, implementation details, and verification evidence, see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).
