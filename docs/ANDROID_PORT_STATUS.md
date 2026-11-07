# Android port status

## Goal

Port NuWorld to a native Android application with Android 10 (API 29) as the minimum supported OS and 64-bit ARM (arm64-v8a) as the initial ABI.

This document records the current porting baseline. It does **not** claim that an Android APK can already be built or that the game is playable on Android.

## Repository audit

The current root build is still a desktop-oriented CMake application:

- The application target is declared with `add_executable(crashwoc-nuworld ...)`.
- SDL3 is fetched from the pinned `release-3.4.16` tag.
- Dear ImGui is built with its SDL3 and **OpenGL 3** backends.
- Platform startup requests an OpenGL 3.3 core context.
- The VFS and save layers use SDL filesystem APIs; Android needs an app-private writable data root and a supported way to import a user-owned disc image.
- The current desktop startup flow includes command-line import/verify/boot paths. Android needs an activity/lifecycle-aware entry path and an in-app import flow rather than relying on command-line arguments.

The upstream README also states that NuWorld is not yet intended for normal play and that the game currently starts with `--boot`. Android packaging alone cannot make unfinished gameplay code ready to play.

## Android port requirements

1. **Native entry point and lifecycle**
   - Add an Android Gradle project and SDL3 Android Java/activity integration.
   - Build the native application as an Android shared library using SDL3's supported Android entry-point pattern.
   - Handle pause/resume, focus changes, surface recreation, and audio shutdown.
2. **Graphics**
   - Replace or abstract the desktop-only OpenGL 3.3 core requirement.
   - Select a mobile-supported rendering path (OpenGL ES 3.x or Vulkan) and port the renderer and ImGui backend consistently. This is a renderer compatibility task, not just a manifest setting.
3. **Storage and game import**
   - Use Android's Storage Access Framework for selecting the user's own `.iso` or `.bin/.cue` image.
   - Copy/import into app-private storage; do not request broad storage access.
   - Keep saves and imported game data under SDL's Android-compatible preference/data directory.
4. **Input and UI**
   - Add touch controls for the game and ensure SDL gamepad input remains available for Bluetooth/USB controllers.
   - Make setup, import progress, errors, and settings usable on phone screens.
5. **Build and validation**
   - Minimum SDK 29; initial ABI `arm64-v8a`.
   - Pin Android Gradle Plugin, Gradle, NDK, and CMake versions.
   - Build a debug APK in CI, then test installation, launch, import, rendering, audio, lifecycle, touch, controller, and save persistence on real devices.
   - Add other ABIs only after arm64 is stable.

## Suggested implementation order

- Phase A: introduce the Android Gradle/SDL3 project and get a minimal native SDL window/activity running on API 29+.
- Phase B: make the graphics backend mobile-compatible and build the existing frontend.
- Phase C: implement SAF import and Android data paths.
- Phase D: implement touch controls and lifecycle/audio handling.
- Phase E: integrate and validate game boot/runtime, then publish APK artifacts.

## Current status

- [x] Dedicated branch created: `android/port-foundation`.
- [x] Initial source/build audit recorded.
- [ ] Android Gradle project and SDL3 activity integration.
- [ ] Android-compatible renderer.
- [ ] SAF import and app-private storage integration.
- [ ] Touch controls and lifecycle handling.
- [ ] Successful Android CI build.
- [ ] Device testing and playable game validation.

## Important limitation

A successful APK build would only prove that the native app packages and starts. It would not prove that the game itself is playable. Do not describe this port as complete until the game runtime and the device test checklist pass.
