#include "async_reader.h"

#include <fcntl.h>
#include <unistd.h>

namespace meridian {

AsyncReader::AsyncReader() = default;

AsyncReader::~AsyncReader() { close(); }

bool AsyncReader::open(const std::filesystem::path& path) {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;
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
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
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
    cv_.notify_one();
}

std::vector<AsyncReadCompletion> AsyncReader::drain_completions() {
    std::vector<AsyncReadCompletion> out;
    std::lock_guard<std::mutex> lk(completion_mutex_);
    out.swap(completions_);
    return out;
}

std::size_t AsyncReader::pending_count() const {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    return pending_.size();
}

void AsyncReader::worker_loop() {
    std::vector<char> scratch;
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

        if (scratch.size() < job.size) scratch.resize(job.size);
        ssize_t got = 0;
        std::size_t total = 0;
        while (total < job.size) {
            got = ::pread(fd_, scratch.data() + total, job.size - total,
                          static_cast<off_t>(job.offset + total));
            if (got <= 0) break;
            total += static_cast<std::size_t>(got);
        }
        AsyncReadCompletion completion{};
        completion.page_index = job.page_index;
        completion.success = (total == job.size);
        {
            std::lock_guard<std::mutex> lk(completion_mutex_);
            completions_.push_back(completion);
        }
        completed_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

}  // namespace meridian
