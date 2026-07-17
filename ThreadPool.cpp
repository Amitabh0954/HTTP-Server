#include "ThreadPool.hpp"

ThreadPool::ThreadPool(size_t numThreads, std::function<void(int)> handler)
    : handler_(std::move(handler)) {
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all(); // wake every worker so they can all observe stop_ and exit
    for (auto& t : workers_) {
        t.join();
    }
}

void ThreadPool::submit(int client_fd) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.push(client_fd);
    }
    // notify_one, not notify_all: exactly one fd was added, so at most one
    // sleeping worker needs to wake up to claim it. Waking every worker to
    // fight over a single item would just waste cycles on the ones that lose.
    cv_.notify_one();
}

void ThreadPool::workerLoop() {
    for (;;) {
        int client_fd;
        {
            std::unique_lock<std::mutex> lock(mutex_);

            // The predicate form of wait() re-checks the condition after
            // every wakeup. That also protects against "spurious wakeups":
            // the standard explicitly permits condition_variable::wait() to
            // return even when nobody called notify() -- so we can never
            // just trust a wakeup, only the state we observe after it.
            cv_.wait(lock, [this] { return stop_ || !pending_.empty(); });

            if (stop_ && pending_.empty()) {
                return;
            }

            client_fd = pending_.front();
            pending_.pop();
        }
        // Call the handler OUTSIDE the lock. Holding mutex_ during a
        // (potentially slow) request would serialize every worker behind
        // whichever one is currently handling a client -- defeating the
        // entire point of having a pool of them.
        handler_(client_fd);
    }
}
