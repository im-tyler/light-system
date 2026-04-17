#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace meridian {

// Async page reader backed by a worker thread performing pread() on a
// file descriptor. Jobs carry a page_index key so the main thread can
// match completions back to residency state without touching the raw
// bytes (the scene payload is already uploaded to the GPU; reads here
// are about exercising real I/O latency for the streaming simulation).
struct AsyncReadJob {
    uint64_t offset = 0;
    std::size_t size = 0;
    uint32_t page_index = 0xffffffffu;
};

struct AsyncReadCompletion {
    uint32_t page_index = 0xffffffffu;
    bool success = false;
};

class AsyncReader {
public:
    AsyncReader();
    ~AsyncReader();

    AsyncReader(const AsyncReader&) = delete;
    AsyncReader& operator=(const AsyncReader&) = delete;

    // Opens `path` for reading and spawns a worker thread. Returns false
    // if the file could not be opened (caller should fall back to the
    // simulated-latency path).
    bool open(const std::filesystem::path& path);
    void close();

    bool is_open() const { return fd_ >= 0; }

    // Thread-safe: enqueues a read job. Multiple jobs are processed in
    // FIFO order by the worker thread.
    void submit(const AsyncReadJob& job);

    // Thread-safe: returns every completion that has landed since the
    // last call, and clears the internal buffer.
    std::vector<AsyncReadCompletion> drain_completions();

    // Diagnostics.
    std::size_t pending_count() const;
    std::uint64_t completed_count() const { return completed_count_.load(std::memory_order_relaxed); }

private:
    void worker_loop();

    int fd_ = -1;
    std::atomic<bool> stop_{false};
    std::atomic<std::uint64_t> completed_count_{0};
    std::thread worker_;

    mutable std::mutex queue_mutex_;
    std::condition_variable cv_;
    std::queue<AsyncReadJob> pending_;

    mutable std::mutex completion_mutex_;
    std::vector<AsyncReadCompletion> completions_;
};

}  // namespace meridian
