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
`build/bin/clickhle-gui`. Unicorn Engine 2 is required at runtime; SDL3 adds window, input, and audio support. Dependencies, game images, firmware, and reference assets are not bundled. See
[LICENSING.md](LICENSING.md) for dependency notes.

Graphics use a native C++ VideoCore II interpreter running original firmware. Native HLE builds
its command packets; games do not execute RetailOS ARM code or require `osos.bin`. Both frontends
display the raw framebuffer, with no shader replacement or Python runtime. Place the game
RenderServer firmware beside the executable as `rserver.bin`. `CLICKHLE_VC2_FIRMWARE`
can override that path; games may also supply their own image.
Built-in pipeline programs also need locally extracted VC2 records (not distributed).
Unsupported APIs and instructions stop execution, and games are not yet expected to render
correctly. Normal play writes no diagnostic captures. Headless tests can opt in with
`--headless --capture-videocore`; captures go to `work/vc2` (or `CLICKHLE_VC2_OUTPUT`).
