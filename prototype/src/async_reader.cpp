#include "async_reader.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace meridian {

AsyncReader::AsyncReader() = default;

AsyncReader::~AsyncReader() { close(); }

bool AsyncReader::open(const std::filesystem::path& path) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;
    struct stat st {};
    if (::fstat(fd_, &st) != 0 || st.st_size <= 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    file_size_ = static_cast<std::uint64_t>(st.st_size);
    mapped_ = ::mmap(nullptr, static_cast<std::size_t>(file_size_), PROT_READ, MAP_PRIVATE, fd_, 0);
    if (mapped_ == MAP_FAILED) {
        mapped_ = nullptr;  // pread backend still works
    }
    stop_.store(false, std::memory_order_relaxed);
    worker_ = std::thread([this] { worker_loop(); });
    return true;
}

void AsyncReader::close() {
    if (worker_.joinable()) {
        {
            std::lock_guard<std::mutex> lk(queue_mutex_);
            stop_.store(true, std::memory_order_relaxed);
        }
        cv_.notify_all();
        worker_.join();
    }
    if (mapped_ != nullptr) {
        ::munmap(mapped_, static_cast<std::size_t>(file_size_));
        mapped_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    file_size_ = 0;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        std::queue<AsyncReadJob>().swap(pending_);
    }
    {
        std::lock_guard<std::mutex> lk(completion_mutex_);
        completions_.clear();
    }
    completed_count_.store(0, std::memory_order_relaxed);
    stop_.store(false, std::memory_order_relaxed);
}

void AsyncReader::submit(const AsyncReadJob& job) {
    if (fd_ < 0) return;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        pending_.push(job);
    }
    if (mapped_ != nullptr && job.offset + job.size <= file_size_) {
        ::madvise(static_cast<char*>(mapped_) + job.offset, job.size, MADV_WILLNEED);
    }
    cv_.notify_one();
}

std::vector<AsyncReadCompletion> AsyncReader::drain_completions() {
    std::vector<AsyncReadCompletion> out;
    std::lock_guard<std::mutex> lk(completion_mutex_);
    out.swap(completions_);
    return out;
}

bool AsyncReader::read_sync(uint64_t offset, std::size_t size, void* dst) const {
    if (fd_ < 0 || size == 0 || offset + size > file_size_) return false;
    if (mapped_ != nullptr) {
        std::memcpy(dst, static_cast<const char*>(mapped_) + offset, size);
        return true;
    }
    auto* out = static_cast<char*>(dst);
    std::size_t total = 0;
    while (total < size) {
        const ssize_t got = ::pread(fd_, out + total, size - total,
                                    static_cast<off_t>(offset + total));
        if (got <= 0) return false;
        total += static_cast<std::size_t>(got);
    }
    return true;
}

std::size_t AsyncReader::pending_count() const {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    return pending_.size();
}

void AsyncReader::worker_loop() {
    while (true) {
        AsyncReadJob job;
        {
            std::unique_lock<std::mutex> lk(queue_mutex_);
            cv_.wait(lk, [this] {
                return stop_.load(std::memory_order_relaxed) || !pending_.empty();
            });
            if (pending_.empty()) {
                if (stop_.load(std::memory_order_relaxed)) return;
                continue;
            }
            job = pending_.front();
            pending_.pop();
        }

        AsyncReadCompletion completion{};
        completion.page_index = job.page_index;
        completion.data.resize(job.size);
        if (mapped_ != nullptr && job.offset + job.size <= file_size_) {
            std::memcpy(completion.data.data(), static_cast<const char*>(mapped_) + job.offset,
                        job.size);
            completion.success = true;
        } else if (fd_ >= 0) {
            std::size_t total = 0;
            while (total < job.size) {
                const ssize_t got = ::pread(fd_, completion.data.data() + total,
                                            job.size - total,
                                            static_cast<off_t>(job.offset + total));
                if (got <= 0) break;
                total += static_cast<std::size_t>(got);
            }
            completion.success = (total == job.size);
        }
        if (!completion.success) {
            completion.data.clear();
        }
        {
            std::lock_guard<std::mutex> lk(completion_mutex_);
            completions_.push_back(std::move(completion));
        }
        completed_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace meridian
