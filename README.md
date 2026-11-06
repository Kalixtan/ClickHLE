# ClickHLE

ClickHLE is an early-stage emulator for click-wheel iPod games. It runs decrypted game executables
with Unicorn Engine and includes a command-line frontend and an optional Qt desktop interface.
Compatibility is incomplete and varies between games.

Build with CMake 3.20 or newer and a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build --parallel
```

The command-line program is `build/bin/clickhle`. When Qt 6 Widgets is available, CMake also builds
`build/bin/clickhle-gui`. Unicorn Engine 2 is required at runtime; SDL3 and OpenGL add video and
audio support. Dependencies, game images, firmware, and reference assets are not bundled. See
[LICENSING.md](LICENSING.md) for dependency notes.
