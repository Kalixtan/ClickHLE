#pragma once

// Loads Unicorn at runtime and resolves only the entry points used by the CPU
// adapter. This avoids a build-time dependency on Unicorn's headers or library.
//
// One process-wide instance supplies the function table to each CPU backend.

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <string>

#include "unicorn_shim.h"

namespace clickhle {

class UnicornDynload {
public:
    static UnicornDynload& instance() {
        static UnicornDynload lib;
        return lib;
    }

    UnicornDynload(const UnicornDynload&) = delete;
    UnicornDynload& operator=(const UnicornDynload&) = delete;

    [[nodiscard]] bool available() const { return handle_ != nullptr; }
    [[nodiscard]] const std::string& error() const { return error_; }

    unicorn_shim::fn_uc_open open = nullptr;
    unicorn_shim::fn_uc_close close = nullptr;
    unicorn_shim::fn_uc_strerror strerror = nullptr;
    unicorn_shim::fn_uc_reg_read reg_read = nullptr;
    unicorn_shim::fn_uc_reg_write reg_write = nullptr;
    unicorn_shim::fn_uc_mem_map_ptr mem_map_ptr = nullptr;
    unicorn_shim::fn_uc_emu_start emu_start = nullptr;
    unicorn_shim::fn_uc_emu_stop emu_stop = nullptr;
    unicorn_shim::fn_uc_hook_add hook_add = nullptr;
    unicorn_shim::fn_uc_ctl_set_cpu_model ctl_set_cpu_model = nullptr;

private:
    UnicornDynload() {
#if defined(_WIN32)
        constexpr const char* candidates[] = {"unicorn.dll"};
#elif defined(__APPLE__)
        constexpr const char* candidates[] = {
            "libunicorn.2.dylib", "libunicorn.dylib"};
#else
        constexpr const char* candidates[] = {"libunicorn.so.2", "libunicorn.so"};
#endif
        for (const char* candidate : candidates) {
            handle_ = open_library(candidate);
            if (handle_ != nullptr) {
                break;
            }
        }
        if (handle_ == nullptr) {
            error_ =
                "Unicorn Engine was not found. Install its runtime library "
                "to run games.";
            return;
        }
        if (!resolve_all()) {
            error_ =
                "Unicorn Engine was found but is missing a required symbol; "
                "the installed version may be incompatible.";
            close_library(handle_);
            handle_ = nullptr;
        }
    }

    ~UnicornDynload() {
        if (handle_ != nullptr) {
            close_library(handle_);
        }
    }

    template <typename Fn>
    bool bind(Fn& target, const char* symbol) {
#if defined(_WIN32)
        const auto address = GetProcAddress(handle_, symbol);
#else
        void* address = dlsym(handle_, symbol);
#endif
        if (address == nullptr) {
            return false;
        }
        target = reinterpret_cast<Fn>(address);
        return true;
    }

#if defined(_WIN32)
    static HMODULE open_library(const char* path) { return LoadLibraryA(path); }
    static void close_library(HMODULE handle) { FreeLibrary(handle); }
    using Handle = HMODULE;
#else
    static void* open_library(const char* path) {
        return dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    static void close_library(void* handle) { dlclose(handle); }
    using Handle = void*;
#endif

    bool resolve_all() {
        // ctl_set_cpu_model binds to the real "uc_ctl" symbol -- a variadic
        // C function -- through a fixed-arity (uc_engine*, int, int)
        // function pointer. Safe in practice on the ABI Unicorn ships on
        // (same pattern already used for uc_hook_add above).
        return bind(open, "uc_open") && bind(close, "uc_close") &&
              bind(strerror, "uc_strerror") && bind(reg_read, "uc_reg_read") &&
              bind(reg_write, "uc_reg_write") &&
              bind(mem_map_ptr, "uc_mem_map_ptr") &&
              bind(emu_start, "uc_emu_start") && bind(emu_stop, "uc_emu_stop") &&
              bind(hook_add, "uc_hook_add") &&
              bind(ctl_set_cpu_model, "uc_ctl");
    }

    Handle handle_ = nullptr;
    std::string error_;
};

}  // namespace clickhle
