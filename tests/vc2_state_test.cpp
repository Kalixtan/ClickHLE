#include "core.h"
#include <iostream>
using namespace clickhle::vc2;
int main() {
    Core original;
    original.write(0x100, 0x12345678);
    original.r[0] = 9;
    original.vrf[30] = 13;
    auto copy = original.fork_reference();
    if (!original.state_difference(*copy).empty()) return 1;
    copy->write(0x100, 0x87654321);
    if (original.read(0x100) != 0x12345678 || original.state_difference(*copy) != "RAM at 0x100") return 1;
    copy->write(0x100, 0x12345678);
    copy->vn[3] = true;
    if (original.state_difference(*copy) != "vn") return 1;
    copy->vn[3] = false;
    copy->advance(17);
    if (original.state_difference(*copy).empty()) return 1;
    copy = original.fork_reference();
    copy->set_gpio_input(49, true);
    if (original.state_difference(*copy).empty()) return 1;
    std::cout << "Reference fork isolates RAM and detects flags, timers and GPIO changes\n";
}
