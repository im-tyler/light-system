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

// Async page reader backed by a worker thread. The preferred backend is a
// read-only mmap of the .vgeo file: each job copies its page's byte range
// out of the mapping (page faults perform the disk I/O off the main
// thread) and the completion carries the bytes so the caller can upload
// them into the GPU payload buffer. If mmap fails, the worker falls back
// to pread() on the same fd with identical completion semantics.
struct AsyncReadJob {
    uint64_t offset = 0;
    std::size_t size = 0;
    uint32_t page_index = 0xffffffffu;
};

struct AsyncReadCompletion {
    uint32_t page_index = 0xffffffffu;
    bool success = false;
    std::vector<std::byte> data;
};

class AsyncReader {
public:
    AsyncReader();
    ~AsyncReader();

    AsyncReader(const AsyncReader&) = delete;
    AsyncReader& operator=(const AsyncReader&) = delete;

    // Opens `path`, mmaps it when possible, and spawns a worker thread.
    // Returns false if the file could not be opened (caller should fall
    // back to the simulated-latency path). An mmap failure alone keeps the
    // reader usable via the pread backend.
    bool open(const std::filesystem::path& path);
    void close();

    bool is_open() const { return fd_ >= 0; }
    bool mmap_active() const { return mapped_ != nullptr; }

    // Thread-safe: enqueues a read job. Multiple jobs are processed in
    // FIFO order by the worker thread.
    void submit(const AsyncReadJob& job);

    // Thread-safe: returns every completion that has landed since the
    // last call, and clears the internal buffer.
    std::vector<AsyncReadCompletion> drain_completions();

    // Synchronous read on the calling thread (startup seed uploads).
    // Reads from the mapping when active, otherwise pread()s the fd.
    bool read_sync(uint64_t offset, std::size_t size, void* dst) const;

    // Diagnostics.
    std::size_t pending_count() const;
    std::uint64_t completed_count() const { return completed_count_.load(std::memory_order_relaxed); }

private:
    void worker_loop();

    int fd_ = -1;
    void* mapped_ = nullptr;
    std::uint64_t file_size_ = 0;
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
