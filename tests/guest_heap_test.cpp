#include "clickhle/hle/guest_heap.h"

#include <iostream>

int main() {
    clickhle::GuestHeap heap;
    heap.initialise(0x20000000, 0x2000);
    const auto first = heap.allocate(0x100);
    const auto second = heap.allocate(0x100);
    if (first == 0 || second < first + 0x100 + clickhle::GuestHeap::kHeaderBytes) {
        std::cerr << "guest allocations have no header gap\n";
        return 1;
    }
    if (heap.size_of(first) != 0x100 || heap.size_of(second) != 0x100) {
        std::cerr << "payload size includes allocator metadata\n";
        return 1;
    }
    if (!heap.release(first) || heap.allocate(0x80) != first) {
        std::cerr << "released allocation was not reused\n";
        return 1;
    }
    return 0;
}
