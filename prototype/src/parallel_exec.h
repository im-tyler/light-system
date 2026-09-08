#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace meridian {

// Minimal fork-join executor for deterministic parallel work. There is no
// work stealing and no result ordering: callers partition work into jobs and
// merge results in a fixed order themselves, so output never depends on
// scheduling. run() is reentrant from inside a job -- every thread blocked
// in run() keeps executing queued jobs, so nested fork-join batches cannot
// deadlock. All jobs must be exception-free.
class ParallelExecutor {
public:
    // total_threads includes the submitting thread: the executor owns
    // total_threads - 1 persistent workers.
    explicit ParallelExecutor(unsigned int total_threads)
        : total_threads_(total_threads < 1 ? 1 : total_threads) {
        workers_.reserve(total_threads_ - 1);
        for (unsigned int i = 0; i + 1 < total_threads_; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ParallelExecutor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        for (std::thread& worker : workers_) {
            worker.join();
        }
    }

    ParallelExecutor(const ParallelExecutor&) = delete;
    ParallelExecutor& operator=(const ParallelExecutor&) = delete;

    unsigned int total_threads() const { return total_threads_; }

    // Run jobs (in any order, possibly concurrently) and return when all
    // have completed. The calling thread participates by draining the queue
    // while it waits.
    void run(const std::vector<std::function<void()>>& jobs) {
        if (jobs.empty()) {
            return;
        }
        if (total_threads_ == 1 || jobs.size() == 1) {
            for (const std::function<void()>& job : jobs) {
                job();
            }
            return;
        }
        std::atomic<std::size_t> remaining(jobs.size());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const std::function<void()>& job : jobs) {
                queue_.push_back(QueueEntry{job, &remaining});
            }
        }
        wake_.notify_all();
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (remaining.load(std::memory_order_acquire) == 0) {
                return;
            }
            if (!queue_.empty()) {
                const QueueEntry entry = queue_.front();
                queue_.pop_front();
                lock.unlock();
                run_entry(entry);
                lock.lock();
            } else {
                done_.wait(lock, [&] {
                    return remaining.load(std::memory_order_acquire) == 0 || !queue_.empty();
                });
            }
        }
    }

private:
    struct QueueEntry {
        std::function<void()> job;
        std::atomic<std::size_t>* remaining;
    };

    void run_entry(const QueueEntry& entry) {
        entry.job();
        // Notify under the mutex: a waiter checking the predicate must not
        // be able to sleep between our decrement and its notify.
        if (entry.remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lock(mutex_);
            done_.notify_all();
        }
    }

    void worker_loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (!queue_.empty()) {
                const QueueEntry entry = queue_.front();
                queue_.pop_front();
                lock.unlock();
                run_entry(entry);
                lock.lock();
                continue;
            }
            if (stopping_) {
                return;
            }
            wake_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
        }
    }

    unsigned int total_threads_;
    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable done_;
    std::deque<QueueEntry> queue_;
    bool stopping_ = false;
};

}  // namespace meridian
