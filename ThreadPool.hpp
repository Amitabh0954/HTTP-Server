#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// A fixed-size pool of worker threads that pull client file descriptors off
// a shared queue and hand each one to a caller-supplied handler function.
//
// Why a queue of plain int (not a generic queue of std::function tasks):
// std::function in C++17 requires its target callable to be
// copy-constructible. Our Socket is move-only (copying would risk two
// objects both closing the same fd), so a lambda capturing a Socket by
// value could never be stored in a std::function in the first place.
// Passing the raw fd sidesteps the problem: each worker wraps it in its
// own Socket only after dequeuing it, on its own thread.
class ThreadPool {
public:
    ThreadPool(size_t numThreads, std::function<void(int)> handler);

    // Signals all workers to stop once the queue drains, and joins them.
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Hands ownership of client_fd to whichever worker thread picks it up
    // next. Safe to call from the accepting thread while workers are running.
    void submit(int client_fd);

private:
    void workerLoop();

    std::function<void(int)> handler_;
    std::vector<std::thread> workers_;

    std::queue<int> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
};
