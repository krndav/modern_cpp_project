#pragma once
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// C++17 educational work-stealing pool. Mutex-protected, not lock-free.
class WorkStealingPool {
public:
    explicit WorkStealingPool(std::size_t count = std::thread::hardware_concurrency());
    ~WorkStealingPool();
    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    template<class F, class... Args>
    auto submit(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using R = std::invoke_result_t<F, Args...>;
        auto bound = std::bind(std::forward<F>(f), std::forward<Args>(args)...);
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(bound));
        auto future = task->get_future();
        {
            std::lock_guard<std::mutex> submit_lock(submit_mutex_);
            if (stopping_.load(std::memory_order_acquire))
                throw std::runtime_error("submit after shutdown");
            auto index = next_.fetch_add(1, std::memory_order_relaxed) % workers_.size();
            Worker& w = *workers_[index];
            std::lock_guard<std::mutex> queue_lock(w.mutex);
            w.tasks.emplace_back([task] { (*task)(); });
            queued_.fetch_add(1, std::memory_order_release);
        }
        cv_.notify_one();
        return future;
    }

    void shutdown();
    std::size_t threadCount() const noexcept;

private:
    struct Worker {
        std::mutex mutex;
        std::deque<std::function<void()>> tasks;
        std::thread thread;
    };
    bool takeTask(std::size_t index, std::function<void()>& task);
    void run(std::size_t index);

    std::vector<std::unique_ptr<Worker>> workers_;
    std::atomic<std::size_t> next_{0}, queued_{0};
    std::atomic<bool> stopping_{false};
    std::mutex submit_mutex_, wait_mutex_;
    std::condition_variable cv_;
};
