# crashwoc-android

Unofficial Android port of *Crash Bandicoot: The Wrath of Cortex*.

> ⚠️ This project is not affiliated with, endorsed by, or sponsored by
> Activision, Universal Interactive, or any rights holder. All trademarks
> belong to their respective owners.

## About

crashwoc-android brings *The Wrath of Cortex* to Android devices. The goal
is a playable, well-performing port with touch and gamepad support.

## Features

- Runs natively on Android ([min. Android version])
- Touch controls and Bluetooth/USB gamepad support
- [OpenGL ES / Vulkan] renderer
- [Save data support, resolution scaling, etc.]

## Requirements

- Android [version] or newer
- A device with [ARM64 / ARMv7] architecture
- **Your own copy of the original game files.** This repository does NOT
  include or distribute any copyrighted game assets.

## Installation

1. Download the latest APK from the [Releases](../../releases) page.
2. Install the APK on your device.
3. Copy your game files to `[path on device]`.
4. Launch the app.

## Building from Source

```bash
git clone https://github.com/[username]/crashwoc-android.git
cd crashwoc-android
./gradlew assembleRelease
```

Requirements: Android Studio, Android NDK [version], CMake [version].

## Status

- [x] [Completed feature]
- [ ] [Work in progress]
- [ ] [Planned feature]

## Contributing

Issues and pull requests are welcome. Please open an issue first to
discuss major changes.

## Legal

This is a fan-made, non-commercial project for educational purposes.
No copyrighted assets are included. You must own a legitimate copy of the
game to use this software.

## License

[Choose a license, e.g. MIT / GPL-3.0] — see [LICENSE](LICENSE).
