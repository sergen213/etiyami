# Building from source

## Build on Linux

The AppImage installer is the easiest way to play. To build on Arch Linux / CachyOS:

```sh
sudo pacman -S --needed git gcc cmake ninja pkgconf sdl3 libepoxy libxml2 ffmpeg curl libarchive json-c vulkan-headers glslang python

git clone https://github.com/sergen213/etiyami.git
cd etiyami
```

Supply the original assets as described in [INSTALL.md](INSTALL.md#2-supply-the-original-assets), placing `game/` at the repository root, then build and launch:

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

### Build on Windows (MSYS2 UCRT64)

Use one MSYS2 **UCRT64** environment for the compiler and every dependency; do not mix MSYS, MinGW, and UCRT libraries:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-toolchain \
  mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja \
  mingw-w64-ucrt-x86_64-pkgconf mingw-w64-ucrt-x86_64-python \
  mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-libepoxy \
  mingw-w64-ucrt-x86_64-libxml2 mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-curl-winssl mingw-w64-ucrt-x86_64-libarchive \
  mingw-w64-ucrt-x86_64-json-c mingw-w64-ucrt-x86_64-vulkan-headers \
  mingw-w64-ucrt-x86_64-glslang mingw-w64-ucrt-x86_64-7zip
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DYAMI_ENABLE_VULKAN=ON
cmake --build build --parallel
./build/yami-launcher.exe --asset-root game
```

The build includes `yami-setup.exe`, `yami-remove.exe`, Vulkan/FSR support, and regression checks. `ctest --test-dir build --output-on-failure -R '^(update|update_install|windows_update_install|windows_uninstall)$'` runs the asset-free Windows update/uninstall checks. `windows_setup` is registered when `YAMI_TEST_ISO` and `YAMI_TEST_ARCHIVER` identify your original ISO and trusted `7zz.exe`; it performs a real isolated installation/repair/uninstall. GUI/gameplay checks need original assets and an actual desktop. Release packaging collects the matching UCRT64 DLL closure; use `tools/package_release.py` and `tools/package_windows_setup.py` as shown in `.github/workflows/release.yml`.

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

## Checks

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

The smoke command exercises real menu input, New Game, gameplay, and rendering; it needs an actual focused window. Keep the mouse/keyboard idle and do not switch windows during this automated check: concurrent physical input or focus loss intentionally fails it. By default it uses a new temporary save directory. Existing checks cover assets, scripts, gameplay, audio, settings, rendering, and safe update installation. Full campaign progression has not been manually played end-to-end.

The optional installer check performs real extraction and isolated install/cancellation/safety/shortcut checks. Its final directory argument must not already exist; it creates an installation there and needs the same free space as an ordinary install. These commands are instructions, not a claim that your ISO or desktop has already been verified.

The optional GUI check needs a running graphical desktop, your own original ISO and archiver, and a frame directory that does not already exist. It saves local preview/cancellation framebuffers and performs no installation; callback delivery is simulated even in this graphical mode. Keep those original-artwork captures private.

For reconstruction notes, implementation details, and verification evidence, see [REVERSE_ENGINEERING.md](../REVERSE_ENGINEERING.md).
