
#include "task_scheduler.hpp"

TaskScheduler::TaskScheduler(std::size_t numWorkers) {
    if (numWorkers == 0)
        numWorkers = 1;

    try {
        for (std::size_t i = 0; i < numWorkers; ++i) {
            workers.emplace_back([this]() {
                workerLoop();
            });
        }

        schedulerThread = std::thread([this]() {
            schedulerLoop();
        });
    } catch (...) {
        {
            std::lock_guard<std::mutex> lock(readyMutex);
            workersStopping = true;
        }

        readyCV.notify_all();

        for (auto& worker : workers) {
            if (worker.joinable())
                worker.join();
        }

        throw;
    }
}

void TaskScheduler::addTask(
    Clock::time_point due,
    int priority,
    TaskItem item
) {
    {
        std::lock_guard<std::mutex> lock(scheduleMutex);

        if (stopping) {
            throw std::runtime_error(
                "TaskScheduler is stopping"
            );
        }

        item.due = due;
        item.priority = priority;
        item.sequence = nextSequence++;

        scheduled.push(std::move(item));
    }

    scheduleCV.notify_one();
}

void TaskScheduler::dispatch(TaskItem item) {
    {
        std::lock_guard<std::mutex> lock(readyMutex);
        ready.push(std::move(item));
    }

    readyCV.notify_one();
}

void TaskScheduler::workerLoop() {
    while (true) {
        TaskItem item;

        {
            std::unique_lock<std::mutex> lock(readyMutex);

            readyCV.wait(lock, [this]() {
                return workersStopping || !ready.empty();
            });

            if (workersStopping && ready.empty())
                return;

            item = std::move(ready.front());
            ready.pop();
        }

        // execute() atomically claims the Pending state.
        // A cancelled task cannot begin executing.
        item.execute();
    }
}

void TaskScheduler::schedulerLoop() {
    std::unique_lock<std::mutex> lock(scheduleMutex);

    while (true) {
        if (stopping) {
            while (!scheduled.empty()) {
                TaskItem item = scheduled.top();
                scheduled.pop();

                if (shutdownMode == ShutdownMode::CancelPending) {
                    item.state->cancel();
                    continue;
                }

                if (item.state->status.load(
                        std::memory_order_acquire)
                    == TaskStatus::Pending) {
                    lock.unlock();
                    dispatch(std::move(item));
                    lock.lock();
                }
            }

            return;
        }

        if (scheduled.empty()) {
            scheduleCV.wait(lock, [this]() {
                return stopping || !scheduled.empty();
            });

            continue;
        }

        const auto due = scheduled.top().due;

        if (Clock::now() < due) {
            scheduleCV.wait_until(lock, due);
            continue;
        }

        TaskItem item = scheduled.top();
        scheduled.pop();

        if (item.state->status.load(
                std::memory_order_acquire)
            != TaskStatus::Pending) {
            continue;
        }

        lock.unlock();
        dispatch(std::move(item));
        lock.lock();
    }
}

void TaskScheduler::shutdown(ShutdownMode mode) {
    {
        std::lock_guard<std::mutex> lock(scheduleMutex);

        if (!stopping) {
            stopping = true;
            shutdownMode = mode;
        }
    }

    scheduleCV.notify_all();

    if (schedulerThread.joinable())
        schedulerThread.join();

    {
        std::lock_guard<std::mutex> lock(readyMutex);

        if (shutdownMode == ShutdownMode::CancelPending) {
            while (!ready.empty()) {
                ready.front().state->cancel();
                ready.pop();
            }
        }

        workersStopping = true;
    }

    readyCV.notify_all();

    for (auto& worker : workers) {
        if (worker.joinable())
            worker.join();
    }
}

TaskScheduler::~TaskScheduler() {
    shutdown();
}

