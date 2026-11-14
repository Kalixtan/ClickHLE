# ClickHLE

ClickHLE is a hobbyist emulator for click-wheel iPod games. It is an early work in progress: compatibility and graphics support vary, and many games do not yet run correctly. Game files and firmware are not included.

Build with CMake 3.20 or newer and a C++20 compiler:

```sh
cmake -S . -B build
cmake --build build --parallel
```

The command-line frontend is built as `clickhle`; when Qt 6 Widgets is installed, CMake also builds `clickhle-gui`. Unicorn Engine is required at runtime. SDL3, Qt 6, LLVM, libzip, OpenSSL, and FFmpeg are optional dependencies. See [LICENSING.md](LICENSING.md) and [THIRD_PARTY.md](THIRD_PARTY.md) for license details.

The VC2 JIT was written by AI using the author's hand-written interpreter and reverse-engineering notes as its basis. It is an experimental effort to run games closer to real time; instruction and graphics behavior still need validation.

Platform 1 uses the VC2 interpreter/JIT path. Platform 3 has an experimental desktop OpenGL backend, and support for other platforms is preliminary. Local firmware, game dumps, and working notes belong outside version control; `.gitignore` excludes the usual asset and documentation folders.
