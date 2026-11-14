# ClickHLE

ClickHLE is a hobbyist emulator for games made for click-wheel iPods. It is an
early project: compatibility and graphics support are incomplete, and a game
loading successfully does not mean it is fully playable.

## Build

Use CMake 3.20 or newer and a C++20 compiler. On Linux, install the compiler,
SDL3, Qt 6 Widgets, OpenSSL, and libzip development packages, then run:

```sh
cmake -S . -B build
cmake --build build --parallel
```

On Windows, use the MSYS2 MinGW 64-bit shell and install the toolchain and
libraries:

```sh
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-cmake \
  mingw-w64-x86_64-ninja mingw-w64-x86_64-qt6-base \
  mingw-w64-x86_64-sdl3 mingw-w64-x86_64-openssl mingw-w64-x86_64-libzip
cmake -S . -B build -G Ninja
cmake --build build
```

CMake builds the command-line program as `clickhle` and, when Qt 6 Widgets is
available, the desktop program as `clickhle-gui`. LLVM enables the optional
VC2 JIT when a compatible LLVM development package is found. Unicorn 2 is
loaded at runtime and must also be available as `unicorn.dll` on Windows.

## Games and virtual drive

The GUI can install `.ipg` and `.zip` game packages into the `hdd` folder beside
the program. Plaintext packages need no key. For encrypted packages, place the
matching raw 32-byte `.ikey` file in the neighboring `keys` folder before
installing. The installer checks that the key decrypts a valid game image.
You can also use File → Load Game for a decrypted executable; keep its game
data where the title expects it.

ClickHLE creates an empty iPod drive on first launch. Installed games go under
`hdd/iPod_Control/Games_RO`; saves and statistics go under `GameData_RW` and
`GameStats_WO`. ClickHLE creates a minimal empty `iTunes/iTunesDB` understood
by its music reader and leaves an existing database untouched. It is not a
full RetailOS-generated catalog. Add your own music files and matching database
if a title needs a populated library.

Platform 1 games also need the matching `rserver.bin` RenderServer firmware
beside the program, or at the path in `CLICKHLE_VC2_FIRMWARE`. ClickHLE does
not include that firmware, game packages, or decryption keys.

## Platform support

| Platform ID | iPod family | Current state |
| --- | --- | --- |
| 1 | 5G and 5.5G | Main implementation. ARM APIs and the VC2 interpreter/JIT run a number of titles, but coverage, speed, and graphics accuracy remain incomplete. |
| 2 | Not confirmed | No platform-specific implementation. Currently routed through the experimental Platform 3 API as a placeholder. |
| 3 | 7G | Early implementation based on the 7G OSOS. The software graphics path and optional desktop OpenGL backend are incomplete; many games still stop or draw incorrectly. |
| 4 | Not confirmed | No platform-specific implementation. Currently routed through the experimental Platform 3 API as a placeholder. |

The Platform 2 and 4 device mappings have not been established from the project’s
verified firmware and game evidence. Their placeholder path should not be read
as support for those devices.

## Code and assets

No Apple firmware, game executables, game artwork, music, or encryption keys are
distributed in this repository. The source code is copyrighted: ClickHLE’s
original code is MIT-licensed, and the VC2 core includes GPL-2.0-or-later code
derived from ProkBox with its license notice retained. So it would be inaccurate
to claim that the repository contains no copyrighted code; it contains no
proprietary game or firmware data. See [LICENSING.md](LICENSING.md) and
[THIRD_PARTY.md](THIRD_PARTY.md) for details.

The VC2 JIT was written by AI using the author’s hand-written interpreter and
reverse-engineering notes as its basis. It is an experimental effort to make
games run closer to real time, and its instruction behavior still needs testing.
