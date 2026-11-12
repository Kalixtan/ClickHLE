#include "worker.h"
#include <stdexcept>
#include <utility>

namespace clickhle::vc2 {
Worker::Worker() : thread_([this] { run(); }) {}

Worker::~Worker() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    ready_.notify_one();
    thread_.join();
}

std::future<void> Worker::submit(std::function<void()> job) {
    std::packaged_task<void()> task(std::move(job));
    auto result = task.get_future();
    {
        std::lock_guard lock(mutex_);
        if (stopping_) throw std::runtime_error("VC2 worker is stopping");
        jobs_.push_back(std::move(task));
    }
    ready_.notify_one();
    return result;
}

void Worker::run() {
    for (;;) {
        std::packaged_task<void()> task;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (jobs_.empty()) return;
            task = std::move(jobs_.front());
            jobs_.pop_front();
        }
        task();
    }
}
} // namespace clickhle::vc2
