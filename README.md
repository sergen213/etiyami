# ETI Yami Native — Linux

A native C++20/SDL3 reconstruction of **ETI Yami**, using the original game's artwork, models, scripts, audio, and videos. Includes an original-style settings launcher, OpenGL/GLES rendering, widescreen support, and automatic launcher/engine updates. No Wine, DirectX installer, or Windows codec installation is needed.

**The original game assets are not included in this repository or release downloads. You need a legitimate copy of the original game.** Linux releases are available for x86_64 and ARM64; Windows/macOS releases are deferred.

## Quick start: download and play

### 1. Download the Linux release

Open [the latest release](https://github.com/sergen213/etiyami/releases/latest) and choose:

| Computer | Download |
|---|---|
| Intel/AMD 64-bit Linux | `yami-linux-x86_64.zip` |
| 64-bit ARM Linux | `yami-linux-arm64.zip` |

Requirements: **glibc 2.35 or newer**, your distribution's graphics drivers and C++ runtime, and an OpenGL 3.3 or GLES 3-capable desktop. Release packages include application libraries, not graphics drivers or the host C++ runtime. ARM64 packages are built and validated in CI; live graphics/gameplay verification has been performed on x86_64 Linux.

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

The launcher exposes the same 13 settings as the in-game **Ayarlar / Settings** screen: brightness, sensitivity, aiming mode, music/effects volume, lighting, ambient occlusion, reflections, bloom, sharpening, MSAA, texture filtering, and fullscreen. It stays windowed while you configure settings; the saved fullscreen choice applies when you press Play.

Keyboard navigation: **Tab / Shift+Tab** selects a control, **arrow keys** adjust it, **Enter / Space** activates it, and **Escape** closes the launcher. Mouse buttons apply changes on release. Closing saves settings without starting the game.

![Original-style settings launcher](analysis/native-launcher-github-updated.png)

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

The release ZIP is the easiest way to play. To build on Arch Linux / CachyOS:

```sh
sudo pacman -S --needed git gcc cmake ninja pkgconf sdl3 libepoxy libxml2 ffmpeg curl libarchive json-c

git clone https://github.com/sergen213/etiyami.git
cd etiyami
```

Supply the original assets as described above, placing `game/` at the repository root, then build and launch:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/yami-launcher
```

For other distributions, install equivalent development packages: a C++20 compiler, CMake 3.20+, Ninja, pkg-config, SDL3, libepoxy, libxml2, FFmpeg (`libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`), libcurl 7.85+, libarchive, and json-c. FFmpeg must include the Indeo 5 decoder. **SDL2 is not a substitute for SDL3.** The Linux release workflow builds its dependencies on Ubuntu 22.04; distribution packages may be too old for a source build.

### Extracting from the original installer

The repository's `extract_game.py` supports the original Turkish Yami MSI/CAB release. Place your legitimate **`Yami.msi`** and **`Data1.cab`** at the repository root, then on Arch/CachyOS run:

```sh
sudo pacman -S --needed python 7zip cabextract
python3 extract_game.py
```

This reads the installer tables and CAB without running a Windows installer, validates the extracted files, and creates `game/`. It refuses to overwrite an existing `game/`. It is specific to this original release, not a general MSI extractor. Installer archives and original data are excluded from Git; do not commit or redistribute them without the appropriate rights.

## Useful launch options

Examples below use the downloaded executables; for a source build, prefix them with `./build/` instead of `./`.

```sh
# Direct gameplay, bypassing the settings launcher
./yami-native --skip-intro

# Use GLES instead of the default OpenGL backend
./yami-launcher --gles

# Original lighting without the enhanced world effects
./yami-launcher --classic-graphics

# Lower-cost graphics settings
./yami-launcher --samples 0 --anisotropy 1 --ao 0 --reflections 0 --bloom 0 --sharpen 0

# Print the installed version or all supported options
./yami-native --version
./yami-native --help
```

Effect strengths accept values from `0` to `1`; zero disables that effect. CLI graphics options override saved values, while edits made in the launcher take precedence before Play. `yami-native --launcher` opens the same launcher interface.

## Troubleshooting

- **Original assets not found:** check that `game/data/menu/menulist.xml` exists and that the complete original data was copied. Use `--asset-root "/path/to/game"` if it is stored elsewhere.
- **Permission denied when launching:** if your extractor lost executable permissions, run `chmod +x yami-launcher yami-native yami-updater` in the extracted directory. Do not run the game as root.
- **Update cannot install:** move the whole installation to a per-user writable directory, such as `~/Games/etiyami`; keep its libraries and update helper together.
- **EGL/OpenGL startup error:** ensure your host graphics drivers and C++ runtime are current. The withdrawn 1.0.0 package could conflict with newer Mesa; repair that installation by extracting the latest fixed package into the same directory. Do not add its old bundled C++ libraries to `LD_LIBRARY_PATH`.
- **Wayland-specific issue:** Linux prefers Wayland when available. To explicitly try X11 on a desktop that provides it, run `SDL_VIDEO_DRIVER=x11 ./yami-launcher`.
- **Poor performance:** reduce MSAA, filtering, or effects in Ayarlar, or use the lower-cost CLI example above.

## Checks and technical details

From a source checkout with the original `game/` data and a running graphical desktop:

```sh
ctest --test-dir build --output-on-failure
./build/yami-native --smoke --skip-intro --no-updates
```

The smoke command exercises real menu input, New Game, gameplay, and rendering; it needs an actual focused window. By default it uses a new temporary save directory. Existing checks cover assets, scripts, gameplay, audio, settings, rendering, and safe update installation. Full campaign progression has not been manually played end-to-end.

For reconstruction notes, implementation details, and verification evidence, see [REVERSE_ENGINEERING.md](REVERSE_ENGINEERING.md).
