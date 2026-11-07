# CrashWOC Android — Phase A

This is the first native Android packaging milestone, not a playable game build.

## Target

- Android 10 / API 29 minimum
- arm64-v8a only
- Android Gradle Plugin 8.7.3
- Gradle 8.9
- JDK 17
- Android NDK 28.2.13676358
- CMake 3.25.2
- SDL 3.4.16 Android AAR, downloaded from the official SDL release during the Gradle build

## Build a debug APK

Install Android SDK platform 35, build-tools, NDK 28.2.13676358, and CMake 3.25.2. Then run from this directory:

```sh
gradle assembleDebug
```

The first build downloads the official SDL3 Android development archive and extracts its AAR into the local build output. The archive is not committed to this repository.

The APK output is:

```
app/build/outputs/apk/debug/app-debug.apk
```

Install it on an arm64 Android device with:

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

## What this milestone proves

The current native target is deliberately a small SDL3 smoke-test shell. It creates an Android fullscreen surface, initializes SDL video/audio/gamepad subsystems, and presents frames. It is intended to validate the Android toolchain and SDL activity integration.

## What it does not prove

- The NuWorld frontend or game runtime is not linked into this APK yet.
- The desktop renderer uses OpenGL 3.3-only features and is not yet GLES 3 compatible.
- Disc import, Storage Access Framework, touch gameplay controls, save paths, and actual gameplay are not implemented in this phase.
- No APK/device test is claimed until a CI or physical-device build succeeds.

The next phases are renderer compatibility, app-private data paths and SAF import, then touch/controller input and game runtime integration.
