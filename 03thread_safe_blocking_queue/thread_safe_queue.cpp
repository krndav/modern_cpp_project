
#include <iostream>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <vector>
#include <stdexcept>
#include <cstddef>
#include <utility>

template <typename T>
class BlockingQueue {
private:
    std::queue<T> queue;
    const std::size_t capacity;

    mutable std::mutex mtx;
    std::condition_variable not_full;
    std::condition_variable not_empty;

    bool closed = false;

public:
    explicit BlockingQueue(std::size_t capacity)
        : capacity(capacity)
    {
        if (capacity == 0) {
            throw std::invalid_argument(
                "Queue capacity must be greater than zero"
            );
        }
    }

    // Prevent copying: mutexes and condition variables
    // should not be copied.
    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    void push(T value) {
        {
            std::unique_lock<std::mutex> lock(mtx);

            not_full.wait(lock, [this]() {
                return closed || queue.size() < capacity;
            });

            if (closed) {
                throw std::runtime_error("Queue is closed");
            }

            queue.push(std::move(value));
        }

        // Notify one waiting consumer.
        not_empty.notify_one();
    }

    bool pop(T& result) {
        {
            std::unique_lock<std::mutex> lock(mtx);

            not_empty.wait(lock, [this]() {
                return closed || !queue.empty();
            });

            // Closed and no items remain.
            if (queue.empty()) {
                return false;
            }

            result = std::move(queue.front());
            queue.pop();
        }

        // Notify one waiting producer.
        not_full.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lock(mtx);
            closed = true;
        }

        // Wake producers and consumers.
        not_full.notify_all();
        not_empty.notify_all();
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(mtx);
        return queue.size();
    }
};

int main() {
    BlockingQueue<int> queue(3);

    // Producer
    std::thread producer([&queue]() {
        for (int i = 1; i <= 10; ++i) {
            queue.push(i);
            std::cout << "Produced: " << i << '\n';
        }

        queue.close();
    });

    // Consumer
    std::thread consumer([&queue]() {
        int value;

        while (queue.pop(value)) {
            std::cout << "Consumed: " << value << '\n';
        }

        std::cout << "Consumer finished\n";
    });

    producer.join();
    consumer.join();

    std::cout << "Remaining items: "
              << queue.size() << '\n';

    return 0;
}

