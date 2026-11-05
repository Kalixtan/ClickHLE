#include "clickhle/hle/call_frame.h"

#include "clickhle/common/log.h"

namespace clickhle {

void HleCallFrame::warn_bad_store(GuestAddr address) {
    CLICKHLE_WARN("hle") << "store to unmapped guest address 0x" << std::hex
                       << address;
}

void ArgReader::align_to_even() {
    if ((word_index_ & 1u) != 0) {
        ++word_index_;
    }
}

u32 ArgReader::next_word() {
    const unsigned index = word_index_++;
    if (index < 4) {
        return cpu_.reg(index);
    }
    // Stack arguments begin at sp; the first four words lived in registers.
    const GuestAddr address = cpu_.sp() + (index - 4) * 4;
    const auto value = memory_.read<u32>(address);
    if (!value.has_value()) {
        ok_ = false;
        return 0;
    }
    return *value;
}

u32 ArgReader::next_u32() { return next_word(); }

u64 ArgReader::next_u64() {
    align_to_even();
    const u64 low = next_word();
    const u64 high = next_word();
    return low | (high << 32);
}

}  // namespace clickhle
