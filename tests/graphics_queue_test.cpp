#include "graphics_api.h"
#include <array>
#include <stdexcept>
#include <vector>

using namespace clickhle;

int main() {
    std::vector<std::vector<u8>> submitted;
    bool reject = false;
    GraphicsApi api([&](std::span<const u8> bytes) {
        if (reject) throw std::runtime_error("submission failed");
        submitted.emplace_back(bytes.begin(), bytes.end());
    });
    const std::array<u8, 4> first{0x11, 0x22, 0x33, 0x44};
    api.queue({0x12345678, 2, 3, 4}, first);
    api.flush();
    const std::array<u8, 1> second{0x99};
    api.queue({1, 2, 3, 4}, second);
    reject = true;
    try { api.flush(); return 1; } catch (const std::runtime_error &) {}
    if (api.pending_packets().size() != 20) return 1;
    reject = false;
    api.flush();
    if (submitted.size() != 2 || submitted[1].size() != 20 ||
        submitted[1][16] != 0x99 || submitted[1][17] != 0x22 ||
        submitted[1][18] != 0x33 || submitted[1][19] != 0x44) return 1;

    // A full batch flushes before the next packet. An aliased payload still
    // refers to the original bytes, not the new packet's overwritten header.
    const std::vector<u8> large(0x2000 - 16, 0x5a);
    api.queue({0x12345678, 0, 0, 0}, large);
    const auto alias = api.pending_packets().first(4);
    api.queue({0xaabbccdd, 0, 0, 0}, alias);
    if (submitted.size() != 3 || submitted.back().size() != 0x2000) return 1;
    const auto pending = api.pending_packets();
    if (pending.size() != 20 || pending[16] != 0x78 || pending[17] != 0x56 ||
        pending[18] != 0x34 || pending[19] != 0x12) return 1;
    api.flush();
    return api.pending_packets().empty() && submitted.size() == 4 ? 0 : 1;
}
