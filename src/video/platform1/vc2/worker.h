#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>
#include <thread>

namespace clickhle::vc2 {

// FIFO execution for coprocessor work. The caller owns synchronization with
// guest-visible state; a completed future means the job has stopped touching it.
class Worker {
  public:
    Worker();
    ~Worker();
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    std::future<void> submit(std::function<void()> job);

  private:
    void run();
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<std::packaged_task<void()>> jobs_;
    bool stopping_ = false;
    std::thread thread_;
};
} // namespace clickhle::vc2
