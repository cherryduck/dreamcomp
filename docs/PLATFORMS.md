# Platforms

| Platform | Toolchain | Graphics | Status |
|---|---|---|---|
| Windows x64 | clang-cl + Ninja inside VS 2022 Build Tools; `tools/dc.py build` | Vulkan (Vulkan SDK for `glslc`) | **works** (Soulcalibur, Iris Xe) |
| Linux x64 | gcc 15/16 / clang; `tools/dc.py build` or `tools/build_linux.sh` | Vulkan; needs SDL3, Vulkan headers, `glslc` (Arch: `sdl3 vulkan-headers shaderc`) | **works** natively (CachyOS, g++ 16, RTX 5090, Wayland; Ecco, 2026-10-15). Profiler and PresentMon pacing are still Windows-only |
| macOS ARM64 | Apple clang | Vulkan via MoltenVK (the engine's primary development platform) | engine CI; not verified for dreamcomp here (no Mac) |
| Android ARM64 | NDK + Gradle, SDL3's Android glue | Vulkan | planned; no SDK/NDK on this machine |

## Linux (WSL) — what Daniel needs to run once

```sh
sudo apt install libsdl3-dev glslc     # or: shaderc
```

Then `wsl -d Ubuntu -- bash tools/build_linux.sh ports/<slug>` builds the window version.

## Android plan

The game code is translated once on the build host; only compiling the generated C++ and the
runtime happens for the device. Work items:

1. **Host translator**: run `dream-translate` from a host build and feed its output to the
   cross build (`DREAM_TRANSLATE_EXECUTABLE`-style cache variable instead of the in-tree target).
2. **Engine portability fixes**: `<execinfo.h>` backtraces only exist from API 33 (guard by API
   level); `fenv.cpp` already supports ARM64 FPCR.
3. **Entry point**: SDL3's `SDL_main` with the launcher's `main(argc, argv)`; the dreamcomp core
   extension supplies `--config` from the APK assets copy and `--disc` from settings.
4. **Disc access**: Storage Access Framework picker → content URI → file descriptor; the GD-ROM
   readers use `FILE*` today, so open via `fdopen` on the descriptor SDL3 returns.
5. **Lifecycle**: pause the guest on `SDL_EVENT_WILL_ENTER_BACKGROUND`, recreate the Vulkan
   surface on resume.
6. **Input**: SDL3 gamepad already maps; touch overlay for phones without a pad (later).
7. **Packaging**: Gradle project (planned location: a platform/android folder), APK built per port; no game data in
   the APK.

Needs: Android SDK + NDK r27+ (Android Studio or command-line tools, several GB) — Daniel's call.
