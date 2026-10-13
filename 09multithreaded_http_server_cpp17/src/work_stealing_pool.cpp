#include "work_stealing_pool.hpp"
#include <chrono>

WorkStealingPool::WorkStealingPool(std::size_t count) {
    if (count == 0) count = 1;
    workers_.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
        workers_.push_back(std::make_unique<Worker>());
    for (std::size_t i = 0; i < count; ++i)
        workers_[i]->thread = std::thread([this, i] { run(i); });
}
WorkStealingPool::~WorkStealingPool() { shutdown(); }
std::size_t WorkStealingPool::threadCount() const noexcept { return workers_.size(); }

void WorkStealingPool::shutdown() {
    {
        std::lock_guard<std::mutex> lock(submit_mutex_);
        stopping_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    for (auto& w : workers_)
        if (w->thread.joinable()) w->thread.join();
}

bool WorkStealingPool::takeTask(std::size_t index, std::function<void()>& task) {
    {
        Worker& w = *workers_[index];
        std::lock_guard<std::mutex> lock(w.mutex);
        if (!w.tasks.empty()) {
            task = std::move(w.tasks.back());
            w.tasks.pop_back();
            queued_.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    }
    for (std::size_t n = 1; n < workers_.size(); ++n) {
        Worker& victim = *workers_[(index + n) % workers_.size()];
        std::lock_guard<std::mutex> lock(victim.mutex);
        if (!victim.tasks.empty()) {
            task = std::move(victim.tasks.front());
            victim.tasks.pop_front();
            queued_.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    }
    return false;
}
void WorkStealingPool::run(std::size_t index) {
    for (;;) {
        std::function<void()> task;
        if (takeTask(index, task)) {
            task();
            continue;
        }
        if (stopping_.load(std::memory_order_acquire) &&
            queued_.load(std::memory_order_acquire) == 0) return;
        std::unique_lock<std::mutex> lock(wait_mutex_);
        cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
            return stopping_.load(std::memory_order_acquire) ||
                   queued_.load(std::memory_order_acquire) != 0;
        });
    }
}
