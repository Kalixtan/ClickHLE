#include "clickhle/common/log.h"

#include <atomic>
#include <iostream>
#include <mutex>

namespace clickhle::log {
namespace {

std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;

const char* level_tag(Level level) noexcept {
    switch (level) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info:  return "info ";
        case Level::Warn:  return "warn ";
        case Level::Error: return "error";
        case Level::Off:   return "off  ";
    }
    return "?????";
}

}  // namespace

void set_level(Level level) noexcept {
    g_level.store(level, std::memory_order_relaxed);
}

Level level() noexcept { return g_level.load(std::memory_order_relaxed); }

bool enabled(Level lvl) noexcept {
    return lvl >= g_level.load(std::memory_order_relaxed) && lvl != Level::Off;
}

bool parse_level(std::string_view text, Level& out) noexcept {
    if (text == "trace") { out = Level::Trace; return true; }
    if (text == "debug") { out = Level::Debug; return true; }
    if (text == "info")  { out = Level::Info;  return true; }
    if (text == "warn")  { out = Level::Warn;  return true; }
    if (text == "error") { out = Level::Error; return true; }
    if (text == "off")   { out = Level::Off;   return true; }
    return false;
}

void write(Level lvl, std::string_view category, std::string_view message) {
    if (!enabled(lvl)) {
        return;
    }
    std::ostream& out = (lvl >= Level::Warn) ? std::cerr : std::cout;
    const std::lock_guard<std::mutex> guard(g_mutex);
    // Flushed per line, deliberately. A run that hangs gets killed, and
    // buffered output dies with it -- which is exactly the run whose log you
    // needed. Tracing is slower for it; being able to read the last thing that
    // happened is worth more.
    out << '[' << level_tag(lvl) << "] " << category << ": " << message
        << std::endl;
}

}  // namespace clickhle::log
