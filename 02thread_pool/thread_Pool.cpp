
#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <functional>
#include <future>
#include <stdexcept>

class ThreadPool {
private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex mtx;
    std::condition_variable cv;
    bool stopping = false;

public:
    explicit ThreadPool(size_t numThreads) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back([this]() {
                while (true) {
                    std::function<void()> task;

                    {
                        std::unique_lock<std::mutex> lock(mtx);

                        cv.wait(lock, [this]() {
                            return stopping || !tasks.empty();
                        });

                        if (stopping && tasks.empty())
                            return;

                        task = std::move(tasks.front());
                        tasks.pop();
                    }

                    task();
                }
            });
        }
    }

    // Prevent copying
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <typename F, typename... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using ReturnType = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<ReturnType()>>(
            [fn = std::forward<F>(f),
             ...args = std::forward<Args>(args)]() mutable {
                return std::invoke(
                    std::move(fn), std::move(args)...
                );
            }
        );

        std::future<ReturnType> result = task->get_future();

        {
            std::lock_guard<std::mutex> lock(mtx);

            if (stopping)
                throw std::runtime_error("ThreadPool is stopping");

            tasks.emplace([task]() {
                (*task)();
            });
        }

        cv.notify_one();
        return result;
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            stopping = true;
        }

        cv.notify_all();

        for (auto& worker : workers) {
            if (worker.joinable())
                worker.join();
        }
    }
};

int main() {
    ThreadPool pool(4);

    auto f1 = pool.enqueue([](int a, int b) {
        return a + b;
    }, 10, 20);

    auto f2 = pool.enqueue([]() {
        std::cout << "Task running on thread "
                  << std::this_thread::get_id() << '\n';
        return 100;
    });

    std::cout << "Sum: " << f1.get() << '\n';
    std::cout << "Result: " << f2.get() << '\n';

    return 0;
}

