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

The GUI lists games from `hdd/iPod_Control/Games_RO` beside its executable.
Select a platform at the top of the list; the package manifest chooses its
executable. File → Install Game Package installs `.ipg` or `.zip` packages into that drive.
Plaintext eapps need no key. Encrypted eapps require a valid 32-byte `.ikey` file
in a `keys` folder beside the executable. Package installation is built into
the GUI and CLI using libzip and OpenSSL. The CLI command is
`build/bin/clickhle --install game.ipg [hdd-folder] [keys-folder]`. The CLI
accepts `--platform 1` through `4` when a binary is shared by platforms.
Alternate-platform eapps parse and map, but APIs with different ABI hashes
remain unbound and stop when called.

Installed games read assets from `hdd/iPod_Control/Games_RO/<id>` and write
saves to `GameData_RW/<id>` and statistics to `GameStats_WO/<id>` under the
same `iPod_Control` folder. Shared game data uses `GameData_ShareRW`.
Loose executables also put their saves under this HDD, using their filename as
the directory name. Metadata reads `hdd/iPod_Control/iTunes/iTunesDB`; music
paths in that database resolve under `hdd/iPod_Control/Music`. The database
reader supports tracks and playlists, but most RetailOS Metadata calls still
need their ABI contracts traced before games can browse a populated library.

Graphics use a native C++ VideoCore II core running original firmware. The LLVM JIT is the
default when available; the interpreter can be selected with `--vc2-engine interpreter` or under
Options → VideoCore Engine in the GUI when LLVM is available at build time. Instructions the
JIT cannot yet compile run in the interpreter. Native HLE builds
its command packets; games do not execute RetailOS ARM code or require `osos.bin`. Both frontends
display the raw framebuffer, with no shader replacement or Python runtime. Place the game
RenderServer firmware beside the executable as `rserver.bin`. `CLICKHLE_VC2_FIRMWARE`
can override that path; games may also supply their own image.
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
