#ifndef WORK_STEALING_POOL_HPP
#define WORK_STEALING_POOL_HPP

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
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

class WorkStealingPool {
private:
    struct Worker {
        std::mutex queueMutex;
        std::deque<std::function<void()>> tasks;
        std::thread thread;
    };

    std::vector<std::unique_ptr<Worker>> workers;
    std::atomic<std::size_t> pendingTasks{0};
    std::atomic<std::size_t> nextQueue{0};
    std::atomic<bool> stopping{false};

    std::mutex lifecycleMutex;
    std::mutex waitMutex;
    std::condition_variable wakeCV;

    bool tryGetTask(std::size_t workerIndex,
                    std::function<void()>& task);
    void workerLoop(std::size_t workerIndex);
    void enqueueTask(std::function<void()> task);

public:
    explicit WorkStealingPool(
        std::size_t numThreads = std::thread::hardware_concurrency());

    WorkStealingPool(const WorkStealingPool&) = delete;
    WorkStealingPool& operator=(const WorkStealingPool&) = delete;

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<
            std::decay_t<F>, std::decay_t<Args>...
        >>
    {
        using Result = std::invoke_result_t<
            std::decay_t<F>, std::decay_t<Args>...
        >;

        auto callable = [
            fn = std::forward<F>(f),
            values = std::make_tuple(std::forward<Args>(args)...)
        ]() mutable -> Result {
            return std::apply(
                [&fn = fn](auto&&... unpacked) -> Result {
                    return std::invoke(
                        std::move(fn),
                        std::forward<decltype(unpacked)>(unpacked)...
                    );
                },
                std::move(values)
            );
        };

        auto packaged =
            std::make_shared<std::packaged_task<Result()>>(
                std::move(callable));

        auto result = packaged->get_future();

        enqueueTask([packaged]() {
            (*packaged)();
        });

        return result;
    }

    std::size_t threadCount() const noexcept {
        return workers.size();
    }

    void shutdown();
    ~WorkStealingPool();
};

#endif
