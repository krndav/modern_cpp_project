
#ifndef TASK_SCHEDULER_HPP
#define TASK_SCHEDULER_HPP

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class TaskCancelled : public std::runtime_error {
public:
    TaskCancelled()
        : std::runtime_error("Task was cancelled") {}
};

enum class ShutdownMode {
    Drain,
    CancelPending
};

enum class TaskStatus {
    Pending,
    Running,
    Completed,
    Cancelled
};

struct TaskState {
    std::atomic<TaskStatus> status{TaskStatus::Pending};
    std::function<void()> reportCancelled;

    bool cancel() {
        TaskStatus expected = TaskStatus::Pending;

        if (!status.compare_exchange_strong(
                expected,
                TaskStatus::Cancelled,
                std::memory_order_acq_rel)) {
            return false;
        }

        reportCancelled();
        return true;
    }
};

class TaskHandle {
private:
    std::shared_ptr<TaskState> state;

    explicit TaskHandle(std::shared_ptr<TaskState> s)
        : state(std::move(s)) {}

    friend class TaskScheduler;

public:
    TaskHandle() = default;

    bool cancel() const {
        return state && state->cancel();
    }

    bool is_cancelled() const {
        return state &&
            state->status.load(std::memory_order_acquire)
                == TaskStatus::Cancelled;
    }

    TaskStatus status() const {
        if (!state)
            throw std::logic_error("Empty task handle");

        return state->status.load(std::memory_order_acquire);
    }
};

template <typename R>
struct ScheduledTask {
    std::future<R> result;
    TaskHandle handle;
};

class TaskScheduler {
private:
    using Clock = std::chrono::steady_clock;

    struct TaskItem {
        Clock::time_point due;
        int priority;
        std::uint64_t sequence;
        std::shared_ptr<TaskState> state;
        std::function<void()> execute;
    };

    struct Compare {
        bool operator()(
            const TaskItem& a,
            const TaskItem& b) const {

            if (a.due != b.due)
                return a.due > b.due;

            if (a.priority != b.priority)
                return a.priority < b.priority;

            return a.sequence > b.sequence;
        }
    };

    std::priority_queue<
        TaskItem, std::vector<TaskItem>, Compare
    > scheduled;

    std::mutex scheduleMutex;
    std::condition_variable scheduleCV;

    std::queue<TaskItem> ready;
    std::mutex readyMutex;
    std::condition_variable readyCV;

    std::vector<std::thread> workers;
    std::thread schedulerThread;

    bool stopping = false;
    bool workersStopping = false;
    ShutdownMode shutdownMode = ShutdownMode::Drain;
    std::uint64_t nextSequence = 0;

    void workerLoop();
    void schedulerLoop();
    void dispatch(TaskItem item);

    void addTask(
        Clock::time_point due,
        int priority,
        TaskItem item
    );

public:
    explicit TaskScheduler(
        std::size_t numWorkers = std::thread::hardware_concurrency()
    );

    TaskScheduler(const TaskScheduler&) = delete;
    TaskScheduler& operator=(const TaskScheduler&) = delete;

    template <typename Rep, typename Period,
              typename F, typename... Args>
    auto schedule_after(
        std::chrono::duration<Rep, Period> delay,
        int priority,
        F&& f,
        Args&&... args
    ) -> ScheduledTask<
        std::invoke_result_t<
            std::decay_t<F>, std::decay_t<Args>...
        >
    > {
        using Result = std::invoke_result_t<
            std::decay_t<F>, std::decay_t<Args>...
        >;

        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();

        auto state = std::make_shared<TaskState>();

        state->reportCancelled = [promise]() {
            try {
                throw TaskCancelled();
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
        };

        // std::bind stores the callable and its arguments.
        auto bound = std::bind(
            std::forward<F>(f),
            std::forward<Args>(args)...
        );

        auto callable =
            std::make_shared<decltype(bound)>(std::move(bound));

        TaskItem item{
            Clock::time_point{},
            priority,
            0,
            state,
            [state, promise, callable]() mutable {
                TaskStatus expected = TaskStatus::Pending;

                if (!state->status.compare_exchange_strong(
                        expected,
                        TaskStatus::Running,
                        std::memory_order_acq_rel)) {
                    return;
                }

                try {
                    if constexpr (std::is_void_v<Result>) {
                        (*callable)();
                        promise->set_value();
                    } else {
                        promise->set_value((*callable)());
                    }

                    state->status.store(
                        TaskStatus::Completed,
                        std::memory_order_release
                    );
                } catch (...) {
                    promise->set_exception(
                        std::current_exception()
                    );

                    state->status.store(
                        TaskStatus::Completed,
                        std::memory_order_release
                    );
                }
            }
        };

        auto nonNegativeDelay =
            delay < delay.zero() ? delay.zero() : delay;

        auto convertedDelay =
            std::chrono::duration_cast<Clock::duration>(
                nonNegativeDelay
            );

        addTask(
            Clock::now() + convertedDelay,
            priority,
            std::move(item)
        );

        return ScheduledTask<Result>{
            std::move(future),
            TaskHandle(state)
        };
    }

    template <typename F, typename... Args>
    auto submit(F&& f, Args&&... args) {
        return schedule_after(
            Clock::duration::zero(),
            0,
            std::forward<F>(f),
            std::forward<Args>(args)...
        );
    }

    void shutdown(
        ShutdownMode mode = ShutdownMode::Drain
    );

    ~TaskScheduler();
};

#endif

