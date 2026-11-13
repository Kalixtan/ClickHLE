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
`build/bin/clickhle-gui`. Unicorn Engine 2 is required at runtime; SDL3 adds
window, input, and audio support. WAV effects use the game's PCM buffers;
AAC/MP3 music also needs FFmpeg at build time. Dependencies, game images,
firmware, and reference assets are not bundled. See
[LICENSING.md](LICENSING.md) for dependency notes.

The GUI lists games from `hdd/iPod_Control/Games_RO` beside its executable.
Select a platform at the top of the list; the package manifest chooses its
executable. The list shows each game's version, ID, build, executable size,
and platform version. File → Install Game Package installs `.ipg` or `.zip`
packages into that drive.
Plaintext eapps need no key. Encrypted eapps require a valid 32-byte `.ikey` file
in a `keys` folder beside the executable. Package installation is built into
the GUI and CLI using libzip and OpenSSL. The CLI command is
`build/bin/clickhle --install game.ipg [hdd-folder] [keys-folder]`. The CLI
accepts `--platform 1` through `4` when a binary is shared by platforms.
Platform 3 support is experimental and does not yet display games. Platforms 2
and 4 use the same preliminary API path as a placeholder. Unimplemented APIs
stop execution; VideoCore emulation is available only for Platform 1.

Platform 1's graphics code lives in `src/video/platform1`, including the VC2
interpreter in `vc2` and JIT in `vc2/jit`. The preliminary Classic API and
display code live in `src/hle/platform3` and `src/video/platform3` respectively.

Installed games read assets from `hdd/iPod_Control/Games_RO/<id>` and write
saves to `GameData_RW/<id>` and statistics to `GameStats_WO/<id>` under the
same `iPod_Control` folder on the frontend's HDD, even if the executable was
opened from another drive. Shared game data uses `GameData_ShareRW`.
Loose executables also put their saves under this HDD, using their filename as
the directory name. Metadata reads `hdd/iPod_Control/iTunes/iTunesDB`; music
paths in that database resolve under `hdd/iPod_Control/Music`. The database
reader supports tracks and playlists, but most RetailOS Metadata calls still
need their ABI contracts traced before games can browse a populated library.

Graphics use a native C++ VideoCore II core running original firmware. G5 with LLVM JIT fallback is the
default when LLVM is available; the interpreter can be selected with `--vc2-engine interpreter` or under
Options → VideoCore Engine in the GUI when LLVM is available at build time. Instructions the
JIT cannot yet compile run in the interpreter. Native HLE builds
its command packets; games do not execute RetailOS ARM code or require `osos.bin`. Both frontends
display the raw framebuffer, with no shader replacement or Python runtime. Place the game
RenderServer firmware beside the executable as `rserver.bin`. `CLICKHLE_VC2_FIRMWARE`
can override that path; games may also supply their own image.
The default `--vc2-engine g5` mode (Options → VideoCore Engine → G5 in the GUI)
uses independent high-level replacements for matrix API calls 165–166, 169–172 and 175–176.
Other graphics calls follow the normal packet and VC2 JIT path. New replacements
belong in `src/video/g5/` and should be added only with evidence and matching
return values and guest-memory effects. G5 is an experimental path toward a
larger software renderer; it currently uses VC2 for drawing.
The GUI's Options → iPod Settings window controls the game-facing language
index, time format, and hold switch. Language index 0 is English; available
translations depend on the game.
The JIT keeps compiled objects in the user cache (`~/.cache/clickhle/vc2` on Linux).
Set `CLICKHLE_VC2_CACHE_DIR` to choose another folder, or to an empty string to
disable disk caching. Deleting the folder clears the cache.
Built-in pipeline programs also need locally extracted VC2 records (not distributed).
Unsupported APIs and instructions stop execution, and games are not yet expected to render
correctly. Normal play writes no diagnostic captures. Headless tests can opt in with
`--headless --capture-videocore`; captures go to `work/vc2` (or `CLICKHLE_VC2_OUTPUT`).

For repeatable headless input tests, use `--replay-file replays/port-example.txt`.
Each line holds a raw `0x7000c140` input-port word for a number of port reads:
`120 0x8000001a` or `3 0x8000011a` (Select held). Button bits are 8–13,
the wheel position is in bits 16–22, and bit 30 signals touch. This override
is available in headless mode only; GUI input supplies the same port word.
The emulator currently samples this word in its RetailOS input-worker model;
one headless port read occurs before each game callback, and GUI changes are
sampled immediately so a short button tap is not lost at low frame rates.
The model does not include the clickwheel controller's serial pins or
device-ready register.
