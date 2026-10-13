
#include "task_scheduler.hpp"

#include <iostream>
#include <string>
#include <chrono>

using namespace std::chrono_literals;

int main() {
    // Test 1: Normal execution and cancellation.
    {
        TaskScheduler scheduler(4);

        auto first = scheduler.submit([] {
            return 42;
        });

        auto second = scheduler.submit([](int x) {
            return x * x;
        }, 5);

        auto delayed = scheduler.schedule_after(
            30s, 0, [] {
                return std::string("Should not execute");
            }
        );

        std::cout << "First result: "
                  << first.result.get() << '\n';

        std::cout << "Second result: "
                  << second.result.get() << '\n';

        if (delayed.handle.cancel()) {
            std::cout << "Delayed task cancelled\n";
        }

        try {
            delayed.result.get();
        } catch (const TaskCancelled& e) {
            std::cout << "Future reports: "
                      << e.what() << '\n';
        }

        scheduler.shutdown(ShutdownMode::Drain);
    }

    // Test 2: Drain shutdown runs pending tasks immediately.
    {
        TaskScheduler scheduler(2);

        auto task = scheduler.schedule_after(
            1h, 0, [] {
                return 100;
            }
        );

        scheduler.shutdown(ShutdownMode::Drain);

        std::cout << "Drain result: "
                  << task.result.get() << '\n';
    }

    // Test 3: CancelPending cancels tasks that have not started.
    {
        TaskScheduler scheduler(2);

        auto task = scheduler.schedule_after(
            1h, 0, [] {
                return 200;
            }
        );

        scheduler.shutdown(ShutdownMode::CancelPending);

        try {
            std::cout << task.result.get() << '\n';
        } catch (const TaskCancelled& e) {
            std::cout << "Shutdown cancellation: "
                      << e.what() << '\n';
        }
    }

    std::cout << "All tests completed\n";
    return 0;
}

