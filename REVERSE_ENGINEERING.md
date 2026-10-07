# ETI Yami recovery

## Delivered native engine

`native/` is a C++20 reconstruction; `build/yami-native` is the working Linux executable. It loads the original assets and compiled scripts directly. It does **not** launch Wine, the original executable, or `son.eti`.

The implementation includes all four levels, the 40-opcode script VM and 20 script hosts, model/mesh/animation loaders, CPU skinning, body/flight physics and collision, player controls, five enemy AI families, combat, pickups, mission callbacks, level transitions, the original menus/quizzes/HUD, settings, checkpoints, video/audio, and the native ending dialog/code generator. The original 32-bit VM words, animation ordinals, callable registration order, and 33 ms tick rules are retained rather than translated into host-pointer-sized values.

Linux execution is verified on COSMIC Wayland and X11, with OpenGL and OpenGL ES. Current release work targets Linux only; Windows/macOS builds are deferred. Portable source remains available, but no Windows/macOS binary or blanket behavioral-parity certification is claimed. A complete human campaign playthrough is **not verified**.

## Build and run

Dependencies: a C++20 compiler, CMake 3.20+, Ninja, pkg-config, SDL3, libepoxy, libxml2, FFmpeg development libraries (`libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`), libcurl 7.85+, libarchive, and json-c. FFmpeg must include the Indeo 5 decoder. Curl supplies verified HTTPS, json-c parses GitHub metadata, libarchive safely reads update ZIPs, and FFmpeg's SHA implementation verifies download integrity. Neither the bundled DirectX installer, proprietary Indeo installer, FMOD DLL, nor Wine is needed.

On Arch/CachyOS:

```sh
sudo pacman -S --needed gcc cmake ninja pkgconf sdl3 libepoxy libxml2 ffmpeg curl libarchive json-c
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/yami-native --width 1600 --height 900
```

Assets are discovered relative to the executable (`game/`, then the development tree's `../game/`), with the current directory's `game/` as a final fallback. macOS also checks `game/` beside the `.app` for Finder launches, independent of the working directory. An explicit `--asset-root /path/to/game` is used exactly and must contain `data/menu/menulist.xml`. SDL supplies UTF-8 executable/preference paths, including macOS bundle Resources. The asset tree is read-only; checkpoints and `game.ini` go to `SDL_GetPrefPath("ETI", "YamiNative")`, or an explicit `--save-dir` outside the asset tree.

Controls: WASD/arrows move; mouse controls the camera and the original menu cursor; Ctrl/left mouse fires; Space performs the special action; E/Enter interacts; Tab shows objectives; Escape opens the game menu; F11 toggles fullscreen. Mouse capture is released on focus loss and during the ending dialog, with held input cleared before reacquisition.

Optional flags: `--fullscreen`, `--gles`, `--samples 4`, `--anisotropy 16`, `--skip-intro`, `--level 1` through `--level 4`, and `--capture output.ppm`. `--help` lists the interface. Linux prefers Wayland when available; `SDL_VIDEO_DRIVER=x11` explicitly selects X11.

Enhanced graphics are on by default. Effect strengths are independently adjustable in `0..1`: `--ao 0.65 --reflections 0.22 --bloom 0.12 --sharpen 0.18`. Zero disables that effect; nonfinite/out-of-range values are rejected. `--classic-graphics` restores the recovered lighting and disables world effects while retaining widescreen layout, MSAA, and texture filtering.

### Ayarlar graphics controls

The original **Ayarlar** screen now has a right-hand parchment graphics panel, using the game's original bitmap font, blue/red buttons, hover artwork, and background. Existing brightness, sensitivity, sound, aiming knobs, and Back remain in their original locations. The original asset files are not modified.

- **Işıklandırma:** classic or enhanced lighting/world effects.
- **Ortam gölgesi, Yansımalar, Parlama, Keskinlik:** SSAO, rough-surface reflections, bloom, and sharpening strengths; each click changes five percentage points, bounded to 0–100%. Loaded/CLI values are retained until edited. Classic mode retains these values but does not execute the effects.
- **Yumuşatma:** MSAA off or a GPU-supported quality level. **Doku filtresi:** anisotropic filtering from 1× to 16×, bounded by GPU support. Live changes preserve the GL context and scene assets; only MSAA render targets are reallocated, and filtering updates already-uploaded world textures.
- **Tam ekran:** fullscreen on/off, synchronized with F11 and completed SDL fullscreen events, including asynchronous Wayland transitions.

Native buttons change on mouse release, not hover or repeated held frames. Changes apply immediately and are saved by atomic temporary-file replacement. Original held knobs save on release; Back and normal window close also save. Five-line legacy settings files remain readable. Native `game.ini` appends validated `grafik_*` fields; invalid, duplicate, or unknown fields are rejected. Saved graphics settings load before renderer creation; explicit CLI flags override only their corresponding saved fields. The effective settings are saved on normal exit. Window dimensions follow resize; OpenGL/GLES remains a launch-time selection.

### Native asset-based launcher

```sh
./build/yami-launcher
# Same interface through the direct executable:
./build/yami-native --launcher
```

The launcher uses the original Ayarlar artwork, bitmap fonts, dials, parchment graphics panel, and game artwork rather than host-native or web controls. It exposes the same five original settings and eight native graphics settings. Both interfaces share the settings parser, bounds, hardware capabilities, and atomic file replacement; there is no second launcher configuration file.

The launcher starts windowed, even when fullscreen is saved. **Tam ekran** selects the game's launch mode without taking over the desktop while editing settings. **OYNA / PLAY** saves the selected settings and enters the native game in-process, reusing the window/context/menu assets. Audio registration, intro playback, game VM initialization, and level loading happen only after Play. Closing saves settings without starting the game. CLI overrides seed the controls; subsequent launcher edits take precedence. Tab/Shift-Tab select one of the 13 setting rows or Play/Close, arrows change values, Enter/Space activate, and Escape closes; keyboard focus is visibly outlined. `--no-launcher` bypasses the interface, and `--smoke` always bypasses it.

For a portable installed layout with original data:

```sh
cmake -S . -B build -DYAMI_INSTALL_ORIGINAL_ASSETS=ON
cmake --build build
cmake --install build --prefix /path/to/yami-distribution
```

The install target includes launcher, game, and update helper, original `game/data`, the original EULA/icon, and macOS bundle Resources when targeting Apple. It excludes the old Windows executables, Wine reference, legacy DLLs, and bundled codec/DirectX installers. Asset installation is opt-in; redistribution still requires the appropriate asset rights. Third-party runtime-library packaging is handled by the release workflow; platform installers and Apple notarization are separate requirements.

### Windows build prerequisites

Use an MSYS2 **UCRT64** shell and its matching compiler/dependency packages, not a mixture of MSYS and MinGW libraries:

```sh
pacman -S --needed mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake \
  mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-pkgconf \
  mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-libepoxy \
  mingw-w64-ucrt-x86_64-libxml2 mingw-w64-ucrt-x86_64-ffmpeg \
  mingw-w64-ucrt-x86_64-curl-winssl mingw-w64-ucrt-x86_64-libarchive \
  mingw-w64-ucrt-x86_64-json-c
cmake -S . -B build-win -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
./build-win/yami-native.exe --asset-root game
```

The dependency packages are published by MSYS2: [SDL3](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-sdl3), [libepoxy](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-libepoxy), and [FFmpeg](https://packages.msys2.org/packages/mingw-w64-ucrt-x86_64-ffmpeg). This recipe has not been executed on Windows. Keep UCRT64 runtime DLLs available on `PATH`; a standalone Windows distribution is not supplied. CMake also defines MSVC strict floating-point/UTF-8 options, but MSVC execution is unverified. macOS requires equivalent dependencies and a compatible SDL/OpenGL desktop session; its build/run is likewise unverified.

### Public GitHub project and updates

Repository: [sergen213/etiyami](https://github.com/sergen213/etiyami), **public**. Git tracks the native source, build/release configuration, recovery utilities, existing technical notes, and selected visual evidence. Proprietary `game/` data, installer archives/executables, Wine state, build products, and credentials remain local and excluded.

The launcher checks GitHub releases in the background before Play. Version checks use `vMAJOR.MINOR.PATCH`; only a newer complete asset matching the native OS/architecture is eligible. Downloads use verified HTTPS and GitHub's authenticated asset endpoint. The SHA-256 digest from GitHub must match before bounded extraction; traversal, links, conflicting paths, and original/user-data targets are rejected. A detached copy of `yami-updater` replaces launcher, game/engine binaries, and their bundled runtimes with backup/rollback, then restarts the launcher with the same asset/preferences paths. Settings and original assets are not part of updates.

Public releases can be checked and downloaded without a GitHub account or token. Optional per-user `YAMI_GITHUB_TOKEN`, `GH_TOKEN`, or `GITHUB_TOKEN`, or an existing local `gh auth login` credential, also work. Tokens are never built into the application or written to update files/command arguments. If this updater is configured for a private repository, grant a fine-grained token **Contents: read** only for that repository.

Environment-based authentication is inherited by the trusted update helper and restarted launcher so it survives an automatic restart. It is never serialized into the package or staging directory. The helper uses null standard streams and a private diagnostic log.

Missing releases, inaccessible metadata, offline operation, or a non-writable installation produce a visible status; the current installed game remains launchable. Closing cancels a pending check/download. `--no-updates` explicitly disables network checks, `--version` prints the installed binary version, and direct game/smoke execution does not check updates.

`.github/workflows/release.yml` builds Linux x86_64 and arm64 packages without proprietary assets. The publication contract remains exactly six assets: `yami-linux-{x86_64|arm64}.zip`, `yami-setup-linux-{x86_64|arm64}.run`, and `yami-setup-linux-{x86_64|arm64}.AppImage`. ZIPs retain exactly the three launcher/game/updater executables and application dependencies; installers additionally carry `yami-setup`, the standalone `yami-remove` plus `uninstall.sh`, and a trusted static `7zz` for offline ISO extraction. There is no separate uninstall AppImage or release asset. Ubuntu 22.04 x86_64/arm64 jobs and the glibc 2.35 ceiling are unchanged. Linux executables and application libraries use a flat installation with `$ORIGIN` RUNPATH, without `LD_LIBRARY_PATH` injection; host glibc, `libstdc++`/`libgcc`, and graphics drivers are never bundled. This also avoids obsolete Ubuntu runtimes retained in `lib/` by the withdrawn 1.0.0 package. CMake embeds `YAMI_RELEASE_VERSION` and `YAMI_UPDATE_REPO`; the current default version is the unpublished 1.0.3 candidate and the default repository is this public project. Windows/macOS release jobs are deferred.

AppImages combine architecture-matched, fully static uruntime 0.8.1 with a SquashFS installer payload and automatic extraction fallback when FUSE is unavailable. `--appimage-extract-and-run` explicitly requests no-FUSE execution. The `.run` alternative uses standard shell/tar/gzip/coreutils. Neither format includes the original game artwork, font, icon, or data. Packaging checks enforce payload membership, architecture, host-library exclusions, and the glibc ceiling; those checks are not evidence of physical ARM64 GPU/gameplay execution. The AppImage carries pinned runtime/helper source archives, license notices, and exact-version attribution for all 86 locked Rust dependencies. The MIT-declared `memfd-exec` upstream supplies no copyright notice; its original source/metadata and explicitly labeled SPDX permission terms are preserved without inventing attribution.

To publish, push a stable `vMAJOR.MINOR.PATCH` tag after committing the intended source. A manual workflow run resolves an existing tag's commit rather than silently building the selected branch. Both architecture jobs and re-verification of all six assets must succeed before draft creation. Uploads remain draft until the workflow verifies each GitHub SHA256 digest against its local file through the draft's numeric API URL, then publishes the complete release. Failed uploads remain draft and are not offered by the launcher. This describes the publication contract, not an assertion that the current source has already been released.

### Linux original-ISO installer

`native/setup_assets.cpp` reads the selected original ISO's MSI/CAB content without executing Windows programs. MSI table mapping selects 9,032 native assets (637,554,001 bytes), excluding legacy executables and codec/DirectX installers. `native/setup_main.cpp` uses SDL native file/folder dialogs and cancellable background work; Tab/Shift+Tab, Enter/Space, and Escape provide keyboard operation. Before installation, nine allowlisted menu/material/image/font files are extracted into a private temporary preview and validated by `native/setup_artwork.cpp`; original artwork is never embedded in public binaries. The preview is removed on cancellation/exit.

`native/setup_install.cpp` privately stages original assets, three native engine executables/application libraries, the root `uninstall.sh`, and `.eti-yami-uninstall/yami-remove`; it converts the ISO's original icon to PNG and publishes the installation plus play/uninstall application-menu/Desktop shortcuts with ownership checks and rollback. It refuses root execution, unrelated existing destinations/shortcuts, links, unsafe paths, and malformed or oversized input. A valid inventory-bearing reinstall keeps existing engine/assets/settings and repairs shortcuts/removal support without needing the original ISO or engine source files. Legacy format-1 roots without inventories require the original ISO to prove asset paths/bytes; unproven files/libraries are retained rather than claimed by scanning the live tree. Cancellation does not publish a partial fresh installation. Default storage is `${XDG_DATA_HOME:-$HOME/.local/share}/etiyami`; preferences remain in SDL's separate per-user location.

Build `yami-setup` to run the source GUI with `--engine DIRECTORY --archiver /absolute/path/to/7zz`. Its dependency builds the SDL/FFmpeg-free, stdlib/POSIX-only `yami-remove`, and CMake copies the matching `uninstall.sh` beside the setup binary. `InstallRequest.remover` defaults to `SDL_GetBasePath()/yami-remove`; `--remover FILE` selects an alternate helper whose parent also contains the script. The optional real-ISO regression is `check_setup_install ISO ENGINE 7ZZ REMOVER NEW_ISOLATED_DIRECTORY` (build target `check_setup_install`); it exercises extraction, cancellation, safe publication, owned reinstall/legacy provenance, and both shortcut pairs in an isolated directory. The `uninstall` CTest invokes `check_uninstall HELPER SCRIPT`, linking only the standard C++ runtime.

The root `.eti-yami-install` format-1 marker remains unchanged. `.eti-yami-installed-files` records exact installer-created file paths and owned shortcut paths; `.eti-yami-engine-files` records validated engine payload filenames. Successful trusted updates maintain/merge that engine inventory locally without adding a fourth executable or manifest to the update ZIP. Manual ZIP installations never acquire uninstall authority merely by updating.

The uninstall shortcut has `Terminal=true` and invokes `/usr/bin/env -- /bin/sh` with the escaped installed script path. The script derives its own root and invokes the private helper; it never requests `--yes`, sudo, a download, a GUI, or an ISO. Interactive removal requires a terminal, shows the exact root and preserved-data policy, and defaults to cancellation. Only recorded ownership is removable; save/settings/unknown files survive and remaining files are reported. Only empty recorded child directories are pruned; the installation root is always retained, even when empty. Invalid/missing ownership metadata fails closed. Removal metadata/support and shortcuts remain until late in the operation. The helper depends only on host glibc/C++ libraries, not the engine/SDL/FFmpeg runtime.

Security boundary: removal trusts the installing/current UID and unsigned private ownership inventories. Private `0700` storage and the cooperative updater lock exclude other-user access and concurrent cooperating product writers. No-follow descriptor-relative traversal, root/parent/mount identity validation, preflight, and checked recovery detachment guard path substitution. Linux `unlinkat` remains name-based, not inode-conditional: a malicious same-UID process able to modify the private recovery namespace is outside this guarantee. No hostile-same-UID deletion guarantee, privileged/root helper, or protective namespace is claimed.

Both installer formats include this complete support pair offline. `.run` validation checks required members, exact executable/nonexecutable modes, matching script bytes, helper architecture/glibc floor/host-only dependencies, closed application-library membership, and byte-for-byte agreement with the paired engine ZIP. AppImage verification inherits that verified payload and checks exact SquashFS membership/modes/bytes. New regressions mutate real support members rather than mock command output. Existing pinned archiver/runtime caches, attribution/notices, corresponding-source policy, and original-asset exclusions remain unchanged.

### Local 1.0.3 uninstall verification

The final integrated suite passed **14/14 CTests in 10.86 seconds**. The real original-ISO installer check passed in **9.34 seconds**, including legacy migration, custom-icon preservation, and pinned-source checks. `ldd` on the removal helper showed only host `libstdc++`, `libm`, `libgcc_s`, `libc`, and the loader. The `.run` and AppImage self-tests passed their real integrity, membership, byte/mode, corruption, and bounds checks.

- Source fresh-ISO installation completed in **1.737 seconds**, followed by confirmed uninstall in **1.162 seconds**. Both actual GIO application-menu and Desktop uninstall shortcuts launched the exact helper for an installation path containing Turkish UTF-8, spaces, `%`, quotes, backslash, `$`, and backticks. `/usr/bin/kgx` supplied a real `/dev/pts/2` terminal; the helper's syscall was reading standard input while waiting for default-cancel confirmation.
- Answering no left all file inodes and all four shortcuts intact. Repair with missing ISO/engine source files successfully restored a missing script, private helper, and uninstall shortcuts without changing original-asset SHA256 or user data.
- A real local AppImage, launched normally without source overrides, completed fresh installation in **8.753 seconds** and confirmed removal in **1.165 seconds**, proving its bundled helper/script worked offline with the same preservation behavior.
- A real `install_prepared` call installed the three engine ELFs and a trusted real json-c library alias, `libeti-uninstall-proof.so.1`. Confirmed uninstall completed in **1.159 seconds**: the new library was recorded and removed, an unknown library remained unclaimed and intact, and the update had preserved the script/helper/artwork before removal.
- Saved checkpoints, root `game.ini`, custom game notes, user-save directories, an unknown `.so`, and external SDL preference/save data survived. `/etc/passwd` and the resolved host `libstdc++` retained identical SHA256. All fixtures were isolated in private owned cache locations; the source ISO and original game tree were untouched.

These are local x86_64 observations, not physical ARM GPU or published-artifact proof. **Public 1.0.3 publication/download verification remains pending.** The evidence below describes the previously published 1.0.2 installer.

Historical 1.0.2 installer verification on x86_64 COSMIC Wayland:

- All 13 CTests passed; the real original ISO extraction check also proved preview-only membership, rejection of duplicate/missing artwork selections, and cancellation of the live MSI reader.
- The real ISO install check passed fresh installation, original-icon fidelity, owned repair, early/mid/late cancellation, publication rollback, shared-parent refusal, and unrelated-root substitution preservation.
- Actual branded GUI installation extracted the native assets and created both shortcuts. The AppImage exercised forced extraction and normal startup, then actual Install → Launch into the installed OpenGL settings renderer; the captured launcher completed normal teardown.
- `gio launch` opened both generated shortcuts in a path containing Turkish UTF-8, spaces, `%`, `"`, `\`, `$`, and backticks. GIO rejects percent-containing `argv[0]` before expanding desktop field codes; `Exec=/usr/bin/env --` preserves literal paths without shell interpretation.
- The freshly installed game exercised menu New Game and active level-2 gameplay: Wayland/OpenGL, 1280×720 actual pixels, 123 ticks, movement and action changes, exit 0.
- Offline installer integrity/path regressions and AppImage runtime/policy/filesystem/payload/crate-attribution corruption regressions passed. No physical ARM64 GPU proof or complete manual campaign playthrough is claimed.

Published [v1.0.2](https://github.com/sergen213/etiyami/releases/tag/v1.0.2) from commit `0f9314e1f974c48dd538adc4bf4d2e58b53c3471`: native Ubuntu 22.04 x86_64/ARM64 build, forced-extraction execution, all-six-asset verification, and GitHub digest-gated publication passed in [run 37554476782](https://github.com/sergen213/etiyami/actions/runs/37554476782). Anonymous downloads of the x86_64 AppImage and `.run` matched GitHub SHA256 (`913a7942bb52c28e0e150bbb118a1590c80eb846c1e867fba256f2c8ef9a3bb1` and `87e9ff860d899871078ef46275946f4b60f609178f44901a328ee369c9448569`). The downloaded AppImage installed the real ISO into a fresh isolated root and launched the installed native settings window; its high-DPI artwork and launcher captures were inspected. Normal AppImage startup also reached the native CLI with mount/namespace syscalls deliberately denied, proving automatic no-FUSE extraction fallback.

The source-built game smoke above passed. A subsequent published-game automated smoke was interrupted by concurrent physical mouse motion and then focus loss; SDL queue tracing identified external mouse ID 1 alongside the smoke-owned input. That interrupted attempt is not reported as a passing gameplay smoke. Keep the desktop idle while exercising the automated input checks.


## Graphics and platform changes

- Resizable/high-DPI windows and true widescreen world projection. The recovered frustum retains half-height 0.75, near plane 1, and far plane 30000; only the world aspect changes. Gameplay HUD energy/objectives anchor left, score/notifications centre, battery/jump count anchor right, and the crosshair stays centred. Original artwork and glyph proportions are preserved by stretching only empty HUD background strips. Menus, videos, and ending retain their proportionally fitted 1024×768 canvas; windows narrower than 4:3 also retain the fitted HUD.
- Default 4× MSAA, trilinear mipmaps, and up to 16× anisotropic filtering, subject to GPU capabilities. `--samples 0 --anisotropy 1` disables the quality additions.
- OpenGL 3.3 core or GLES 3 replaces fixed-function/WGL rendering. Enhanced world lighting is evaluated per pixel with normalized normals and a modest hemisphere ambient term. Classic mode retains recovered vertex lighting. Original fog, alpha tests, blending, depth/cull rules, and image orientation remain intact.
- World-only depth-based SSAO supplies contact occlusion. Depth normals choose coplanar neighbors rather than interpolating across silhouettes/corners.
- Screen-space reflections ray-march actual scene depth and sample actual scene colour, with dielectric Fresnel weighting, hit refinement, and edge/distance fading. Exported materials contain no gloss/roughness metadata, so all legacy surfaces use a conservative rough dielectric fallback (roughness 0.85, F0 0.04), not an upward-facing-is-mirrored heuristic. A depth-gated rough lobe softens real hits; even maximum reflection strength replaces at most 2.25% of the surface colour. Pavement remains diffuse. Off-screen objects, hidden surfaces, and misses produce no invented reflection.
- Subtle highlight bloom and depth-aware local contrast complete the world pass. Colour/depth are resolved into persistent size-dependent targets before HUD/menu overlays, leaving interface text and movies untouched. MSAA and zero-MSAA paths avoid texture feedback; resizing recreates all affected targets.
- Original brightness settings are applied through a window-only postprocess, never by changing monitor gamma.
- SDL3 replaces Win32/DirectInput. The first framebuffer is committed before waiting for focus, avoiding a Wayland unmapped-window/focus deadlock. Resize and fullscreen transitions recreate render targets from the actual compositor-provided pixel size.
- Original menu pause/resume timer compensation (`00430060` / `004300e0`) excludes menu wall time from gameplay and animation clocks. Rendering no longer mutates the paused simulation timestamp.
- FFmpeg replaces AVIFile/Indeo and legacy audio decoding; SDL streams the mixer output. AVI sample slots are preserved, including two-byte Indeo repeat samples that produce no new decoded picture.
- Native checkpoint replacement is written and flushed to a temporary file before rename. The original format, two-decimal position precision, ordered callable prefix, and script-variable tail are preserved. The ending consumes only its own native `save43.eti` handoff and returns to the game without starting a Windows helper.

The ending retains historical 2006 text and code generation. Telephone input stays local; the native implementation makes no network connection and does not automatically access the clipboard.

## Exercised verification

```sh
ctest --test-dir build --output-on-failure
./build/yami-native --smoke --width 1600 --height 900 --level 1
./build/yami-native --smoke --skip-intro --width 1680 --height 720 --level 2
./build/yami-native --smoke --skip-intro --width 1200 --height 900 --level 3
./build/yami-native --smoke --skip-intro --gles --samples 0 --width 1600 --height 900 --level 4
```

All twelve CTests passed: assets, script VM, gameplay, enemy AI, entity, combat, media, menu, ending, actual GPU renderer, update validation, and transactional update installation. Update checks exercise SHA-256 tampering, unsafe ZIP members, version/schema boundaries, cancellation, protected paths, busy locks, replacement and rollback after a real filesystem failure. Asset checks parsed 984 models, 3,947 meshes, 141 animation files, and 2,523 materials. GPU geometry fixtures verify corner occlusion, red-wall reflections onto the floor, unchanged background, and unaffected right-anchored HUD pixels. Renderer checks passed all four combinations of OpenGL/GLES and 4×/zero MSAA. Menu geometry checks verify 16:9/21:9 anchors, unscaled artwork/glyphs, and the narrow-window fallback.

All four enhanced native level smokes passed on Wayland: real New Game mouse hit/release, intro/cutscene frames, 123 fixed ticks, meaningful player movement/action, Escape to the original menu, two simulated minutes paused, and mouse hit/release on Continue without a clock jump. Actual drawable sizes were 1600×900 (16:9, level 1), 1680×720 (21:9, level 2), and 1200×900 (4:3, level 3), using OpenGL/4× MSAA. Level 4 passed at 1600×900 using GLES/zero MSAA and replayed its reached `galata_ucus` movie to check missing-WAV completion and repeated registration. A classic-graphics level-1 smoke also passed.

An isolated integration exercise additionally invoked the real `save_game` host, checked the raw callable prefix, restored through the actual checkpoint menu, and invoked `son` through the real blocking SDL dialog. Keyboard tab/text/Enter generated a local code from synthetic digits; Escape closed it; its score handoff was removed and host capture restored. Real window resizing to 1000×700 and F11 fullscreen/windowed transitions passed. COSMIC selected 1280×924 on return to windowed mode; the renderer followed that actual size. Synthetic focus events exercised SDL relative-mode release/reacquisition and held-key clearing.

Game-only visual evidence: [level 1](analysis/native-level1.png), [level 2](analysis/native-level2.png), [level 3](analysis/native-level3.png), [level 4](analysis/native-level4.png), [ending](analysis/native-ending.png), [resize](analysis/native-resized.png), [fullscreen](analysis/native-fullscreen.png), and [windowed return](analysis/native-restored.png).

Enhanced graphics evidence: [16:9](analysis/native-enhanced-16x9.png), [classic comparison](analysis/native-classic-16x9.png), [21:9](analysis/native-enhanced-21x9.png), [4:3](analysis/native-enhanced-4x3.png), and [GLES/zero-MSAA level 4](analysis/native-enhanced-level4.png). A live enhanced-renderer exercise verified resize to [1000×700](analysis/native-enhanced-resized.png), F11 [fullscreen at 2560×1440](analysis/native-enhanced-fullscreen.png), and [windowed return at 1680×720](analysis/native-enhanced-restored.png). Actual [menu](analysis/native-enhanced-menu.png) and decoded [intro](analysis/native-enhanced-intro.png) framebuffers were byte-identical between enhanced and classic graphics at 21:9. These images contain only the native game framebuffer, not the desktop.

Ayarlar verification passed on both OpenGL and GLES using real SDL motion/press/release events: original menu entry, all eight native controls, unchanged original brightness/sensitivity, fullscreen/F11 synchronization, actual level startup, changing MSAA/filtering with world textures already loaded, and Back/Continue. See the [original-style graphics panel](analysis/native-ayarlar-graphics.png), [changed settings](analysis/native-ayarlar-changed.png), [fullscreen](analysis/native-ayarlar-fullscreen.png), [GLES panel](analysis/native-ayarlar-gles.png), and world rendering [before](analysis/native-ayarlar-world.png)/[after live quality restoration](analysis/native-ayarlar-world-restored.png). Actual application restarts on levels 2–4 verified saved strengths, selective CLI overrides, and subsequent persistence. Permanent menu checks cover release timing, bounds, hardware-limited choices, legacy/native settings parsing, malformed extensions, and round-trip precision; GPU checks cover live classic/enhanced restoration and 4×→off→4× MSAA transitions.

A separate actual main-loop window-close exercise persisted an explicit MSAA override while retaining the saved AO strength. Live MSAA changes reuse existing full-size resolve/effect textures; only multisample attachments are reallocated, and unused multisample colour storage is released when MSAA is disabled.

Launcher SDL interaction passed on both OpenGL and GLES: all 13 keyboard-adjustable settings, mouse-release ordering, focus-loss click cancellation, saved fullscreen deferred while the launcher stays windowed, Play/Escape, and shared settings persistence without creating checkpoints. The actual startup main loop also passed launcher-only window close without audio/intro/level initialization, then a separate Play-to-level-1 run with CLI-seeded AO edited to 32%, fullscreen handoff, original Continue, and normal-close persistence. A temporary install with original data ran from `/tmp` without an asset-root override: actual 123-tick level-2 OpenGL/21:9 and level-4 GLES smokes passed. Permanent menu checks cover Unicode settings paths, replacement of existing settings, and preservation/temporary cleanup after a failed write. See [launcher](analysis/native-launcher.png), [GLES launcher](analysis/native-launcher-gles.png), and [actual launched game](analysis/native-launcher-started-game.png). Windows/macOS remain unverified.

The 1.0.1 gameplay fixes correct the shared strafe-axis signs (A/Left screen-left, D/Right screen-right at four headings), lazily open streaming audio only when played, decode native mono/stereo once, and reuse verified directory prefixes within one read-only `SceneCache` lifetime. Resource leaf checks, traversal/symlink rejection, and case ambiguity checks remain intact. All 12 checks pass, including strafe, asset-cache, deferred-stream, and real OpenGL/GLES pavement hit/miss/energy bounds with live 4×→off→4× MSAA. Actual 123-tick Wayland smokes passed on levels 1–3 with OpenGL and level 4 with GLES, including the original NewGame click, movie/stream restart, native input, and shared settings. See [diffuse pavement](analysis/native-pavement-rough.png) and [GLES](analysis/native-rough-gles.png).

Warm local measurements of the original-menu NewGame path, not the user's cold-storage latency:

| Measurement | Before | After |
|---|---:|---:|
| Startup frame, ms | 221.9 | 91.2 |
| NewGame load frame, ms | 40.3 | 26.8 |
| NewGame filesystem metadata calls | 11544 | 4286 |

A live private-repository probe used the stored `gh` credential and observed the authenticated repository response; cancelling a pending check returned in 20 ms. The repository is now public. [Linux v1.0.1](https://github.com/sergen213/etiyami/releases/tag/v1.0.1) was built and published by [run 37545910254](https://github.com/sergen213/etiyami/actions/runs/37545910254): both x86_64/arm64 glibc-2.35 packages passed extraction/architecture policy checks and local SHA256 matched GitHub's digests. Each ZIP contains 28 flat ELFs: three executables and 25 application libraries, no original assets or host C++/graphics runtimes.

A genuine Ubuntu-built 1.0.0 launcher automatically downloaded, verified, installed, and restarted public 1.0.1 in 4.45 seconds with credential variables removed and an empty private PATH containing no `gh`. All 13 settings, original-data/checkpoint sentinels, and legacy runtime bytes were preserved. The trusted installed helper was used, the actual restarted 1600×900 launcher showed 1.0.1/current, its process maps loaded host C++ runtimes rather than the retained legacy `lib/`, and the completed stage was removed. See its [actual GitHub-updated framebuffer](analysis/native-launcher-github-updated.png). The installed CI x86_64 release also passed actual 123-tick OpenGL level-2 and GLES level-4 NewGame/world/movie smokes.

The withdrawn 1.0.0 CI package bundled an older C++ runtime that could prevent EGL startup on newer Mesa. For the legacy-upgrade proof, those genuine runtime files were temporarily quarantined only during old-window initialization, then restored immediately after its first real GL present, before update/helper creation. The old helper loaded the restored old copies; restarted 1.0.1 ignored them through its flat `$ORIGIN` layout. An already affected 1.0.0 install that cannot open must be repaired by extracting the fixed 1.0.1 package into the same installation; an executable that cannot initialize its old window cannot begin that old release's automatic update. ARM64 packaging/build is CI-verified, not physical graphics execution. Windows/macOS releases remain deferred.

Smokes require a real desktop and focus. They use a newly created temporary save directory unless `--save-dir` explicitly overrides it. Injected SDL input and capture-state checks do **not** establish physical mouse hardware behavior. Full campaign progression has not been manually played end-to-end.

## Original-media limitations

The shipped definitions reference three absent audio samples: `env/araba_alarm_basla.mp3`, `env/hayvan_kus1.mp3`, and `env/hayvan_kus2.mp3`. Reached movies `galata_ucus` and `sahaf_kitap_alma` also lack their WAV files. These remain explicitly unavailable; videos still run, and no replacement sound or fabricated PCM is supplied. Missing original images retain the original white-image behavior and emit diagnostics. Missing AVI references are reported; attempting to play an absent video fails rather than showing a fake substitute.

Empty mesh-bound sentinels (±999999) and repeated animation names are authentic data, not discarded corruption: signed bounds and every animation ordinal remain intact. Decompiler output alone was not used as a claim of source correctness; object layouts, instruction behavior, and external boundaries were reconstructed separately.

## Original-engine reference

The extracted original engine runs under system Wine 11.19 on COSMIC Wayland. The patched reference displayed the introductory video, menu, and first playable level. Wine input injection exercised relative motion and a left-button release on New Game. Physical mouse behavior has not been independently verified after the backend change.

```sh
./play-reference.sh
```

This optional comparison launcher uses `.wine-yami`. It selects Wine's native Wayland backend and unsets `DISPLAY` on Wayland; otherwise it selects X11. The Wine Wayland branch was exercised; its X11 launcher branch is unverified. The reference retains 1024×768 and is **not** the native engine or graphics upgrade. Its legacy executable may write settings/checkpoints in its working directory.

For extraction into a fresh working directory:

```sh
./extract_game.py
./prepare_reference.py
./play-reference.sh
```

Extraction needs `7z` and `cabextract` and refuses to overwrite an existing `game/`. `prepare_reference.py` checks the full original hash and creates a separate `game/eti-reference.exe`.

### Verified original defects and patches

Original `game/eti.exe` SHA-256:

`361335a4241ba1165f700d9cb61accdec313710becf0a291dec1d1f9300f119e`

| Address | Evidence | Reference change |
| --- | --- | --- |
| `004327eb` | Startup attempts a legacy exclusive display-mode change. | Skip that mode switch. |
| `00432821` | Window creation is passed the fullscreen flag. | Select the windowed path. |
| `004329dc` | Pixel-format selection increments the `ChoosePixelFormat` result before `SetPixelFormat`. | Remove the increment. |
| `00433110`, `00433118` | Decorated-window dimensions ignore the adjusted outer rectangle; the client is smaller than the fixed viewport. | Use a borderless 1024×768 client. |
| `00432ff9` | `WM_ACTIVATE` accepts `WA_ACTIVE` (1), not `WA_CLICKACTIVE` (2). Deactivation releases DirectInput devices; mouse-click activation fails to reacquire them. | Treat both nonzero activation states as active. |

`check_activation.c` executes the real 32-bit handler instructions and their original state-global writes. Original activation state 2 failed; the patched handler passed states 0, 1, and 2:

```sh
gcc -m32 -no-pie -Wl,-Ttext-segment=0x10000000 -Wall -Wextra -o /tmp/yami-activation-check check_activation.c
/tmp/yami-activation-check game/eti.exe           # expected failure: activation 2
/tmp/yami-activation-check game/eti-reference.exe # passes states 0, 1, and 2
```

This trusted-release regression check needs Linux x86 compatibility and multilib; it is not a general executable loader.

### Installer and recovered evidence

The MSI's `indeo` and `directx` custom actions invoke `iv5setup.exe` and `dxsetup.exe` whenever the product is not installed. Their presence is proven; the exact point of the reported Proton installer stall is not. Static extraction bypasses the whole installer without guessing its hang stage.

The original renderer is fixed-function OpenGL, not Direct3D. Its boundaries include WGL/GDI/USER32, DirectInput 8, AVIFile, GLEW 1.3.3, and FMOD 3.7.5. Its intro is Indeo 5, 512×384 at 12.5 fps. `game.ini` contains brightness, sensitivity, music/effect volume, and aiming mode, not resolution. `son.eti` is a PE32 executable, not a save.

- `analysis/Yami.gpr`: persistent Ghidra project.
- `analysis/TraceStartup.java`: startup, display, and input evidence.
- `analysis/RecoverEngine.java`: recovered functions, instructions, strings, and calls.
- `analysis/recovered/functions/`: 1,835 decompiled functions, including CRT routines.
- `analysis/recovered/functions.tsv`, `calls.tsv`, `strings.tsv`, `disassembly.txt`: address-level evidence.
- `analysis/wayland-click-verified.png`: original-engine first-level gameplay reached through the menu.
- `native/` and `CMakeLists.txt`: compilable reconstruction, separate from those decompiler exports.

Loader entry points include model `0040f1b0`, mesh `00420c40`, animation `00401760`, compiled script `0040dbf0`, script-object construction `0040d9a0`, and script host registration `00415db0`. Formats mix little-endian words with whitespace-delimited names. Zero decompiler failures did not establish correct prototypes; wrong calling conventions, erased layouts, and incomplete indirect targets required additional recovery.
