    
#include <iostream>
#include <mutex>
#include <vector>
#include <string>
#include <thread>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>
#include <limits>

// Thread-safe memory pool for objects of type T
template <typename T>
class MemoryPool {
private:
    struct FreeBlock {
        FreeBlock* next;
    };

    static constexpr std::size_t blockSize =
        sizeof(T) > sizeof(FreeBlock)
            ? sizeof(T)
            : sizeof(FreeBlock);

    static constexpr std::size_t blockAlign =
        alignof(T) > alignof(FreeBlock)
            ? alignof(T)
            : alignof(FreeBlock);

    struct alignas(blockAlign) Block {
        alignas(blockAlign) std::byte storage[blockSize];
    };

    std::vector<Block*> chunks;
    FreeBlock* freeList = nullptr;

    const std::size_t blocksPerChunk;
    std::size_t allocated = 0;
    mutable std::mutex mtx;

    // Add a new chunk of blocks to the pool.
    void grow() {
        Block* chunk = new Block[blocksPerChunk];

        try {
            chunks.push_back(chunk);
        } catch (...) {
            delete[] chunk;
            throw;
        }

        for (std::size_t i = 0; i < blocksPerChunk; ++i) {
            auto* block = reinterpret_cast<FreeBlock*>(
                chunk[i].storage
            );

            block->next = freeList;
            freeList = block;
        }
    }

    // Return raw memory to the free list.
    void release(void* memory) {
        std::lock_guard<std::mutex> lock(mtx);

        auto* block = static_cast<FreeBlock*>(memory);
        block->next = freeList;
        freeList = block;
        --allocated;
    }

public:
    explicit MemoryPool(std::size_t blocks = 256)
        : blocksPerChunk(blocks) {
        if (blocks == 0) {
            throw std::invalid_argument(
                "Blocks per chunk must be positive"
            );
        }
    }

    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    // Construct an object using its constructor arguments.
    template <typename... Args>
    T* create(Args&&... args) {
        void* memory = nullptr;

        {
            std::lock_guard<std::mutex> lock(mtx);

            if (freeList == nullptr) {
                grow();
            }

            FreeBlock* block = freeList;
            freeList = freeList->next;
            ++allocated;

            memory = block;
        }

        try {
            return new (memory) T(
                std::forward<Args>(args)...
            );
        } catch (...) {
            release(memory);
            throw;
        }
    }

    // Destroy the object and return its storage.
    void destroy(T* object) {
        if (object == nullptr) {
            return;
        }

        object->~T();
        release(object);
    }

    std::size_t outstanding() const {
        std::lock_guard<std::mutex> lock(mtx);
        return allocated;
    }

    // All objects must be destroyed before the pool.
    ~MemoryPool() {
        for (Block* chunk : chunks) {
            delete[] chunk;
        }
    }
};

// Example object 1
struct User {
    int id;
    std::string name;

    User(int id, std::string name)
        : id(id), name(std::move(name)) {}

    ~User() {
        // Demonstrates object destruction.
    }
};

// Example object 2
struct Sensor {
    double temperature;

    explicit Sensor(double temperature)
        : temperature(temperature) {}
};

int main() {
    MemoryPool<User> userPool(16);
    MemoryPool<Sensor> sensorPool(16);

    // Basic allocation and deallocation.
    User* user = userPool.create(1, "Karan");

    std::cout << "User: "
              << user->id << ", "
              << user->name << '\n';

    userPool.destroy(user);

    Sensor* sensor = sensorPool.create(36.5);

    std::cout << "Temperature: "
              << sensor->temperature << '\n';

    sensorPool.destroy(sensor);

    // Concurrent allocation and destruction.
    auto worker = [](MemoryPool<User>& pool, int threadId) {
        for (int i = 0; i < 1000; ++i) {
            User* item = pool.create(
                threadId * 1000 + i,
                "Worker"
            );

            // Use the object while this thread owns it.
            if (item->id < 0) {
                std::cerr << "Unexpected ID\n";
            }

            pool.destroy(item);
        }
    };

    std::thread t1(worker, std::ref(userPool), 1);
    std::thread t2(worker, std::ref(userPool), 2);
    std::thread t3(worker, std::ref(userPool), 3);
    std::thread t4(worker, std::ref(userPool), 4);

    t1.join();
    t2.join();
    t3.join();
    t4.join();

    std::cout << "Outstanding user objects: "
              << userPool.outstanding() << '\n';

    std::cout << "All tests completed.\n";

    return 0;
}

