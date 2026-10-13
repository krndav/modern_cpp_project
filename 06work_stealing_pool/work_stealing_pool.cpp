#include "work_stealing_pool.hpp"

WorkStealingPool::WorkStealingPool(std::size_t numThreads) {
    if (numThreads == 0)
        numThreads = 1;

    workers.reserve(numThreads);
    for (std::size_t i = 0; i < numThreads; ++i)
        workers.push_back(std::make_unique<Worker>());

    try {
        for (std::size_t i = 0; i < numThreads; ++i) {
            workers[i]->thread = std::thread([this, i]() {
                workerLoop(i);
            });
        }
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(waitMutex);
            stopping.store(true, std::memory_order_release);
        }
        wakeCV.notify_all();

        for (auto& worker : workers)
            if (worker->thread.joinable())
                worker->thread.join();

        throw;
    }
}

void WorkStealingPool::enqueueTask(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);

        if (stopping.load(std::memory_order_acquire))
            throw std::runtime_error("WorkStealingPool is stopping");

        const std::size_t index =
            nextQueue.fetch_add(1, std::memory_order_relaxed)
            % workers.size();

        {
            std::lock_guard<std::mutex> queueLock(
                workers[index]->queueMutex);

            workers[index]->tasks.push_back(std::move(task));

            std::lock_guard<std::mutex> waitLock(waitMutex);
            pendingTasks.fetch_add(1, std::memory_order_release);
        }
    }

    wakeCV.notify_all();
}

bool WorkStealingPool::tryGetTask(
    std::size_t workerIndex,
    std::function<void()>& task) {

    {
        std::lock_guard<std::mutex> lock(
            workers[workerIndex]->queueMutex);

        auto& tasks = workers[workerIndex]->tasks;
        if (!tasks.empty()) {
            task = std::move(tasks.back());
            tasks.pop_back();

            std::lock_guard<std::mutex> waitLock(waitMutex);
            pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    }

    const std::size_t count = workers.size();

    for (std::size_t offset = 1; offset < count; ++offset) {
        const std::size_t victim = (workerIndex + offset) % count;

        std::lock_guard<std::mutex> lock(
            workers[victim]->queueMutex);

        auto& tasks = workers[victim]->tasks;
        if (!tasks.empty()) {
            task = std::move(tasks.front());
            tasks.pop_front();

            std::lock_guard<std::mutex> waitLock(waitMutex);
            pendingTasks.fetch_sub(1, std::memory_order_acq_rel);
            return true;
        }
    }

    return false;
}

void WorkStealingPool::workerLoop(std::size_t workerIndex) {
    while (true) {
        std::function<void()> task;

        if (tryGetTask(workerIndex, task)) {
            task();
            continue;
        }

        std::unique_lock<std::mutex> lock(waitMutex);
        wakeCV.wait(lock, [this]() {
            return stopping.load(std::memory_order_acquire) ||
                   pendingTasks.load(std::memory_order_acquire) > 0;
        });

        if (stopping.load(std::memory_order_acquire) &&
            pendingTasks.load(std::memory_order_acquire) == 0)
            return;
    }
}

void WorkStealingPool::shutdown() {
    {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
        std::lock_guard<std::mutex> waitLock(waitMutex);
        stopping.store(true, std::memory_order_release);
    }

    wakeCV.notify_all();

    for (auto& worker : workers)
        if (worker->thread.joinable())
            worker->thread.join();
}

WorkStealingPool::~WorkStealingPool() {
    shutdown();
}
