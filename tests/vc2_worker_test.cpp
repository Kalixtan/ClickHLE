#include "worker.h"
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

using clickhle::vc2::Worker;

int main() {
    const auto caller = std::this_thread::get_id();
    std::vector<int> order;
    std::future<void> last;
    {
        Worker worker;
        auto first = worker.submit([&] {
            if (std::this_thread::get_id() == caller) throw std::runtime_error("wrong thread");
            order.push_back(1);
        });
        auto failing = worker.submit([] { throw std::runtime_error("expected failure"); });
        last = worker.submit([&] { order.push_back(2); });
        first.get();
        try {
            failing.get();
            return 1;
        } catch (const std::runtime_error &) {
        }
        // Destruction must drain queued work even if its future is not awaited.
    }
    last.get();
    return order == std::vector<int>{1, 2} ? 0 : 1;
}
