#include "clickhle/core/cpu.h"
#include "clickhle/common/log.h"
#include "clickhle/hle/call_frame.h"
#include "clickhle/hle/classic_memory.h"
#include "clickhle/hle/registry.h"

#include <stdexcept>

namespace clickhle {

// The caller owns the arena and must keep it alive while the registry is used.
// The main-thread identifier is supplied by startup until thread scheduling is
// traced for this platform. Resize (#2) remains unregistered.
void register_classic_memory(HleRegistry& registry, classic::Arena& arena,
                             u16 thread_id) {
    registry.add("MemoryAlloc", 0, "classic_allocate", 1,
                 [&arena, thread_id](HleCallFrame& frame) {
        try {
            frame.ret(arena.allocate(frame.args.next_u32(), 1, thread_id));
        } catch (const std::runtime_error& error) {
            CLICKHLE_ERROR("classic") << "MemoryAlloc#0: " << error.what();
            frame.cpu.request_stop();
        }
    });
    registry.add("MemoryAlloc", 1, "classic_release", 1,
                 [&arena](HleCallFrame& frame) {
        try {
            arena.release(frame.args.next_ptr());
            frame.ret_void();
        } catch (const std::runtime_error& error) {
            CLICKHLE_ERROR("classic") << "MemoryAlloc#1: " << error.what();
            frame.cpu.request_stop();
        }
    });
}

} // namespace clickhle
