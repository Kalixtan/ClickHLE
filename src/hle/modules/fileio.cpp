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

#include <string>
#include <cstdio>
#include <cstdlib>
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

std::string read_path(HleCallFrame& frame, GuestAddr address) {
    std::string out;
    for (u32 i = 0; i < kMaxPath; ++i) {
        const auto c = frame.memory.read<u8>(address + i);
        if (!c.has_value() || *c == 0) {
            break;
        }
        out.push_back(static_cast<char>(*c));
    }
    return out;
}

}  // namespace

/// Request object fields, in bytes.
///
/// These offsets are ABI facts -- the shape of a structure the *games* build --
/// established by the DogParty project's reversing and reused here as facts.
/// None of their code is copied; see LICENSING.md section 5.
namespace request {
constexpr u32 kOperation = 0x04;   ///< byte: 3 = read, 4 = WRITE, 5 = seek
constexpr u32 kFileObject = 0x08;  ///< the game's file object
constexpr u32 kSeekOffset = 0x0C;
constexpr u32 kSeekWhence = 0x10;  ///< byte: 0 set, 1 current, 2 end
constexpr u32 kBuffer = 0x14;
constexpr u32 kLength = 0x18;
constexpr u32 kStatus = 0x20;      ///< 0 on success, all ones on failure
constexpr u32 kSizeResult = 0x24;
constexpr u32 kPositionResult = 0x28;
/// Where the firmware leaves the handle for the game to cache. Later requests
/// are resolved through *this* field, not through the file object, so an open
/// that does not write it leaves the 0xffffffff the game put there and every
/// following request names a stream that does not exist.
constexpr u32 kHandle = 0x2C;
}  // namespace request

/// The file object's result word -- the result of the *operation*, which is not
/// always the handle. For an open that loaded, it is how many bytes arrived.
constexpr u32 kFileObjectResult = 0x08;

constexpr u32 kFailure = 0xFFFFFFFFu;

/// The save store's errno for a null handle argument (0x1016be30). The store
/// slots (#12/#14/#16) are errno-style, 0 = success -- the opposite of the
/// queued slots, which are boolean with 1 = accepted. Specific to #12 --
/// #14/#16 have their own, different failure code (kStoreGenericFailure).
constexpr u32 kStoreNullHandle = 0x13;

/// #13-#16's own failure code, measured by oracle calling into the real
/// firmware on a warm machine with no store (2026-09-19): #12 returns 0x13,
/// #13-#16 return 8. Distinct from kStoreNullHandle (#12's own code) and
/// from kFailure (the unrelated queued-slot completion-status sentinel) --
/// conflating either with this one would misreport #14/#16's real errno.
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
        const u32 mode = f.args.next_u32();
        const GuestAddr path_address = f.args.next_ptr();
        const GuestAddr req = f.args.next_ptr();
        const std::string path = read_path(f, path_address);
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
        const auto vfs_mode = mode == 0 ? Vfs::Mode::kRead : Vfs::Mode::kUpdate;
        const u32 handle = f.emulator.vfs().open(path, vfs_mode);
        if (handle == 0) {
            f.store<u32>(req + request::kStatus, kFailure);
            CLICKHLE_INFO(kCategory) << "open(\"" << path << "\") FAILED";
            f.ret(0);
            return;
        }

        Vfs& vfs = f.emulator.vfs();
        const i64 size = vfs.size(handle);
        f.store<u32>(req + request::kHandle, handle);

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
            std::vector<u8> bytes(*length);
            loaded = vfs.read(handle, bytes.data(), *length);
            for (i64 i = 0; i < loaded; ++i) {
                f.store<u8>(*buffer + static_cast<u32>(i),
                            bytes[static_cast<std::size_t>(i)]);
            }
            CLICKHLE_DEBUG(kCategory)
                << "open loaded " << loaded << " of " << *length
                << " requested bytes";
        }
        // The size result is what the operation produced: the bytes delivered
        // when the open loaded, otherwise the file's size.
        const u32 result =
            loaded > 0 ? static_cast<u32>(loaded)
                       : (size < 0 ? 0u : static_cast<u32>(size));
        f.store<u32>(req + request::kSizeResult, result);
        f.store<u32>(req + request::kPositionResult,
                     static_cast<u32>(vfs.tell(handle)));
        f.store<u32>(req + request::kStatus, 0);

        // The file object's result word takes the *operation's* result. For an
        // open that loaded, that is the byte count -- a title hands it on as an
        // image size, so writing the handle here would tell the renderer its
        // data is one byte long.
        const auto object = f.memory.read<u32>(req + request::kFileObject);
        if (object.has_value() && *object != 0) {
            f.store<u32>(*object + kFileObjectResult, result);
        }
        CLICKHLE_INFO(kCategory)
            << "open(\"" << path << "\") -> handle " << handle << ", " << size
            << " bytes";
        // The operation is done; tell the game so. Without this a title sits on
        // its loading screen forever -- it is not polling anything, it is
        // waiting to be called back.
        f.emulator.queue_completion(req, "afio_open_alt(#3)");
        // 1, not the handle -- boolean contract, see #0. 0x101da9cc returns 0
        // at 0x101daa0c and 1 at 0x101daa50.
        f.ret(1);
    });

    // read(request) -- performs whatever the request describes.
    //
    // One argument, which is what settles the design: the operation, buffer,
    // length and position all live in the request object, so the object is the
    // contract and this is what advances it.
    registry.add(kModule, 2, "afio_read", 1, [](HleCallFrame& f) {
        const GuestAddr req = f.args.next_ptr();
        const auto handle = f.memory.read<u32>(req + request::kHandle);
        const auto operation = f.memory.read<u8>(req + request::kOperation);
        if (!handle.has_value() || !operation.has_value() ||
            *handle == 0 || *handle == kFailure) {
            f.store<u32>(req + request::kStatus, kFailure);
            // 0 = refused, and it has to be. The error code belongs in the
            // request's status word, never in r0: this path queues NO
            // completion, so returning a truthy 0x1f told the guest the request
            // had been accepted and then never called it back -- a guaranteed
            // hang rather than a reported failure.
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
                f.store<u32>(req + request::kStatus, kBadWhence);
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
                f.store<u32>(req + request::kStatus, kPastEnd);
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
            f.store<u32>(req + request::kStatus, ok ? 0u : kPastEnd);
            if (std::getenv("CLICKHLE_NO_SEEK_COMPLETION") == nullptr) {
                f.emulator.queue_io_completion(req, "");
            }
            // 1 on success, NOT 0.
            //
            // The firmware's afio_perform returns 1 when it has enqueued the
            // operation and 0 when it refused it, and the guest tests that as a
            // boolean: stream_seek only advances stream[0x108] to 2 `if (ok)`.
            //
            // Returning 0 for a successful seek meant Mahjong issued the seek,
            // was told the issue had FAILED, left its state at 0, and re-issued
            // on the next pass -- 592 times. The state machine took the
            // "state == 0" arm 594 times out of 594 and never once reached
            // case 2. Everything downstream was working; the game was simply
            // never told the request had been accepted.
            f.ret(ok ? 1u : 0u);
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
                f.store<u32>(req + request::kStatus, kFailure);
                f.ret(0);
                return;
            }
            std::vector<u8> bytes(*length);
            for (u32 i = 0; i < *length; ++i) {
                bytes[i] = f.memory.read<u8>(*buffer + i).value_or(0);
            }
            const i64 written = vfs.write(*handle, bytes.data(), *length);
            CLICKHLE_DEBUG(kCategory)
                << "write " << written << " of " << *length
                << " bytes on handle " << *handle;
            f.store<u32>(req + request::kSizeResult,
                         written < 0 ? 0u : static_cast<u32>(written));
            // Op 4's extra step: it fetches the resulting file position into
            // +0x28 (0x1024c900), which is a write-shaped thing to want.
            f.store<u32>(req + request::kPositionResult,
                         static_cast<u32>(vfs.tell(*handle)));
            f.store<u32>(req + request::kStatus, written < 0 ? kFailure : 0u);
            f.emulator.queue_io_completion(req, "");
            f.ret(1);
            return;
        }

        // Anything else is a read at the current position.
        const auto buffer = f.memory.read<u32>(req + request::kBuffer);
        const auto length = f.memory.read<u32>(req + request::kLength);
        if (!buffer.has_value() || !length.has_value()) {
            f.store<u32>(req + request::kStatus, kFailure);
            // Refused, no completion queued -- 0. See the bad-handle arm.
            f.ret(0);
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
            f.store<u32>(req + request::kStatus, kFailure);
            // Refused, no completion queued -- 0. See the bad-handle arm.
            f.ret(0);
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
            f.store<u32>(req + request::kStatus, kFailure);
            f.ret(0);
            return;
        }
        f.store<u32>(req + request::kSizeResult, static_cast<u32>(got));
        f.store<u32>(req + request::kPositionResult,
                     static_cast<u32>(vfs.tell(*handle)));
        f.store<u32>(req + request::kStatus, 0);

        const auto object = f.memory.read<u32>(req + request::kFileObject);
        if (object.has_value() && *object != 0) {
            f.store<u32>(*object + kFileObjectResult, static_cast<u32>(got));
        }
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
        const auto handle = f.memory.read<u32>(req + request::kHandle);
        CLICKHLE_DEBUG(kCategory)
            << "afio_close: handle " << handle.value_or(0);
        if (handle.has_value()) {
            (void)f.emulator.vfs().close(*handle);
        }
        f.store<u32>(req + request::kStatus, 0);
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

    // #4 carries the request in r2 rather than r0 and otherwise behaves as a
    // close: complete it with nothing to do.
    registry.add(kModule, 4, "afio_operation", 3, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        (void)f.args.next_u32();
        const GuestAddr req = f.args.next_ptr();
        const auto handle = f.memory.read<u32>(req + request::kHandle);
        if (handle.has_value()) {
            (void)f.emulator.vfs().close(*handle);
        }
        f.store<u32>(req + request::kStatus, 0);
        // Close-like, so no completion -- same reasoning as #1, and the same
        // boolean return: 0x101daa58 returns 0 at 0x101daa9c and 1 at
        // 0x101daae0.
        f.ret(1);
    });

    // #0 afio_open(mode, path, flag, request) -- the four-argument open.
    //
    // The order matters and was wrong: the extra word is the *third* argument,
    // not the first. Call sites across the corpus all read
    // (0, "Sudoku.rlb", 1, request), (0, "jdmgsheets0", 1, request),
    // (0, "Localization/en.lproj/text.strings", 1, request) -- mode first, then
    // the path. Skipping a leading word instead read the path pointer as the
    // mode and guest address 1 as the path, so every #0 open failed with an
    // empty name, silently, once per frame forever.
    registry.add(kModule, 0, "afio_open", 4, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32();
        const GuestAddr path_address = f.args.next_ptr();
        (void)f.args.next_u32();
        const GuestAddr req = f.args.next_ptr();
        const std::string path = read_path(f, path_address);
        dump_request(f, req);
        // mode 0 is an asset ("Strings.dta", "audio/bad.wav"); mode 1 is a
        // save ("savefile.dat", "pac_man.dat", "jdmgp.sav"). A save is opened
        // for update, not for writing -- see Vfs::Mode::kUpdate.
        const auto vfs_mode = mode == 0 ? Vfs::Mode::kRead : Vfs::Mode::kUpdate;
        Vfs& vfs = f.emulator.vfs();
        const u32 handle = vfs.open(path, vfs_mode);
        if (handle == 0) {
            f.store<u32>(req + request::kStatus, kFailure);
            // A failure is still a completion. A title that is never told its
            // open failed retries it forever, which looks exactly like a hang.
            CLICKHLE_INFO(kCategory) << "open4(\"" << path << "\") FAILED";
            // No completion, and this is the difference between #0 and #3.
        //
        // #3 is "open and load" -- it carries a destination buffer and the data
        // is there when it returns, so a completion is owed and titles wait for
        // it. #0 is the streaming open: operation 1, no buffer, nothing
        // transferred. Completing it claims a load finished that never started.
        //
        // Bisected rather than reasoned. Texas Hold'em issues 20 #3 completions
        // and 2 #0 completions; suppressing only the #0 pair takes it from
        // faulting at frame 38 to running all 60 and drawing 202 times, and
        // Vortex from 6 draws to 180. No title in the corpus is worse without
        // it, and the fault count across all 20 goes from 1 to 0.
        //
        // The shape of the failure fits: a title told a stream had finished
        // loading walked a table it had not finished filling
        // (ABI_FINDINGS section 41).
        //
        // Re-tested 2026-09-11: the *trampoline* completion changes nothing.
        // The streaming open instead completes through the file-IO list on
        // app+0x2c, which the frame driver publishes once per frame.
        if (std::getenv("CLICKHLE_NO_OPEN_COMPLETION") == nullptr) {
            f.emulator.queue_io_completion(req, path);
        }
            f.ret(0);
            return;
        }
        const i64 size = vfs.size(handle);
        f.store<u32>(req + request::kHandle, handle);
        i64 loaded = 0;
        const auto buffer = f.memory.read<u32>(req + request::kBuffer);
        const auto length = f.memory.read<u32>(req + request::kLength);
        // Both modes load here. A save open carries an empty destination
        // buffer and expects its contents back, exactly as an asset open does.
        if (buffer.value_or(0) != 0 && length.value_or(0) != 0) {
            std::vector<u8> bytes(*length);
            loaded = vfs.read(handle, bytes.data(), *length);
            for (i64 i = 0; i < loaded; ++i) {
                f.store<u8>(*buffer + static_cast<u32>(i),
                            bytes[static_cast<std::size_t>(i)]);
            }
        }
        // +0x24 is BYTES TRANSFERRED and is written by ops 3, 4, 6 and 7 --
        // NOT by op 1. A game reads it as a byte count, so an open that
        // transferred nothing must not claim the file's size arrived: Mahjong's
        // completion handler takes `count = request[0x24]` and hands it to its
        // own callback, so telling it 7,292,512 bytes landed when zero did is
        // worse than telling it nothing.
        const u32 result = loaded > 0 ? static_cast<u32>(loaded)
                                      : (size < 0 ? 0u : static_cast<u32>(size));
        // The two firmware readings conflict here and the working one wins.
        //
        // The LLE track's opcode table says +0x24 is written by ops 3/4/6/7 and
        // not by op 1. The recompiler's *working* implementation sets it to the
        // FILE SIZE on a bufferless open, commented as exactly that. Mahjong's
        // completion handler reads `count = request[0x24]` and currently gets
        // 0, then seeks and retries -- 592 times.
        //
        // A table derived by reading is weaker evidence than an implementation
        // that runs the titles, so: bytes for a transfer, size for a bufferless
        // open.
        f.store<u32>(req + request::kSizeResult, result);
        // An open does NOT write +0x28.
        //
        // The result reaches fileobject[0x08] by this handler writing it there
        // directly, below -- which is what the recompiler's working
        // implementation does. Writing +0x28 as well makes the guest's
        // completion processor copy it a second time, and that is a regression:
        // it faults Texas Hold'em at frame 36, the same fault as section 45.
        //
        // The courier at +0x28 is for operations whose result the open path
        // does not deliver itself.
        f.store<u32>(req + request::kStatus, 0);
        const auto object = f.memory.read<u32>(req + request::kFileObject);
        if (object.has_value() && *object != 0) {
            f.store<u32>(*object + kFileObjectResult, result);
        }
        CLICKHLE_INFO(kCategory)
            << "open4(\"" << path << "\") -> handle " << handle << ", "
            << size << " bytes, loaded " << loaded;
        // Same request contract as #3, so the same completion is owed.
        // No completion, and this is the difference between #0 and #3.
        //
        // #3 is "open and load" -- it carries a destination buffer and the data
        // is there when it returns, so a completion is owed and titles wait for
        // it. #0 is the streaming open: operation 1, no buffer, nothing
        // transferred. Completing it claims a load finished that never started.
        //
        // Bisected rather than reasoned. Texas Hold'em issues 20 #3 completions
        // and 2 #0 completions; suppressing only the #0 pair takes it from
        // faulting at frame 38 to running all 60 and drawing 202 times, and
        // Vortex from 6 draws to 180. No title in the corpus is worse without
        // it, and the fault count across all 20 goes from 1 to 0.
        //
        // The shape of the failure fits: a title told a stream had finished
        // loading walked a table it had not finished filling
        // (ABI_FINDINGS section 41).
        //
        // Re-tested 2026-09-11: the *trampoline* completion changes nothing.
        // The streaming open instead completes through the file-IO list on
        // app+0x2c, which the frame driver publishes once per frame.
        if (std::getenv("CLICKHLE_NO_OPEN_COMPLETION") == nullptr) {
            f.emulator.queue_io_completion(req, path);
        }
        // 1, not the handle. Every queued AsyncFileIO slot (#0-#4) returns a
        // boolean: 1 accepted, 0 refused -- 0x101da410 returns 0 at 0x101da458
        // and 1 at 0x101da498. The handle happens to be non-zero here, so the
        // guest's boolean test passed by luck rather than by contract; the
        // caller reads it as a flag and never as a handle, which it takes from
        // request[0x2c].
        f.ret(1);
    });

    // #7(mode, path, a3, out) -- a directory operation. iQuiz is the only
    // caller in the corpus; it asks for "UserTrivia/Packs".
    //
    // CORRECTION (2026-09-13): the note here used to say that directory "does
    // not exist in this corpus". It does. `UserTrivia/Packs/` ships with the
    // title and is simply EMPTY -- which is a different thing, and the real
    // device would answer it differently: "directory present, no entries"
    // rather than "no such directory". We conflate the two.
    //
    // Kept as-is anyway, because it was measured NOT to be what blocks iQuiz.
    // Sweeping the return value (0/1/2/-1) and writing the `out` struct made
    // no difference at all: 900 draw calls and 2597 colours every time, the
    // logo screen unchanged. So this is a correctness wart to fix when the
    // ordinal's real contract is known, not the cause of the stall -- and
    // guessing a contract for an out-struct we cannot verify is how the
    // Settings#0 -50 regression happened.
    //
    // Live argument dump: CLICKHLE_DUMP_OPENDIR=1. Measured shape is
    // mode=0, a3=null, out=<uninitialised 6-word struct we never write>.
    registry.add(kModule, 7, "afio_open_directory", 4, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32();
        const GuestAddr path_address = f.args.next_ptr();
        const GuestAddr a3 = f.args.next_ptr();
        const GuestAddr out = f.args.next_ptr();
        const std::string path = read_path(f, path_address);
        if (std::getenv("CLICKHLE_DUMP_OPENDIR") != nullptr) {
            CLICKHLE_INFO(kCategory)
                << "open_directory(\"" << path << "\") mode=" << mode
                << " a3=0x" << std::hex << a3 << " out=0x" << out << std::dec;
        }
        CLICKHLE_DEBUG(kCategory)
            << "open_directory(\"" << path << "\") -> none";
        f.ret(0);
    });

    // The save store: #12/#14/#16.
    //
    // Separate from the asset filesystem -- a place a title keeps its progress,
    // which is what Sudoku opening "savefile.dat" in write mode is. Handled
    // through the same VFS, so a save lands in the game's own directory beside
    // its assets, exactly where the device would put it.
    registry.add(kModule, 12, "afio_store_open", 3, [](HleCallFrame& f) {
        (void)f.args.next_u32();
        const GuestAddr path_address = f.args.next_ptr();
        (void)f.args.next_u32();
        const std::string path = read_path(f, path_address);
        const u32 handle = f.emulator.vfs().open(path, Vfs::Mode::kWrite);
        CLICKHLE_INFO(kCategory)
            << "store_open(\"" << path << "\") -> "
            << (handle != 0 ? "ok" : "FAILED");
        // ERRNO, 0 = SUCCESS -- the opposite of the queued slots, and the
        // fourth time this ABI's return conventions have cost a blocker.
        //
        // `AsyncFileIO` has two opposite conventions in one interface: #0-#4 are
        // boolean with 1 = accepted, while the save store (#12/#14/#16) goes
        // through 0x1016bd44 instead of the IO context and returns errno-style
        // codes -- 0 on success, 0x13 when the handle argument is null
        // (0x1016be24). Returning the VFS handle here meant every successful
        // open handed the guest a non-zero value, which it reads as an error
        // code, so it retried forever: 199 opens against 34 closes, with
        // store_open("en/stats") repeating to the end of the run.
        //
        // OPEN QUESTION, deliberately not guessed at: where the handle is
        // supposed to be published. It cannot be r0 if r0 is an errno, so there
        // is an out-parameter we are not writing. The argument read above
        // demonstrably yields correct paths, so the layout is not simply
        // shifted. Fixing the return is the part the evidence supports; if
        // store_write then fails on a bad handle, that identifies the
        // out-parameter and is the next thing to chase.
        // Investigation hook: make every store_open fail cleanly, to test
        // whether the save path is the blocker or merely a symptom.
        if (std::getenv("CLICKHLE_STORE_FAIL") != nullptr) {
            f.ret(kStoreNullHandle);
            return;
        }
        f.ret(handle != 0 ? 0u : kStoreNullHandle);
    });

    registry.add(kModule, 14, "afio_store_close", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        f.ret(f.emulator.vfs().close(handle) ? 0 : kStoreGenericFailure);
    });

    registry.add(kModule, 16, "afio_store_write", 3, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        const GuestAddr buffer = f.args.next_ptr();
        const u32 length = f.args.next_u32();
        std::vector<u8> bytes(length);
        for (u32 i = 0; i < length; ++i) {
            bytes[i] = f.memory.read<u8>(buffer + i).value_or(0);
        }
        const i64 written = f.emulator.vfs().write(handle, bytes.data(), length);
        CLICKHLE_DEBUG(kCategory) << "store_write " << written << " bytes";
        // Errno, 0 = success -- same group as #12, NOT a byte count. Returning
        // the number written meant every successful write of N bytes reported
        // error N. Failure code is #14-#16's own (kStoreGenericFailure, = 8),
        // NOT #12's kStoreNullHandle (0x13) -- oracle measured these as two
        // different codes, 2026-09-19.
        f.ret(written < 0 ? kStoreGenericFailure : 0u);
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
    // #0 returns 0x17 when the open fails. #2 and #3's bad-handle codes were
    // an unconfirmed placeholder (0x20) until oracle called both directly
    // against real firmware (2026-09-19, no arguments, a cold handle table):
    // #2 (read) returns 0x02, #3 (size) returns 0x13. The two differ -- a
    // read and a size query do not fail alike -- so they get separate
    // constants rather than sharing one. #3's 0x13 is bit-identical to
    // kStoreNullHandle, AsyncFileIO#12's own failure code: two unrelated
    // interfaces producing the same non-obvious value, which reads like a
    // shared system errno space rather than coincidence.
    static constexpr u32 kFsOpenFailed = 0x17;
    static constexpr u32 kFsReadBadHandle = 0x02;
    static constexpr u32 kFsSizeBadHandle = kStoreNullHandle;
    static std::vector<u32> fs_table;
    fs_table.assign(kFsSlots, 0);

    registry.add(kFilesystemModule, 0, "fs_open", 4, [](HleCallFrame& f) {
        const u32 mode = f.args.next_u32();
        const GuestAddr path_address = f.args.next_ptr();
        (void)f.args.next_u32();
        const GuestAddr out = f.args.next_ptr();
        const std::string path = read_path(f, path_address);

        // The handle goes out through the fourth argument; the *return value is
        // a status*. We had been returning the handle, which a title checking
        // for zero-means-success reads as a failure on every successful open.
        std::size_t slot = 0;
        while (slot < kFsSlots && fs_table[slot] != 0) {
            ++slot;
        }
        if (slot == kFsSlots) {
            CLICKHLE_WARN(kCategory)
                << "fs_open(\"" << path << "\"): all " << kFsSlots
                << " handle slots are in use, as on hardware";
            if (out != kNullGuest) {
                f.store<u32>(out, 0);
            }
            f.ret(kFsOpenFailed);
            return;
        }

        // mode 0 is an asset ("Strings.dta", "audio/bad.wav"); mode 1 is a
        // save ("savefile.dat", "pac_man.dat", "jdmgp.sav"). A save is opened
        // for update, not for writing -- see Vfs::Mode::kUpdate.
        const auto vfs_mode = mode == 0 ? Vfs::Mode::kRead : Vfs::Mode::kUpdate;
        const u32 handle = f.emulator.vfs().open(path, vfs_mode);
        if (out != kNullGuest) {
            f.store<u32>(out, handle);
        }
        if (handle == 0) {
            CLICKHLE_INFO(kCategory)
                << "fs_open(\"" << path << "\") -> MISSING";
            f.ret(kFsOpenFailed);
            return;
        }
        fs_table[slot] = handle;
        CLICKHLE_INFO(kCategory) << "fs_open(\"" << path << "\") -> ok";
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
    // NOT applied to #3 (fs_size): its claimed layout carries the same
    // unverified swap, but no title in the corpus calls it, so there is no
    // evidence either way and guessing symmetrically would repeat exactly the
    // mistake this comment documents.
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
        if (std::find(fs_table.begin(), fs_table.end(), handle) ==
            fs_table.end()) {
            f.ret(kFsReadBadHandle);
            return;
        }
        std::vector<u8> bytes(length);
        const i64 got = f.emulator.vfs().read(handle, bytes.data(), length);
        for (i64 i = 0; i < got; ++i) {
            f.store<u8>(buffer + static_cast<u32>(i),
                        bytes[static_cast<std::size_t>(i)]);
        }
        if (out != kNullGuest) {
            f.store<u32>(out, got < 0 ? 0u : static_cast<u32>(got));
        }
        CLICKHLE_DEBUG(kCategory)
            << "fs_read(" << handle << ", " << length << ") -> " << got;
        f.ret(got < 0 ? kFsReadBadHandle : 0u);
    });

    registry.add(kFilesystemModule, 1, "fs_close", 1, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        // Free the table slot too, or the ten-handle ceiling becomes a
        // ten-open-ever limit and a title that opens and closes in a loop
        // starts failing partway through.
        const auto slot = std::find(fs_table.begin(), fs_table.end(), handle);
        if (slot != fs_table.end()) {
            *slot = 0;
        }
        (void)f.emulator.vfs().close(handle);
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

    // #3(handle, a, out, d) -- four arguments, not two. The decompiled body
    // references param_3 and param_4 by name, and it forwards (p1, p3, p2, p4)
    // like #2, so the out-pointer is the *third* argument.
    //
    // Under-declaring this at 2 silently discarded two arguments the firmware
    // reads. Argument layout is from the firmware decompile, not observed
    // behaviour -- same caveat as #2. The failure return value is
    // oracle-measured; see kFsSizeBadHandle above.
    registry.add(kFilesystemModule, 3, "fs_size", 4, [](HleCallFrame& f) {
        const u32 handle = f.args.next_u32();
        (void)f.args.next_u32();
        const GuestAddr out = f.args.next_ptr();
        (void)f.args.next_u32();
        if (std::find(fs_table.begin(), fs_table.end(), handle) ==
            fs_table.end()) {
            f.ret(kFsSizeBadHandle);
            return;
        }
        const i64 size = f.emulator.vfs().size(handle);
        if (out != kNullGuest) {
            f.store<u32>(out, size < 0 ? 0u : static_cast<u32>(size));
        }
        f.ret(size < 0 ? kFsSizeBadHandle : 0u);
    });
}

}  // namespace clickhle
