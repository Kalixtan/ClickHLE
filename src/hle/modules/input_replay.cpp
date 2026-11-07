#include "clickhle/input/replay.h"

#include <exception>
#include <sstream>

namespace clickhle {
namespace {

bool number(const std::string& text, u64& value) {
    if (text.empty() || text.front() == '-') return false;
    try {
        std::size_t used = 0;
        value = std::stoull(text, &used, 0);
        return used == text.size();
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace

bool parse_replay(std::string_view text, std::vector<ReplayEntry>& entries,
                  std::string& error) {
    entries.clear();
    std::istringstream lines{std::string(text)};
    std::string line;
    u64 line_number = 0;
    while (std::getline(lines, line)) {
        ++line_number;
        if (const auto comment = line.find('#'); comment != std::string::npos)
            line.erase(comment);
        std::istringstream words(line);
        std::string polls_text, word_text, extra;
        if (!(words >> polls_text)) continue;
        if (!(words >> word_text) || (words >> extra)) {
            error = "line " + std::to_string(line_number) + ": expected polls port_word";
            return false;
        }
        u64 polls = 0, word = 0;
        if (!number(polls_text, polls) || polls == 0 ||
            !number(word_text, word) || word > 0xffffffffull) {
            error = "line " + std::to_string(line_number) + ": invalid port state";
            return false;
        }
        entries.push_back({polls, static_cast<u32>(word)});
    }
    return true;
}

} // namespace clickhle
