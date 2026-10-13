#include "work_stealing_pool.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std::chrono_literals;

int main() {
    WorkStealingPool pool(4);

    auto sum = pool.submit([](int a, int b) {
        return a + b;
    }, 10, 20);

    std::cout << "Sum: " << sum.get() << '\n';

    auto message = pool.submit([] {
        return std::string("Hello from a worker!");
    });
    std::cout << message.get() << '\n';

    std::vector<std::future<long long>> results;
    for (int i = 1; i <= 100; ++i) {
        results.push_back(pool.submit([i] {
            std::this_thread::sleep_for(1ms);
            return 1LL * i * i;
        }));
    }

    long long total = 0;
    for (auto& result : results)
        total += result.get();

    std::cout << "Sum of squares: " << total << '\n';

    auto failing = pool.submit([]() -> int {
        throw std::runtime_error("Example task failure");
    });

    try {
        (void)failing.get();
    } catch (const std::exception& e) {
        std::cout << "Caught exception: " << e.what() << '\n';
    }

    pool.shutdown();
    std::cout << "Pool stopped successfully\n";
    return 0;
}
