# ClickHLE

ClickHLE is an early-development emulator for iPod click-wheel games. It runs decrypted game images with Unicorn Engine and provides command-line and optional Qt desktop frontends. Compatibility is incomplete and varies by game; this project does not include games or firmware.

Build with CMake 3.20 or newer and a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build --parallel
```

The command-line program is `build/bin/clickhle`; use `--help` for its options. The GUI is built as `build/bin/clickhle-gui` when Qt 6 Widgets is available. SDL3 and OpenGL enable video and audio. Unicorn Engine 2 must be installed separately for game execution; the build does not download or bundle dependencies. See [LICENSING.md](LICENSING.md) for dependency notes.
