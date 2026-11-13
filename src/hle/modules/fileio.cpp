// The "AsyncFileIO" and "Filesytem" libraries: 17 and 4 ordinals.
//
// `Filesytem` is Apple's spelling, not a typo here.
//
// ## What the games ask for
//
// Established by probing, which is the only way it could have been: the games
// build their paths at run time, so the strings are never in the image and
// static analysis cannot see them.
//
//     Lost / Sims Bowling / Sims Pool:  #3(0, "rserver.bin", ptr, ptr, ...)
//     Sudoku:                           #3(1, "savefile.dat", ptr, ptr, ...)
//
// `rserver.bin` sits in each game's directory -- beside `Executables/`, not
// inside it -- which is what fixed the VFS root. The leading argument is 0 for
// the asset every title reads and 1 for the one title that writes a save, so it
// is the mode.

#include <algorithm>
#include "clickhle/hle/modules.h"

#include <filesystem>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <vector>
#include <utility>

#include "clickhle/common/log.h"
#include "clickhle/core/emulator.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/registry.h"
#include "clickhle/hle/vfs.h"

namespace clickhle {
namespace {

constexpr std::string_view kCategory = "file";
constexpr const char* kModule = "AsyncFileIO";

/// Apple spelled it without the "s". Matched literally against the image.
constexpr const char* kFilesystemModule = "Filesytem";

/// Longest guest path we will read. FAT names are short; this only bounds a
/// bad pointer.
constexpr u32 kMaxPath = 256;

constexpr u32 kFileObjectResult = 0x08;

std::optional<std::string> read_path_strict(HleCallFrame& frame,
                                             GuestAddr address) {
    std::string out;
    for (u32 i = 0; i < kMaxPath; ++i) {
        const auto c = frame.memory.read<u8>(address + i);
        if (!c.has_value()) return std::nullopt;
        if (*c == 0) return out;
        out.push_back(static_cast<char>(*c));
    }
    return std::nullopt;
}

std::optional<Vfs::Namespace> file_namespace(u32 mode) {
    switch (mode) {
    case 0: case 4: return Vfs::Namespace::kGame;
    case 1: return Vfs::Namespace::kData;
    case 2: return Vfs::Namespace::kStats;
    case 3: return Vfs::Namespace::kShared;
    default: return std::nullopt;
    }
}

Vfs::Mode open_mode(Vfs::Namespace name_space) {
    if (name_space == Vfs::Namespace::kGame) return Vfs::Mode::kRead;
    if (name_space == Vfs::Namespace::kStats) return Vfs::Mode::kWrite;
    return Vfs::Mode::kUpdate;
}

template <typename T>
bool store_guest_or_stop(HleCallFrame& frame, GuestAddr address, T value) {
    if (frame.store<T>(address, value)) return true;
    frame.cpu.request_stop();
    return false;
}

}  // namespace

/// Request object fields, in bytes.
///
/// These offsets are ABI facts -- the shape of a structure the *games* build --
/// established by the DogParty project's reversing and reused here as facts.
/// None of their code is copied; see LICENSING.md section 5.
namespace request {
constexpr u32 kOperation = 0x04;   ///< byte: 3 = read, 4 = write, 5 = seek, 8 = remove
constexpr u32 kFileObject = 0x08;  ///< the game's file object
constexpr u32 kSeekOffset = 0x0C;
constexpr u32 kSeekWhence = 0x10;  ///< byte: 0 set, 1 current, 2 end
constexpr u32 kBuffer = 0x14;
constexpr u32 kLength = 0x18;
constexpr u32 kStatus = 0x20;      ///< 0 on success; failure uses op-specific status
constexpr u32 kSizeResult = 0x24;
constexpr u32 kPositionResult = 0x28;
constexpr u32 kDirectoryCount = 0x28;  ///< completed AsyncFileIO#7 entries
/// Where the firmware leaves the handle for the game to cache. Later requests
/// are resolved through *this* field, not through the file object, so an open
/// that does not write it leaves the 0xffffffff the game put there and every
/// following request names a stream that does not exist.
constexpr u32 kHandle = 0x2C;
}  // namespace request

constexpr u32 kFailure = 0xFFFFFFFFu;

/// #12 rejects a zero mode with 0x13. The store API is errno-style (0 =
/// success), unlike the queued slots' boolean result. #14/#16 use 8 for an
/// invalid slot instead.
constexpr u32 kStoreInvalidMode = 0x13;

/// #13/#14/#16 invalid-index result. It is distinct from #12's zero-mode
/// result and from kFailure, the unrelated queued-slot completion sentinel.
constexpr u32 kStoreGenericFailure = 0x08;

/// Dumps a request object.
///
/// Kept because the layout is still being learned: the two code pointers that
/// turned out to matter -- the completion trampoline at request+0x34 and the
/// game's callback at fileobject+0x0C -- were both found by reading these.
void dump_request(HleCallFrame& f, GuestAddr req) {
    std::string dump;
    for (u32 off = 0; off <= 0x5C; off += 4) {
        char buffer[24];
        std::snprintf(buffer, sizeof(buffer), " +%u=0x%x", off,
                      f.memory.read<u32>(req + off).value_or(0xdeadbeef));
        dump += buffer;
    }
    CLICKHLE_DEBUG(kCategory) << "request" << dump;

    const auto obj = f.memory.read<u32>(req + request::kFileObject);
    if (obj.value_or(0) != 0) {
        std::string odump;
        for (u32 off = 0; off <= 0x3C; off += 4) {
            char buffer[24];
            std::snprintf(buffer, sizeof(buffer), " +%u=0x%x", off,
                          f.memory.read<u32>(*obj + off).value_or(0xdeadbeef));
            odump += buffer;
        }
        CLICKHLE_DEBUG(kCategory) << "fileobj" << odump;
    }
}

void register_fileio(HleRegistry& registry) {
    // open(mode, path, request) -> nonzero on success.
    //
    // Arity 3: both Tetris and Lost set exactly r0-r2 at the call site.
    //
    // Asynchronous on the device -- the game fills in a request, the firmware
    // accepts it, a completion runs later. Performed at once here, which is
    // indistinguishable to a game that polls its own state and finds the work
    // already done.
    registry.add(kModule, 3, "afio_open_alt", 3, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32() & 0xFFu;
        const GuestAddr path_address = f.args.next_ptr();
        const GuestAddr req = f.args.next_ptr();
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (path_address == 0 || *operation != 6) {
            f.ret(0);
            return;
        }
        const auto path_value = read_path_strict(f, path_address);
        if (!path_value.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const std::string& path = *path_value;
        // The whole request, once per open. Cheap, and the alternative is
        // guessing which field mattered.
        {
            std::string dump;
            for (u32 off = 0; off <= 0x5C; off += 4) {
                const auto word = f.memory.read<u32>(req + off);
                dump += " +" + std::to_string(off) + "=0x";
                char buffer[16];
                std::snprintf(buffer, sizeof(buffer), "%x",
                              word.value_or(0xdeadbeef));
                dump += buffer;
            }
            CLICKHLE_DEBUG(kCategory) << "request" << dump;
            // The file object too. If the guest hands the firmware a completion
            // callback anywhere, it is a code pointer (0x18xxxxxx) sitting in
            // one of these two structures.
            const auto obj = f.memory.read<u32>(req + request::kFileObject);
            if (obj.value_or(0) != 0) {
                std::string odump;
                for (u32 off = 0; off <= 0x3C; off += 4) {
                    const auto word = f.memory.read<u32>(*obj + off);
                    char buffer[24];
                    std::snprintf(buffer, sizeof(buffer), " +%u=0x%x", off,
                                  word.value_or(0xdeadbeef));
                    odump += buffer;
                }
                CLICKHLE_DEBUG(kCategory) << "fileobj" << odump;
            }
            // What is already in the destination buffer decides whether this
            // open is delivering data or accepting it: a write has the game's
            // bytes there already, a read has zeros or stale heap.
            const auto buf = f.memory.read<u32>(req + request::kBuffer);
            const auto len = f.memory.read<u32>(req + request::kLength);
            if (buf.value_or(0) != 0 && len.value_or(0) != 0) {
                std::string bytes;
                for (u32 i = 0; i < std::min<u32>(*len, 32); ++i) {
                    char byte[4];
                    std::snprintf(byte, sizeof(byte), "%02x",
                                  f.memory.read<u8>(*buf + i).value_or(0));
                    bytes += byte;
                }
                CLICKHLE_DEBUG(kCategory)
                    << "buffer " << *len << "B at 0x" << std::hex << *buf
                    << std::dec << ": " << bytes;
            }
        }

        // mode 0 is an asset ("Strings.dta", "audio/bad.wav"); mode 1 is a
        // save ("savefile.dat", "pac_man.dat", "jdmgp.sav"). A save is opened
        // for update, not for writing -- see Vfs::Mode::kUpdate.
        const auto name_space = file_namespace(mode);
        if (!name_space) { f.ret(0); return; }
        Vfs& vfs = f.emulator.vfs();
        const auto vfs_mode = *name_space == Vfs::Namespace::kStats
            ? Vfs::Mode::kRead : open_mode(*name_space);
        const u32 handle = vfs.open(path, vfs_mode,
                                    vfs_mode != Vfs::Mode::kRead, *name_space);
        if (handle == 0) {
            if (!store_guest_or_stop(f, req + request::kStatus, kFailure) ||
                !store_guest_or_stop(f, req + request::kHandle, kFailure)) return;
            CLICKHLE_INFO(kCategory) << "open(\"" << path << "\") FAILED";
            // The request passed the wrapper's opcode/path gates and would
            // have been queued. Report accepted, then publish its failed
            // completion just as the worker does.
            f.emulator.queue_io_completion(req, path);
            f.ret(1);
            return;
        }

        const i64 size = vfs.size(handle);
        if (!store_guest_or_stop(f, req + request::kHandle, handle) ||
            !store_guest_or_stop(f, req + request::kStatus, 0u) ||
            !store_guest_or_stop(f, req + request::kPositionResult,
                                 static_cast<u32>(vfs.tell(handle)))) return;

        // An open that carries a destination buffer *is* the load.
        //
        // This is why Tetris opens Strings.dta and never issues a read: on the
        // device there was nothing left to read, the open did it. Treating the
        // open as submission only left the game waiting for a completion that
        // was never going to come, through a state field it had set itself.
        i64 loaded = 0;
        const auto buffer = f.memory.read<u32>(req + request::kBuffer);
        const auto length = f.memory.read<u32>(req + request::kLength);
        // Both modes load here. A save open carries an empty destination
        // buffer and expects its contents back, exactly as an asset open does.
        if (buffer.value_or(0) != 0 && length.value_or(0) != 0) {
            const i64 remaining = size - vfs.tell(handle);
            const u32 wanted = remaining <= 0
                                   ? 0u
                                   : std::min<u32>(*length,
                                                  static_cast<u32>(remaining));
            std::vector<u8> bytes(wanted);
            loaded = wanted == 0 ? 0 : vfs.read(handle, bytes.data(), wanted);
            if (loaded > 0 &&
                !f.memory.write_bytes(*buffer, bytes.data(),
                                     static_cast<u32>(loaded))) {
                loaded = -1;
            }
            CLICKHLE_DEBUG(kCategory)
                << "open loaded " << loaded << " of " << *length
                << " requested bytes";
        }
        // Opcode 6 is open + read + close. The read handler publishes the
        // transferred byte count, but does not write the position field.
        const u32 result = loaded < 0 ? 0u : static_cast<u32>(loaded);
        if (!store_guest_or_stop(f, req + request::kSizeResult, result) ||
            !store_guest_or_stop(f, req + request::kStatus,
                                 loaded < 0 ? kFailure : 0u)) return;

        (void)vfs.close(handle);
        CLICKHLE_INFO(kCategory)
            << "open/read/close(\"" << path << "\") -> handle " << handle
            << ", " << size << " bytes, transferred " << loaded;
        f.emulator.queue_io_completion(req, path);
        // The wrapper returns whether it accepted the request, not the handle
        // or the eventual operation status.
        f.ret(1);
    });

    // read(request) -- performs whatever the request describes.
    //
    // One argument, which is what settles the design: the operation, buffer,
    // length and position all live in the request object, so the object is the
    // contract and this is what advances it.
    registry.add(kModule, 2, "afio_read", 1, [](HleCallFrame& f) {
        const GuestAddr req = f.args.next_ptr();
        if (f.emulator.platform_id() == 3) {
            // 7G: 0x081e3a64 validates the request, 0x081e4114 reads it.
            const auto operation = f.memory.read<u8>(req + 0x10);
            const auto handle = f.memory.read<u32>(req + 0x34);
            if (!operation || !handle) { f.cpu.request_stop(); return; }
            if (*operation != 3 && *operation != 4 && *operation != 5) {
                f.ret(0);
                return;
            }
            if (*handle == 0 || *handle == kFailure) { f.ret(0); return; }
            if (*operation == 5) {
                // 0x081e4158 passes a sign-extended offset and whence to
                // 0x0826d8e0, which rejects invalid origins and positions.
                const auto offset = f.memory.read<u32>(req + 0x18);
                const auto whence = f.memory.read<u8>(req + 0x1c);
                if (!offset || !whence) { f.cpu.request_stop(); return; }
                Vfs& vfs = f.emulator.vfs();
                const i64 size = vfs.size(*handle);
                if (size < 0) { f.cpu.request_stop(); return; }
                u32 status = 0;
                i64 target = static_cast<i32>(*offset);
                if (*whence == 1) target += vfs.tell(*handle);
                else if (*whence == 2) target += size;
                else if (*whence != 0) status = 6;
                if (!status && (target < 0 || target > size)) status = 5;
                if (!status && !vfs.seek(*handle, target)) {
                    f.cpu.request_stop();
                    return;
                }
                if (!store_guest_or_stop(f, req + 0x28, status)) return;
                f.emulator.queue_io_completion(req, "");
                f.ret(1);
                return;
            }
            if (*operation != 3) {
                CLICKHLE_ERROR(kCategory) << "7G async write is not implemented";
                f.cpu.request_stop();
                return;
            }
            const auto buffer = f.memory.read<u32>(req + 0x20);
            const auto length = f.memory.read<u32>(req + 0x24);
            if (!buffer || !length || (*length && !*buffer)) {
                f.cpu.request_stop();
                return;
            }
            Vfs& vfs = f.emulator.vfs();
            const i64 position = vfs.tell(*handle);
            const i64 size = vfs.size(*handle);
            if (position < 0 || size < 0) { f.cpu.request_stop(); return; }
            // 0x0826d5fc reports 5 for a nonempty read beginning at EOF.
            if (*length && position == size) {
                if (!store_guest_or_stop(f, req + 0x2c, 0u) ||
                    !store_guest_or_stop(f, req + 0x28, 5u)) return;
                f.emulator.queue_io_completion(req, "");
                f.ret(1);
                return;
            }
            std::vector<u8> bytes(*length);
            const i64 loaded = vfs.read(*handle, bytes.data(), *length);
            if (loaded < 0 || (loaded && !f.memory.write_bytes(
                    *buffer, bytes.data(), static_cast<u32>(loaded)))) {
                CLICKHLE_ERROR(kCategory) << "7G async read failed";
                f.cpu.request_stop();
                return;
            }
            if (!store_guest_or_stop(f, req + 0x2c, static_cast<u32>(loaded)) ||
                !store_guest_or_stop(f, req + 0x28, 0u)) return;
            f.emulator.queue_io_completion(req, "");
            f.ret(1);
            return;
        }
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        // RetailOS only accepts read, write, and seek request opcodes here. A
        // refused request is not queued and the wrapper only returns 0; it does
        // not write the request's completion status.
        if (*operation != 3 && *operation != 4 && *operation != 5) {
            f.ret(0);
            return;
        }
        const auto handle = f.memory.read<u32>(req + request::kHandle);
        if (!handle.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*handle == 0 || *handle == kFailure) {
            f.ret(0);
            return;
        }
        Vfs& vfs = f.emulator.vfs();

        CLICKHLE_DEBUG(kCategory)
            << "afio_read: handle " << *handle << " op "
            << static_cast<u32>(*operation);
        if (*operation == 5) {  // seek -- confirmed from firmware
            const auto offset = f.memory.read<u32>(req + request::kSeekOffset);
            const auto whence = f.memory.read<u8>(req + request::kSeekWhence);
            const i64 size = vfs.size(*handle);
            const i32 delta = static_cast<i32>(offset.value_or(0));
            const u8 from = whence.value_or(1);  // the game's default is CUR

            // whence 0/1/2 = SET/CUR/END, and the firmware's error codes are
            // specific: 6 for a bad whence, 5 for a target past EOF, and the
            // seek is NOT committed in either case. We had been returning a
            // generic failure and letting the underlying seek do as it liked.
            constexpr u32 kBadWhence = 6;
            constexpr u32 kPastEnd = 5;
            if (from > 2) {
                if (!store_guest_or_stop(f, req + request::kStatus, kBadWhence)) return;
                f.emulator.queue_io_completion(req, "");
                // Accepted, then completed with an error -- so 1, with the code
                // in the status word where the completion handler reads it.
                // r0 is a boolean on every AsyncFileIO queued slot; it never
                // carries an errno.
                f.ret(1);
                return;
            }
            i64 target = delta;
            if (from == 1) {
                target += vfs.tell(*handle);
            } else if (from == 2) {
                target += size;
            }
            if (target < 0 || (size >= 0 && target > size)) {
                CLICKHLE_DEBUG(kCategory)
                    << "seek past end: " << target << " of " << size;
                if (!store_guest_or_stop(f, req + request::kStatus, kPastEnd)) return;
                f.emulator.queue_io_completion(req, "");
                // Accepted, completed with an error -- see the bad-whence arm.
                f.ret(1);
                return;
            }
            const bool ok = vfs.seek(*handle, target);
            CLICKHLE_DEBUG(kCategory)
                << "seek handle " << *handle << " to " << target << " (whence "
                << static_cast<u32>(from) << ", offset " << delta << ")";
            // A seek writes ONLY the status -- not +0x24, not +0x28. The
            // firmware's op-5 handler touches neither.
            //
            // This file previously carried a "clobbering" rationale here, that
            // a seek reporting its position at +0x28 would overwrite the size
            // an open had put in fileobject[0x08]. That story is withdrawn: the
            // open path's completion (`finish_open`) does not touch
            // fileobject[0x08] at all, so there was nothing to clobber. The
            // behaviour below was right; the reason given for it was not.
            //
            // Writing only the status is correct because that is what the
            // firmware does, which is the only justification it needs.
            if (!store_guest_or_stop(f, req + request::kStatus,
                                     ok ? 0u : kPastEnd)) return;
            if (std::getenv("CLICKHLE_NO_SEEK_COMPLETION") == nullptr) {
                f.emulator.queue_io_completion(req, "");
            }
            // The wrapper reports whether it accepted the request, not whether
            // the worker's seek operation succeeded. The guest tests this as a
            // boolean: stream_seek advances stream[0x108] only when accepted.
            //
            // Returning 0 for a successful seek meant Mahjong issued the seek,
            // was told the issue had FAILED, left its state at 0, and re-issued
            // on the next pass -- 592 times. The state machine took the
            // "state == 0" arm 594 times out of 594 and never once reached
            // case 2. Everything downstream was working; the game was simply
            // never told the request had been accepted.
            f.ret(1);
            return;
        }

        // NOTE FROM LLE (2026-09-17, comment only -- no behaviour changed here).
        //
        // "Anything else" folds op 4 in with op 3, and the firmware separates
        // them: op 3 reads and **op 4 writes**. The two opcode handlers call
        // different primitives -- 0x101dae90 -> 0x1021c248, 0x101daf2c ->
        // 0x1021c7a0 -- and the save store uses that same pair for its own read
        // and write forty bytes apart (0x1016c118 / 0x1016c164). They differ in
        // what a short transfer means: EOF on the read side (0x1016c158 sets a
        // flag), failure on the write side (0x1016c19c returns 0). Op 4 also
        // calls 0x1024c900 afterwards to put the resulting position in +0x28,
        // which a read has no use for. Op 7 is therefore open + write + close.
        //
        // Op 4 writes; op 3 reads. The composite operations follow the same
        // pattern: op 6 opens, reads, and closes; op 7 opens, writes, and closes.
        if (*operation == 4) {
            const auto buffer = f.memory.read<u32>(req + request::kBuffer);
            const auto length = f.memory.read<u32>(req + request::kLength);
            if (!buffer.has_value() || !length.has_value()) {
                if (!store_guest_or_stop(f, req + request::kStatus, kFailure)) return;
                f.emulator.queue_io_completion(req, "");
                f.ret(1);
                return;
            }
            std::vector<u8> bytes(*length);
            if (*length != 0 &&
                (*buffer == 0 ||
                 !f.memory.read_bytes(*buffer, bytes.data(), *length))) {
                if (!store_guest_or_stop(f, req + request::kStatus, kFailure)) return;
                f.emulator.queue_io_completion(req, "");
                f.ret(1);
                return;
            }
            const i64 written = vfs.write(*handle, bytes.data(), *length);
            CLICKHLE_DEBUG(kCategory)
                << "write " << written << " of " << *length
                << " bytes on handle " << *handle;
            if (!store_guest_or_stop(f, req + request::kSizeResult,
                                     written < 0 ? 0u : static_cast<u32>(written))) return;
            // Op 4's extra step: it fetches the resulting file position into
            // +0x28 (0x1024c900), which is a write-shaped thing to want.
            if (!store_guest_or_stop(f, req + request::kPositionResult,
                                     static_cast<u32>(vfs.tell(*handle))) ||
                !store_guest_or_stop(f, req + request::kStatus,
                                     written < 0 ? kFailure : 0u)) return;
            f.emulator.queue_io_completion(req, "");
            f.ret(1);
            return;
        }

        // Anything else is a read at the current position.
        const auto buffer = f.memory.read<u32>(req + request::kBuffer);
        const auto length = f.memory.read<u32>(req + request::kLength);
        if (!buffer.has_value() || !length.has_value()) {
            if (!store_guest_or_stop(f, req + request::kStatus, kFailure)) return;
            // The wrapper accepted this valid opcode/handle. Worker-side
            // failures complete the request; they do not change that result.
            if (std::getenv("CLICKHLE_NO_READ_COMPLETION") == nullptr) {
                f.emulator.queue_io_completion(req, "");
            }
            f.ret(1);
            return;
        }
        // Clamp to what the file can actually supply BEFORE allocating.
        //
        // This was `std::vector<u8> bytes(*length)` on the guest's word,
        // unexamined. It was harmless while every read was a 4 KB retry, but
        // once reads began advancing, Mahjong asked for a length that ran past
        // the end of a 7 MB file and the emulator sat allocating and zeroing it
        // -- a hang in the host, not the guest, which is why SIGTERM could not
        // shift it and the CPU's stop flag was never even consulted.
        //
        // A short read is the correct answer to a request past EOF: the byte
        // count goes to +0x24, the resource library advances by it, and the
        // caller asks for the rest. It is not an error and must not be one.
        const i64 remaining = vfs.size(*handle) - vfs.tell(*handle);
        const u32 wanted =
            remaining <= 0 ? 0u
                           : std::min<u32>(*length, static_cast<u32>(remaining));
        if (wanted != *length) {
            CLICKHLE_DEBUG(kCategory)
                << "afio_read: clamping " << *length << " to " << wanted
                << " (" << remaining << " left on handle " << *handle << ")";
        }
        std::vector<u8> bytes(wanted);
        const i64 got =
            wanted == 0 ? 0 : vfs.read(*handle, bytes.data(), wanted);
        if (got < 0) {
            CLICKHLE_WARN(kCategory)
                << "afio_read: vfs read failed on handle " << *handle;
            if (!store_guest_or_stop(f, req + request::kStatus, kFailure)) return;
            if (std::getenv("CLICKHLE_NO_READ_COMPLETION") == nullptr) {
                f.emulator.queue_io_completion(req, "");
            }
            f.ret(1);
            return;
        }
        // Bulk copy, not a byte at a time. This loop was `f.store<u8>` per
        // byte, which was invisible while every read was a 4 KB retry of the
        // same block. The moment reads started advancing, Mahjong began pulling
        // real assets -- 258,533 bytes in a single call -- and a per-byte store
        // with its own bounds check turned a 1200-frame run into something that
        // had not finished in six minutes.
        if (!f.memory.write_bytes(*buffer, bytes.data(),
                                  static_cast<u32>(got))) {
            CLICKHLE_WARN(kCategory)
                << "afio_read: refusing " << got << " bytes to " << std::hex
                << *buffer << std::dec << " -- outside guest memory";
            if (!store_guest_or_stop(f, req + request::kStatus, kFailure)) return;
            if (std::getenv("CLICKHLE_NO_READ_COMPLETION") == nullptr) {
                f.emulator.queue_io_completion(req, "");
            }
            f.ret(1);
            return;
        }
        if (!store_guest_or_stop(f, req + request::kSizeResult,
                                 static_cast<u32>(got)) ||
            !store_guest_or_stop(f, req + request::kStatus, 0u)) return;
        CLICKHLE_DEBUG(kCategory) << "read " << got << " bytes";
        // A read completes through the app+0x2c list, like an open. Mahjong
        // only reaches this call at all once its streaming open completes, and
        // it then waits on the read the same way.
        //
        // Note this is the app+0x2c path, NOT the request+0x34 trampoline --
        // completing a read through the trampoline is what broke Tetris
        // (section 40). The guest's own processor calls the callback; we only
        // publish.
        if (std::getenv("CLICKHLE_NO_READ_COMPLETION") == nullptr) {
            f.emulator.queue_io_completion(req, "");
        }
        // The rest of the old note still applies to the trampoline path.
        //
        // The opens are completed on positive evidence: PAC-MAN sat on its
        // loading screen polling nothing until its open trampoline ran, and
        // completing them moved six titles. There is no such evidence for a
        // read, and there is direct evidence against: Tetris issues exactly one
        // #2, and completing it branches to 0x1801d258 -- a real function in
        // the image, so no pointer check catches it -- which never returns.
        //
        // The likely reading is that #2 is synchronous: it does the work and
        // returns, the way its single argument and its use as "advance this
        // request" suggest. Completing it dispatches a callback the game has
        // already run itself.
        //
        // 1, not 0 -- and this is the SAME contract the seek arm above already
        // obeys. #2 is one slot with one return convention; the seek arm was
        // fixed and the read arm was left behind, so Mahjong was told every
        // seek was accepted and every read was refused. It responded exactly as
        // the state machine says it must: never advanced its position, and
        // re-seeked to 0 before each retry. The log is 3996 reads in 8000
        // frames, every one of them preceded by `seek handle 2 to 0`, on a file
        // that needs 1781 sequential chunks.
        f.ret(1);
    });

    registry.add(kModule, 1, "afio_close", 1, [](HleCallFrame& f) {
        const GuestAddr req = f.args.next_ptr();
        if (f.emulator.platform_id() == 3) {
            // 0x081e3714 validates the request; 0x081e41b0 closes it.
            const auto operation = f.memory.read<u8>(req + 0x10);
            const auto handle = f.memory.read<u32>(req + 0x34);
            if (!operation || !handle) { f.cpu.request_stop(); return; }
            if (*operation != 2 || *handle == 0 || *handle == kFailure ||
                !f.emulator.vfs().close(*handle)) {
                f.ret(0);
                return;
            }
            if (!store_guest_or_stop(f, req + 0x28, 0u)) return;
            f.emulator.queue_io_completion(req, "");
            f.ret(1);
            return;
        }
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*operation != 2) {
            f.ret(0);
            return;
        }
        const auto handle = f.memory.read<u32>(req + request::kHandle);
        if (!handle.has_value()) {
            f.cpu.request_stop();
            return;
        }
        CLICKHLE_DEBUG(kCategory)
            << "afio_close: handle " << handle.value_or(0);
        // The firmware wrapper accepts only opcode 2 and returns 0 without
        // touching the request if the opcode or stream handle is invalid.
        if (*handle == 0 || *handle == kFailure ||
            !f.emulator.vfs().close(*handle)) {
            f.ret(0);
            return;
        }
        if (!store_guest_or_stop(f, req + request::kStatus, 0u)) return;
        // A close DOES need a completion, and suppressing it leaked every slot.
        //
        // The old note here said a close is not something a title waits on.
        // That was drawn from a real crash but generalised too far: what broke
        // Tetris was completing through the *request+0x34 trampoline* on a
        // recycled request, whose +0x34 pointed into the middle of a function.
        // The trampoline is still the wrong mechanism. The app+0x2c list is not.
        //
        // Mahjong made the cost visible. The resource library has exactly ten
        // 0x184-byte slots on a free list, and a slot returns to that list only
        // in `0x18016f8c`, which runs off the close completion. With the close
        // never completing, all ten leaked: ten opens of main.rlb, ten closes,
        // and the eleventh allocation returned -1. The title then sat on its
        // "slot index negative" assert at 0x18016b3c -- a `b .` self-loop that
        // took 96.5% of all profile samples.
        //
        // Ten opens and ten closes with no progress is what a leak looks like
        // from the outside; the counts matching is exactly why it read as
        // healthy.
        if (std::getenv("CLICKHLE_NO_CLOSE_COMPLETION") == nullptr) {
            f.emulator.queue_io_completion(req, "");
        }
        //
        // 1, not 0 -- the same boolean contract as #2 (see afio_read).
        //
        // Mahjong's close wrapper at 0x1801dbc0 sets fileobject[0x04] to 3 only
        // if this returns non-zero. Reporting a successful close as failed left
        // the object stuck in state 2, and the `state == 0` gate at 0x1801dc7c
        // then refused every later open on it -- silently, with no framework
        // call to show for it. One wrong return value poisons that file object
        // for the rest of the run.
        f.ret(1);
    });

    // #4(mode, path, request) accepts opcode 7: open, write, close.
    registry.add(kModule, 4, "afio_operation", 3, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32() & 0xFFu;
        const GuestAddr path_address = f.args.next_ptr();
        const GuestAddr req = f.args.next_ptr();
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (mode == 0 || path_address == 0 || *operation != 7) {
            f.ret(0);
            return;
        }
        const auto path_value = read_path_strict(f, path_address);
        if (!path_value.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const std::string& path = *path_value;
        Vfs& vfs = f.emulator.vfs();
        const auto name_space = file_namespace(mode);
        if (!name_space || *name_space == Vfs::Namespace::kGame) {
            f.ret(0);
            return;
        }
        const u32 handle = vfs.open(path, open_mode(*name_space), true, *name_space);
        if (handle == 0) {
            if (!store_guest_or_stop(f, req + request::kHandle, kFailure) ||
                !store_guest_or_stop(f, req + request::kSizeResult, 0u) ||
                !store_guest_or_stop(f, req + request::kPositionResult, 0u) ||
                !store_guest_or_stop(f, req + request::kStatus, kFailure)) return;
            f.emulator.queue_io_completion(req, path);
            f.ret(1);
            return;
        }

        if (!store_guest_or_stop(f, req + request::kHandle, handle)) return;
        const auto buffer = f.memory.read<u32>(req + request::kBuffer);
        const auto length = f.memory.read<u32>(req + request::kLength);
        i64 written = -1;
        if (buffer.has_value() && length.has_value() &&
            (*length == 0 || *buffer != 0)) {
            std::vector<u8> bytes(*length);
            if (*length == 0 ||
                f.memory.read_bytes(*buffer, bytes.data(), *length)) {
                written = vfs.write(handle, bytes.data(), *length);
            }
        }
        if (!store_guest_or_stop(f, req + request::kSizeResult,
                                 written < 0 ? 0u : static_cast<u32>(written)) ||
            !store_guest_or_stop(f, req + request::kPositionResult,
                                 static_cast<u32>(vfs.tell(handle))) ||
            !store_guest_or_stop(f, req + request::kStatus,
                                 written < 0 ? kFailure : 0u)) return;
        (void)vfs.close(handle);
        f.emulator.queue_io_completion(req, path);
        // 1 means the composite request was accepted. Its status and byte
        // count are delivered through the request completion.
        f.ret(1);
    });

    // #5 queues opcode 8: remove a path. The RetailOS wrapper checks all
    // three inputs before enqueueing; the worker writes its result to +0x20.
    // The host VFS confines removal to saves so dumped game assets stay intact.
    registry.add(kModule, 5, "afio_remove", 3, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32() & 0xffu;
        const GuestAddr path_address = f.args.next_ptr();
        const GuestAddr req = f.args.next_ptr();
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (mode == 0 || path_address == 0 || *operation != 8) {
            f.ret(0);
            return;
        }
        const auto path = read_path_strict(f, path_address);
        if (!path.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const auto name_space = file_namespace(mode);
        const bool removed = name_space &&
            f.emulator.vfs().remove_saved(*path, *name_space);
        // The firmware's filesystem method returns a status word. The exact
        // missing-path code is not established; success is zero.
        if (!store_guest_or_stop(f, req + request::kStatus,
                                 removed ? 0u : kFailure)) return;
        CLICKHLE_INFO(kCategory) << "remove(\"" << *path << "\") "
                                 << (removed ? "ok" : "failed");
        f.emulator.queue_io_completion(req, *path);
        f.ret(1);
    });

    // #0 afio_open(mode, path, flag, request) -- streaming open.
    registry.add(kModule, 0, "afio_open", 4, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32() & 0xffu;
        const GuestAddr path_address = f.args.next_ptr();
        const u32 flag = f.args.next_u32() & 0xffu;
        const GuestAddr req = f.args.next_ptr();
        if (f.emulator.platform_id() == 3) {
            // 7G worker 0x081e4078 uses a different request layout from 5G.
            const auto operation = f.memory.read<u8>(req + 0x10);
            if (!operation) { f.cpu.request_stop(); return; }
            if (*operation != 1 || path_address == 0 ||
                ((mode == 0 || mode == 4) && flag == 0)) {
                f.ret(0);
                return;
            }
            const auto path = read_path_strict(f, path_address);
            const auto name_space = file_namespace(mode);
            if (!path || !name_space) { f.cpu.request_stop(); return; }
            Vfs& vfs = f.emulator.vfs();
            const u32 handle = vfs.open(*path, open_mode(*name_space), true,
                                        *name_space);
            if (!handle) {
                // The 7G filesystem's error mapping is not yet established.
                CLICKHLE_ERROR(kCategory) << "7G open failed: " << *path;
                f.emulator.stop_with_error("7G file-open error is not yet modeled: " + *path);
                return;
            }
            const i64 size = vfs.size(handle);
            if (size < 0 || static_cast<u64>(size) > 0xffffffffu) {
                f.cpu.request_stop();
                return;
            }
            const auto object = f.memory.read<u32>(req + 0x14);
            if (!object || !store_guest_or_stop(f, req + 0x34, handle) ||
                !store_guest_or_stop(f, req + 0x28, 0u) ||
                !store_guest_or_stop(f, req + 0x30, static_cast<u32>(size)) ||
                (*object && !store_guest_or_stop(f, *object + 8,
                                                  static_cast<u32>(size)))) return;
            CLICKHLE_INFO(kCategory) << "7G open(\"" << *path << "\") -> "
                                     << size << " bytes";
            f.emulator.queue_io_completion(req, *path);
            f.ret(1);
            return;
        }
        dump_request(f, req);
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*operation != 1 || path_address == 0 ||
            (mode == 0 && flag == 0)) {
            CLICKHLE_DEBUG(kCategory) << "open4 rejected: mode=" << mode
                                     << ", flag=" << flag
                                     << ", operation=" << unsigned(*operation);
            f.ret(0);
            return;
        }
        const auto path_value = read_path_strict(f, path_address);
        if (!path_value.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const std::string& path = *path_value;
        Vfs& vfs = f.emulator.vfs();
        const auto name_space = file_namespace(mode);
        if (!name_space) { f.ret(0); return; }
        const u32 handle = vfs.open(path, open_mode(*name_space), true, *name_space);
        if (handle == 0) {
            if (!store_guest_or_stop(f, req + request::kHandle, kFailure) ||
                !store_guest_or_stop(f, req + request::kStatus, 0x17u)) return;
            CLICKHLE_INFO(kCategory) << "open4(\"" << path << "\") FAILED";
            if (std::getenv("CLICKHLE_NO_OPEN_COMPLETION") == nullptr) {
                f.emulator.queue_io_completion(req, path);
            }
            f.ret(1);
            return;
        }

        const i64 size = vfs.size(handle);
        if (!store_guest_or_stop(f, req + request::kHandle, handle)) return;
        i64 loaded = 0;
        const auto buffer = f.memory.read<u32>(req + request::kBuffer);
        const auto length = f.memory.read<u32>(req + request::kLength);
        if (buffer.value_or(0) != 0 && length.value_or(0) != 0) {
            const i64 remaining = size - vfs.tell(handle);
            const u32 wanted = remaining <= 0
                                   ? 0u
                                   : std::min<u32>(*length,
                                                  static_cast<u32>(remaining));
            std::vector<u8> bytes(wanted);
            loaded = wanted == 0 ? 0 : vfs.read(handle, bytes.data(), wanted);
            if (loaded > 0 &&
                !f.memory.write_bytes(*buffer, bytes.data(),
                                     static_cast<u32>(loaded))) {
                loaded = -1;
            }
        }
        const u32 result = loaded > 0
                               ? static_cast<u32>(loaded)
                               : (size < 0 ? 0u : static_cast<u32>(size));
        if (!store_guest_or_stop(f, req + request::kSizeResult, result) ||
            !store_guest_or_stop(f, req + request::kStatus,
                                 loaded < 0 ? kFailure : 0u)) return;

        // RetailOS leaves request+0x28 alone on open. Its result is published
        // through fileobject+0x08, which the game's stream reader uses to size
        // the next read. Omitting that field leaves the stream waiting forever.
        const auto object = f.memory.read<u32>(req + request::kFileObject);
        if (object.has_value() && *object != 0 &&
            !store_guest_or_stop(f, *object + kFileObjectResult, result)) return;

        CLICKHLE_INFO(kCategory)
            << "open4(\"" << path << "\") -> handle " << handle
            << ", " << size << " bytes, loaded " << loaded;
        if (std::getenv("CLICKHLE_NO_OPEN_COMPLETION") == nullptr) {
            f.emulator.queue_io_completion(req, path);
        }
        f.ret(1);
    });

    // #7 opens a directory and queues its names for #9. RetailOS caps the
    // result list at 500 entries and writes its count at request+0x28.
    // iQuiz currently supplies mode 0 and flags 0 for UserTrivia/Packs.
    registry.add(kModule, 7, "afio_open_directory", 4, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32() & 0xFFu;
        const GuestAddr path_address = f.args.next_ptr();
        const u32 flags = f.args.next_u32();
        const GuestAddr req = f.args.next_ptr();
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!operation.has_value()) {
            f.cpu.request_stop();
            return;
        }
        if (*operation != 10 || path_address == kNullGuest) {
            f.ret(0);
            return;
        }
        const auto path_value = read_path_strict(f, path_address);
        if (!path_value.has_value()) {
            f.cpu.request_stop();
            return;
        }
        const std::string& path = *path_value;

        Vfs& vfs = f.emulator.vfs();
        const auto name_space = file_namespace(mode);
        if (!name_space) { f.ret(0); return; }
        const std::filesystem::path& root = vfs.root_for(*name_space);
        const std::filesystem::path host_path = vfs.resolve_under(root, path);
        std::error_code error;
        if (host_path.empty() ||
            !std::filesystem::is_directory(host_path, error) || error) {
            f.ret(0);
            return;
        }

        constexpr u32 kMaximumEntries = 500;
        Emulator::FilesystemState::DirectoryRequest directory;
        directory.path = path;
        directory.mode = static_cast<u8>(mode);
        directory.flags = static_cast<u8>(flags);
        for (std::filesystem::directory_iterator it(host_path, error), end;
             !error && it != end; it.increment(error)) {
            if (directory.entries.size() == kMaximumEntries) {
                break;
            }
            std::error_code type_error;
            const bool is_directory = it->is_directory(type_error);
            if (type_error) {
                error = type_error;
                break;
            }
            directory.entries.push_back(
                {it->path().filename().string(),
                 static_cast<u8>(is_directory ? 1 : 0)});
        }
        if (error) {
            f.ret(0);
            return;
        }

        const u32 status = directory.entries.size() == kMaximumEntries
                                ? 0x12u
                                : 0u;
        auto& directories = f.emulator.filesystem_state().directories;
        const u32 handle = static_cast<u32>(directories.size());
        if (!store_guest_or_stop(f, req + request::kStatus, status) ||
            !store_guest_or_stop(f, req + request::kDirectoryCount,
                                 static_cast<u32>(directory.entries.size()))) {
            return;
        }
        directories.push_back(std::move(directory));
        if (!f.store<u32>(req + request::kHandle, handle)) {
            f.cpu.request_stop();
            return;
        }
        f.emulator.queue_io_completion(req, path);
        if (std::getenv("CLICKHLE_DUMP_OPENDIR") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "open_directory(\"" << path << "\") mode=" << mode
                << " flags=0x" << std::hex << flags << " request=0x" << req
                << " handle=" << std::dec << handle;
        }
        CLICKHLE_DEBUG(kCategory)
            << "open_directory(\"" << path << "\") -> handle " << handle
            << ", " << directories.back().entries.size() << " entries";
        f.ret(1);
    });

    // #9 returns one NUL-terminated directory name per poll. Its status byte
    // identifies the entry type; RetailOS returns 0 while more entries remain,
    // 1 on the final/empty result, 4 for a missing queue slot, and 0x17 when
    // the caller's buffer is too small.
    registry.add(kModule, 9, "afio_poll_directory", 4, [](HleCallFrame& f) {
        const u32 slot = f.args.next_u32();
        const GuestAddr buffer = f.args.next_ptr();
        const u32 capacity = f.args.next_u32();
        const GuestAddr type_out = f.args.next_ptr();

        auto& directories = f.emulator.filesystem_state().directories;
        if (slot >= directories.size()) {
            f.ret(4);
            return;
        }

        auto& directory = directories[slot];
        if (directory.completed ||
            directory.next_entry >= directory.entries.size()) {
            directory.completed = true;
            CLICKHLE_DEBUG(kCategory)
                << "poll_directory slot " << slot << " -> complete";
            f.ret(1);
            return;
        }

        // RetailOS marks a queue record complete before checking the caller's
        // capacity, so a short-buffer result is followed by a completed poll.
        directory.completed = true;
        const auto& entry = directory.entries[directory.next_entry];
        const u32 required = static_cast<u32>(entry.name.size() + 1);
        if (capacity < required) {
            CLICKHLE_DEBUG(kCategory)
                << "poll_directory slot " << slot << " buffer too small: "
                << capacity << " < " << required;
            f.ret(0x17);
            return;
        }

        std::vector<u8> name(required);
        std::copy(entry.name.begin(), entry.name.end(), name.begin());
        name.back() = 0;
        if (buffer == kNullGuest || type_out == kNullGuest ||
            !f.memory.write_bytes(buffer, name.data(), required) ||
            !store_guest_or_stop(f, type_out, entry.type)) {
            f.cpu.request_stop();
            return;
        }
        ++directory.next_entry;
        directory.completed =
            directory.next_entry >= directory.entries.size();
        CLICKHLE_DEBUG(kCategory)
            << "poll_directory slot " << slot << " -> \"" << entry.name
            << "\" type " << static_cast<u32>(entry.type)
            << (directory.completed ? " (last)" : "");
        f.ret(directory.completed ? 1u : 0u);
    });

    // The save store uses four indexed entries. #12 opens a name and writes
    // the selected slot index through its third argument; #13 retains a slot,
    // #14 releases it, #15 reads at an absolute or current-relative offset,
    // and #16 replaces its stored bytes. The VFS backs persistent bytes,
    // while indices, positions, and reference counts are per app.
    registry.add(kModule, 12, "afio_store_open", 3, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32() & 0xFFu;
        const GuestAddr path_address = f.args.next_ptr();
        const GuestAddr out = f.args.next_ptr();
        if (mode == 0) {
            f.ret(kStoreInvalidMode);
            return;
        }
        const auto path_value = read_path_strict(f, path_address);
        if (!path_value) {
            f.cpu.request_stop();
            return;
        }
        const std::string& path = *path_value;

        auto& stores = f.emulator.filesystem_state().stores;
        for (u32 i = 0; i < stores.size(); ++i) {
            if (stores[i].references != 0 && stores[i].path == path) {
                ++stores[i].references;
                if (!f.store<u32>(out, i)) {
                    f.cpu.request_stop();
                    return;
                }
                f.ret(0);
                return;
            }
        }
        for (u32 i = 0; i < stores.size(); ++i) {
            if (stores[i].references == 0) {
                std::vector<u8> data;
                const u32 file = f.emulator.vfs().open(
                    path, Vfs::Mode::kRead, false, Vfs::Namespace::kData);
                if (file != 0) {
                    const i64 file_size = f.emulator.vfs().size(file);
                    if (file_size < 0 || file_size > 0x19000u) {
                        (void)f.emulator.vfs().close(file);
                        f.ret(0x17);
                        return;
                    }
                    data.resize(static_cast<std::size_t>(file_size));
                    if (file_size != 0 &&
                        f.emulator.vfs().read(file, data.data(),
                                              static_cast<u32>(file_size)) !=
                            file_size) {
                        (void)f.emulator.vfs().close(file);
                        f.ret(0x17);
                        return;
                    }
                    (void)f.emulator.vfs().close(file);
                }
                const u64 total = static_cast<u64>(f.emulator.filesystem_state().store_bytes) +
                                  data.size();
                if (total > 0x19000u) {
                    f.ret(0x17);
                    return;
                }
                stores[i].path = path;
                stores[i].references = 1;
                stores[i].size = static_cast<u32>(data.size());
                stores[i].position = 0;
                stores[i].data = std::move(data);
                f.emulator.filesystem_state().store_bytes =
                    static_cast<u32>(total);
                if (!f.store<u32>(out, i)) {
                    f.cpu.request_stop();
                    return;
                }
                CLICKHLE_INFO(kCategory) << "store_open(\"" << path
                                         << "\") -> slot " << i;
                f.ret(0);
                return;
            }
        }
        f.ret(0x17);
    });

    registry.add(kModule, 13, "afio_store_retain", 1, [](HleCallFrame& f) {
        const u32 slot = f.args.next_u32();
        auto& stores = f.emulator.filesystem_state().stores;
        if (slot >= stores.size() || stores[slot].references == 0) {
            f.ret(kStoreGenericFailure);
            return;
        }
        ++stores[slot].references;
        f.ret(0);
    });

    registry.add(kModule, 14, "afio_store_close", 1, [](HleCallFrame& f) {
        const u32 slot = f.args.next_u32();
        auto& state = f.emulator.filesystem_state();
        if (slot >= state.stores.size() ||
            state.stores[slot].references == 0) {
            f.ret(kStoreGenericFailure);
            return;
        }
        auto& store = state.stores[slot];
        --store.references;
        if (store.references == 0) {
            state.store_bytes -= store.size;
            store = {};
        }
        f.ret(0);
    });

    // #15(slot, destination, in/out byte_count, origin byte, offset) reads
    // from the store object. origin 0 uses an absolute offset; origin 1 adds
    // the supplied offset to the object's current position. RetailOS returns
    // 8 for an invalid/uninitialized slot, 6 for an invalid origin or
    // negative effective offset, 5 when the offset is at/past EOF, and 0 on
    // success. At EOF the count pointer is not touched.
    registry.add(kModule, 15, "afio_store_read", 5, [](HleCallFrame& f) {
        const u32 slot = f.args.next_u32();
        const GuestAddr destination = f.args.next_ptr();
        const GuestAddr count_address = f.args.next_ptr();
        const u32 origin = f.args.next_u32() & 0xFFu;
        const u32 requested_offset = f.args.next_u32();
        auto& stores = f.emulator.filesystem_state().stores;
        if (slot >= stores.size() || stores[slot].references == 0) {
            f.ret(kStoreGenericFailure);
            return;
        }
        auto& store = stores[slot];
        if (origin > 1) {
            f.ret(6);
            return;
        }
        const i64 offset = static_cast<i64>(requested_offset) +
                           (origin == 1 ? store.position : 0);
        if (offset < 0 || offset > std::numeric_limits<i32>::max()) {
            f.ret(6);
            return;
        }
        if (static_cast<u64>(offset) >= store.size) {
            f.ret(5);
            return;
        }
        const auto count = f.memory.read<u32>(count_address);
        if (!count) {
            f.cpu.request_stop();
            return;
        }
        const u32 available = store.size - static_cast<u32>(offset);
        const u32 length = std::min(*count, available);
        if (length != *count && !f.store<u32>(count_address, length)) {
            f.cpu.request_stop();
            return;
        }
        if (length != 0 &&
            (store.data.size() < store.size ||
             !f.memory.write_bytes(destination, store.data.data() + offset,
                                   length))) {
            f.cpu.request_stop();
            return;
        }
        store.position = static_cast<u32>(offset) + length;
        f.ret(0);
    });

    registry.add(kModule, 16, "afio_store_write", 3, [](HleCallFrame& f) {
        const u32 slot = f.args.next_u32();
        const GuestAddr buffer = f.args.next_ptr();
        const u32 length = f.args.next_u32();
        auto& state = f.emulator.filesystem_state();
        if (slot >= state.stores.size() ||
            state.stores[slot].references == 0) {
            f.ret(kStoreGenericFailure);
            return;
        }
        const u64 total = static_cast<u64>(state.store_bytes) -
                          state.stores[slot].size + length;
        if (total > 0x19000u || (length != 0 && buffer == kNullGuest)) {
            f.ret(0x17);
            return;
        }
        std::vector<u8> bytes(length);
        if (length != 0 && !f.memory.read_bytes(buffer, bytes.data(), length)) {
            f.ret(0x17);
            return;
        }
        const std::string path = state.stores[slot].path;
        const u32 host_handle = f.emulator.vfs().open(
            path, Vfs::Mode::kWrite, true, Vfs::Namespace::kData);
        if (host_handle == 0) {
            f.ret(0x17);
            return;
        }
        const i64 written = f.emulator.vfs().write(host_handle, bytes.data(), length);
        (void)f.emulator.vfs().close(host_handle);
        if (written < 0 || static_cast<u32>(written) != length) {
            f.ret(0x17);
            return;
        }
        state.store_bytes = static_cast<u32>(total);
        state.stores[slot].size = length;
        state.stores[slot].data = std::move(bytes);
        state.stores[slot].position = 0;
        CLICKHLE_DEBUG(kCategory) << "store_write slot " << slot << ": "
                                  << length << " bytes";
        f.ret(0);
    });

    // --- "Filesytem" (Apple's spelling) -------------------------------------
    //
    // A second, synchronous file API alongside AsyncFileIO. Four ordinals.
    //
    // #0 probed in iQuiz:
    //     Filesytem#0(0, "Sounds/All_Out.wav", 1, out)
    //
    // Note the subdirectory -- this one takes real paths, where AsyncFileIO's
    // openers take bare names. The VFS already resolves either, and
    // case-insensitively, which matters because the games were built against
    // FAT.
    // The firmware keeps a table of 10 handle slots and every ordinal loops
    // `while (i < 10)` (LLE track). Ten is a real ceiling, not an
    // implementation detail: an eleventh open has nowhere to go, and a title
    // that hits it on hardware would get a failure we would otherwise not give.
    static constexpr u32 kFsSlots = 10;
    // #0 returns 0x17 when open fails. A nonzero handle absent from the
    // ten-entry table returns 0x20 in both #2 and #3. An oracle probe using
    // handle 0 on a cold table instead matched the first empty slot and
    // reached the inner file method: it returned 0x02 for read and 0x13 for
    // write. Keep those inner-method errors distinct from the wrapper's table
    // miss; zero is also the table's empty-slot value.
    static constexpr u32 kFsOpenFailed = 0x17;
    static constexpr u32 kFsTableMiss = 0x20;
    static constexpr u32 kFsReadNullObject = 0x02;
    static constexpr u32 kFsWriteDenied = 0x13;

    registry.add(kFilesystemModule, 0, "fs_open", 4, [](HleCallFrame& f) {
        const u32 namespace_id = f.args.next_u32() & 0xFFu;
        const GuestAddr path_address = f.args.next_ptr();
        const u32 access_flag = f.args.next_u32() & 0xFFu;
        const GuestAddr out = f.args.next_ptr();

        // RetailOS initializes *out before it even scans the handle table or
        // dereferences the path. Do not turn an invalid output pointer into a
        // recoverable open error, and do not open a host file after that fault.
        if (!f.store<u32>(out, 0)) {
            f.cpu.request_stop();
            return;
        }

        auto& fs_table = f.emulator.filesystem_state().handles;
        std::size_t slot = 0;
        while (slot < kFsSlots && fs_table[slot] != 0) {
            ++slot;
        }
        if (slot == kFsSlots) {
            CLICKHLE_WARN(kCategory)
                << "fs_open: all " << kFsSlots
                << " handle slots are in use, as on hardware";
            f.ret(kFsOpenFailed);
            return;
        }

        // The firmware's path helper reads until NUL. Bound that walk for a
        // corrupt guest pointer, but preserve its fault behavior instead of
        // treating an unmapped or unterminated path as an empty filename.
        std::string path;
        bool terminated = false;
        for (u32 i = 0; i < kMaxPath; ++i) {
            const auto c = f.memory.read<u8>(path_address + i);
            if (!c.has_value()) {
                f.cpu.request_stop();
                return;
            }
            if (*c == 0) {
                terminated = true;
                break;
            }
            path.push_back(static_cast<char>(*c));
        }
        if (!terminated) {
            f.cpu.request_stop();
            return;
        }
        // The first argument selects the firmware path namespace. The third
        // argument is a separate file-object access flag; the inner writer
        // returns 0x13 whenever it is nonzero.
        const auto name_space = file_namespace(namespace_id);
        if (!name_space) { f.ret(kFsOpenFailed); return; }
        const auto vfs_mode = access_flag == 0
            ? open_mode(*name_space) : Vfs::Mode::kRead;
        const bool allow_write = *name_space != Vfs::Namespace::kGame &&
                                 access_flag == 0;
        const u32 handle = f.emulator.vfs().open(
            path, vfs_mode, allow_write, *name_space);
        if (handle == 0) {
            CLICKHLE_INFO(kCategory)
                << "fs_open(\"" << path << "\") -> MISSING";
            f.ret(kFsOpenFailed);
            return;
        }
        // The successful object is installed in the global table before the
        // wrapper publishes its handle through argument 4.
        fs_table[slot] = handle;
        if (!f.store<u32>(out, handle)) {
            f.cpu.request_stop();
            return;
        }
        CLICKHLE_INFO(kCategory) << "fs_open[" << namespace_id << "](\""
                                 << path << "\", access=" << access_flag
                                 << ") -> ok";
        f.ret(0);
    });

    // #2(handle, buffer, length, out) -- the NATURAL order. Measured.
    //
    // This previously read (handle, length, buffer, out) on the strength of a
    // decompile note that the firmware forwards (p1, p3, p2, p4) internally,
    // carrying the caveat "UNVERIFIED AGAINST A TITLE: nothing in the corpus
    // calls #2 or #3". That caveat was wrong on both counts: **Bejeweled
    // calls #2 constantly**, and the swapped reading is what broke it.
    //
    // Measured live (2026-09-12, CLICKHLE_DUMP_FSREAD), every call identical:
    //
    //     fs_read raw: handle=58 arg2=0x300ffe50 arg3=0x1 out=0x300ffe40
    //
    // arg2 is a stack ADDRESS (the destination buffer) and arg3 is 1 -- a
    // one-byte read, which is what a character-at-a-time text parser does.
    // Read the swapped way, `length` became 0x300ffe50 (~805 MB), the VFS
    // returned 0 bytes, nothing was ever written to the buffer, and the
    // out-parameter reported 0. The whole failure chain followed from that:
    // Bejeweled's parser scans for a tab (9) or newline (10) with
    // `add r4,r4,#1 / ldrb r1,[r4] / cmp r1,#9 / cmpne r1,#0xa / bne` at
    // 0x1801120c, never finds one in a buffer that was never filled, walks
    // up off the end of the stack and dies on an unmapped read at
    // 0x30100000 -- which is also, finally, the real explanation for the
    // "hang" reported as pc==lr==0x180208a4 (an innocent return address the
    // budget guard happened to be parked on, exactly as oracle suspected).
    //
    // If the firmware really does transpose these internally, then it does so
    // AFTER the ordinal boundary, and the ordinal's own signature is the
    // natural one -- which is what a caller can actually see.
    //
    // #3's writer also receives the same public `(handle, buffer, length,
    // out)` ordering: its export performs the same internal argument swap,
    // and the target's decompilation identifies arg3 as the byte count and
    // arg2 as the source buffer. It has no runtime caller in the current
    // corpus, so this part is firmware-structure evidence rather than a title
    // observation.
    registry.add(kFilesystemModule, 2, "fs_read", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const u32 arg2 = f.args.next_u32();
        const u32 arg3 = f.args.next_u32();
        const GuestAddr out = f.args.next_ptr();
        if (std::getenv("CLICKHLE_DUMP_FSREAD") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "fs_read raw: handle=" << handle << " arg2=0x" << std::hex
                << arg2 << " arg3=0x" << arg3 << " out=0x" << out << std::dec;
        }
        const GuestAddr buffer = arg2;
        const u32 length = arg3;
        const auto& fs_table = f.emulator.filesystem_state().handles;
        if (std::find(fs_table.begin(), fs_table.end(), handle) ==
            fs_table.end()) {
            f.ret(kFsTableMiss);
            return;
        }
        // The inner RetailOS method clears the byte-count output before it
        // checks the file object or performs the read. It dereferences this
        // pointer unconditionally; a null/unmapped output is a guest memory
        // fault, not an omitted optional result.
        if (!store_guest_or_stop(f, out, 0u)) return;
        std::vector<u8> bytes(length);
        const i64 got = f.emulator.vfs().read(handle, bytes.data(), length);
        for (i64 i = 0; i < got; ++i) {
            if (!store_guest_or_stop(
                    f, buffer + static_cast<u32>(i),
                    bytes[static_cast<std::size_t>(i)])) return;
        }
        if (!store_guest_or_stop(f, out,
                                 got < 0 ? 0u : static_cast<u32>(got))) return;
        CLICKHLE_DEBUG(kCategory)
            << "fs_read(" << handle << ", " << length << ") -> " << got;
        f.ret(got < 0 ? kFsReadNullObject : 0u);
    });

    registry.add(kFilesystemModule, 1, "fs_close", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        // Free the table slot too, or the ten-handle ceiling becomes a
        // ten-open-ever limit and a title that opens and closes in a loop
        // starts failing partway through.
        auto& fs_table = f.emulator.filesystem_state().handles;
        const auto slot = std::find(fs_table.begin(), fs_table.end(), handle);
        if (slot != fs_table.end()) {
            *slot = 0;
            (void)f.emulator.vfs().close(handle);
        }
        // VOID. It does not write r0 at all.
        //
        // Measured by the oracle against both RetailOS images, four inputs:
        // 0xDEADBEEF in -> 0xDEADBEEF out, 0x12345678 -> 0x12345678,
        // 0xFFFFFFFF -> 0xFFFFFFFF, bit-identical every time.
        //
        // The static reading said "const 0", and their explanation of how that
        // arose is worth keeping: it came from passing r0 = 0, where a void
        // function returns 0 and is indistinguishable from one that returns a
        // constant. A probe that only ever passes zero cannot tell those apart.
        f.ret_void();
    });

    // #3(handle, buffer, length, out) writes bytes. The export forwards
    // (handle, arg3, arg2, arg4) to the internal writer; its second argument
    // is the byte count, third is the source buffer, and fourth receives the
    // number written. This is the write-side twin of #2's read call.
    registry.add(kFilesystemModule, 3, "fs_write", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const GuestAddr buffer = f.args.next_ptr();
        const u32 length = f.args.next_u32();
        const GuestAddr out = f.args.next_ptr();
        const auto& fs_table = f.emulator.filesystem_state().handles;
        if (std::find(fs_table.begin(), fs_table.end(), handle) ==
            fs_table.end()) {
            f.ret(kFsTableMiss);
            return;
        }

        // The inner writer initializes its byte-count output before checking
        // the file's write mode. A null table entry is possible when the
        // caller passes handle zero; it reaches the inner method and reports
        // its observed write-denied status (0x13), unlike a nonzero table miss.
        if (!store_guest_or_stop(f, out, 0u)) return;
        Vfs& vfs = f.emulator.vfs();
        if (!vfs.writable(handle)) {
            f.ret(kFsWriteDenied);
            return;
        }
        std::vector<u8> bytes(length);
        if (length != 0 &&
            (buffer == kNullGuest ||
             !f.memory.read_bytes(buffer, bytes.data(), length))) {
            // RetailOS passes the guest source directly to its copy/write
            // path. An invalid guest address faults there; it is not the
            // filesystem's write-denied status.
            f.cpu.request_stop();
            return;
        }
        const i64 written = vfs.write(handle, bytes.data(), length);
        if (written < 0) {
            f.ret(kFsWriteDenied);
            return;
        }
        if (!store_guest_or_stop(f, out, static_cast<u32>(written))) return;
        f.ret(0);
    });
}

}  // namespace clickhle
