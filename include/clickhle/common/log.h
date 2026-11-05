#pragma once

#include <sstream>
#include <string>
#include <string_view>

namespace clickhle::log {

enum class Level { Trace = 0, Debug, Info, Warn, Error, Off };

void set_level(Level level) noexcept;
[[nodiscard]] Level level() noexcept;
[[nodiscard]] bool enabled(Level level) noexcept;

/// Parses "trace" / "debug" / "info" / "warn" / "error" / "off".
[[nodiscard]] bool parse_level(std::string_view text, Level& out) noexcept;

void write(Level level, std::string_view category, std::string_view message);

/// Streaming helper: accumulates into a buffer and emits on destruction, so a
/// whole line lands atomically rather than interleaving with another thread's.
class Line {
public:
    Line(Level level, std::string_view category)
        : level_(level), category_(category) {}
    ~Line() { write(level_, category_, buffer_.str()); }

    Line(const Line&) = delete;
    Line& operator=(const Line&) = delete;

    template <typename T>
    Line& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

private:
    Level level_;
    std::string_view category_;
    std::ostringstream buffer_;
};

}  // namespace clickhle::log

// The `if (!enabled) ; else Line{}` shape is deliberate: it keeps the streamed
// arguments unevaluated when the level is off, and the trailing `else` swallows
// any dangling-else that would otherwise bind to a caller's `if`.
#define CLICKHLE_LOG_AT(lvl, cat)       \
    if (!::clickhle::log::enabled(lvl)) \
        ;                             \
    else                              \
        ::clickhle::log::Line { lvl, cat }

#define CLICKHLE_TRACE(cat) CLICKHLE_LOG_AT(::clickhle::log::Level::Trace, cat)
#define CLICKHLE_DEBUG(cat) CLICKHLE_LOG_AT(::clickhle::log::Level::Debug, cat)
#define CLICKHLE_INFO(cat)  CLICKHLE_LOG_AT(::clickhle::log::Level::Info, cat)
#define CLICKHLE_WARN(cat)  CLICKHLE_LOG_AT(::clickhle::log::Level::Warn, cat)
#define CLICKHLE_ERROR(cat) CLICKHLE_LOG_AT(::clickhle::log::Level::Error, cat)
