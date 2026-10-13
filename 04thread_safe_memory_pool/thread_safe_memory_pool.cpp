
#include <iostream>
#include <mutex>
#include <vector>
#include <unordered_set>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <limits>
#include <thread>

class ThreadSafeMemoryPool {
private:
    struct FreeBlock {
        FreeBlock* next;
    };

    void* storage = nullptr;
    FreeBlock* freeList = nullptr;

    const std::size_t blockSize;
    const std::size_t capacity;
    const std::size_t alignment;
    const std::size_t stride;

    std::mutex mtx;
    std::unordered_set<void*> inUse;

    static std::size_t roundUp(std::size_t n,
                               std::size_t align) {
        const std::size_t remainder = n % align;

        if (remainder == 0)
            return n;

        const std::size_t extra = align - remainder;

        if (n > std::numeric_limits<std::size_t>::max() - extra)
            throw std::length_error("Block size overflow");

        return n + extra;
    }

public:
    ThreadSafeMemoryPool(std::size_t requestedBlockSize,
                         std::size_t numBlocks,
                         std::size_t requestedAlignment =
                             alignof(std::max_align_t))
        : blockSize(requestedBlockSize),
          capacity(numBlocks),
          alignment(requestedAlignment),
          stride(roundUp(
              requestedBlockSize < sizeof(FreeBlock)
                  ? sizeof(FreeBlock)
                  : requestedBlockSize,
              requestedAlignment))
    {
        if (requestedBlockSize == 0 || numBlocks == 0)
            throw std::invalid_argument(
                "Block size and capacity must be positive"
            );

        if (alignment < alignof(FreeBlock) ||
            (alignment & (alignment - 1)) != 0) {
            throw std::invalid_argument(
                "Alignment must be a suitable power of two"
            );
        }

        if (capacity >
            std::numeric_limits<std::size_t>::max() / stride) {
            throw std::length_error("Pool size overflow");
        }

        storage = ::operator new(
            stride * capacity, std::align_val_t{alignment}
        );

        // Build the initial free list.
        auto* base = static_cast<std::byte*>(storage);

        for (std::size_t i = 0; i < capacity; ++i) {
            auto* block = reinterpret_cast<FreeBlock*>(
                base + i * stride
            );

            block->next = freeList;
            freeList = block;
        }
    }

    ThreadSafeMemoryPool(const ThreadSafeMemoryPool&) = delete;
    ThreadSafeMemoryPool& operator=(
        const ThreadSafeMemoryPool&) = delete;

    void* allocate() {
        std::lock_guard<std::mutex> lock(mtx);

        if (freeList == nullptr)
            throw std::bad_alloc();

        FreeBlock* block = freeList;
        freeList = freeList->next;

        try {
            inUse.insert(block);
        } catch (...) {
            block->next = freeList;
            freeList = block;
            throw;
        }

        return block;
    }

    void deallocate(void* ptr) {
        if (ptr == nullptr)
            return;

        std::lock_guard<std::mutex> lock(mtx);

        // Reject foreign pointers and double frees.
        auto it = inUse.find(ptr);

        if (it == inUse.end()) {
            throw std::invalid_argument(
                "Invalid pointer or double deallocation"
            );
        }

        inUse.erase(it);

        auto* block = static_cast<FreeBlock*>(ptr);
        block->next = freeList;
        freeList = block;
    }

    std::size_t block_size() const {
        return blockSize;
    }

    std::size_t available() {
        std::lock_guard<std::mutex> lock(mtx);
        return capacity - inUse.size();
    }

    ~ThreadSafeMemoryPool() {
        ::operator delete(
            storage, std::align_val_t{alignment}
        );
    }
};

// Example type allocated from the pool.
struct Data {
    int id;
    double value;

    Data(int id, double value)
        : id(id), value(value) {}
};

int main() {
    ThreadSafeMemoryPool pool(sizeof(Data), 100);

    auto worker = [&pool](int threadId) {
        for (int i = 0; i < 10; ++i) {
            void* memory = pool.allocate();

            try {
                Data* data = new (memory)
                    Data(threadId * 100 + i, i * 1.5);

                std::cout << "Thread " << threadId
                          << ": Data ID = " << data->id
                          << ", value = " << data->value
                          << '\n';

                data->~Data();
                pool.deallocate(memory);
            } catch (...) {
                // Return the block if object construction fails.
                pool.deallocate(memory);
                throw;
            }
        }
    };

    std::thread t1(worker, 1);
    std::thread t2(worker, 2);
    std::thread t3(worker, 3);
    std::thread t4(worker, 4);

    t1.join();
    t2.join();
    t3.join();
    t4.join();

    std::cout << "\nAvailable blocks: "
              << pool.available() << '\n';

    return 0;
}

