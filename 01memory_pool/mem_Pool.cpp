
#include <cstddef>
#include <iostream>
#include <new>
#include <utility>

template <typename T>
class MemoryPool {
private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
        Slot* next;
    };

    Slot* slots;
    Slot* freeList;
    std::size_t capacity;

public:
    explicit MemoryPool(std::size_t count)
        : slots(new Slot[count]),
          freeList(nullptr),
          capacity(count) {

        // Link every slot into the free list.
        for (std::size_t i = 0; i < capacity; ++i) {
            slots[i].next = freeList;
            freeList = &slots[i];
        }
    }

    // A pool must not be copied: it owns its memory.
    MemoryPool(const MemoryPool&) = delete;
    MemoryPool& operator=(const MemoryPool&) = delete;

    ~MemoryPool() {
        delete[] slots;
    }

    template <typename... Args>
    T* create(Args&&... args) {
        if (!freeList) {
            throw std::bad_alloc();
        }

        Slot* slot = freeList;
        freeList = freeList->next;

        try {
            return new (slot->storage)
                T(std::forward<Args>(args)...);
        } catch (...) {
            slot->next = freeList;
            freeList = slot;
            throw;
        }
    }

    void destroy(T* object) {
        if (!object) {
            return;
        }

        // Run the object's destructor.
        object->~T();

        // Return the slot to the free list.
        Slot* slot = reinterpret_cast<Slot*>(object);
        slot->next = freeList;
        freeList = slot;
    }
};

struct Particle {
    int id;

    explicit Particle(int value) : id(value) {
        std::cout << "Constructed: " << id << '\n';
    }

    ~Particle() {
        std::cout << "Destroyed: " << id << '\n';
    }
};

int main() {
    MemoryPool<Particle> pool(3);

    Particle* p1 = pool.create(101);
    Particle* p2 = pool.create(102);

    std::cout << "Particle ID: " << p1->id << '\n';

    pool.destroy(p1);
    pool.destroy(p2);

    // The released slots can be reused.
    Particle* p3 = pool.create(103);
    pool.destroy(p3);

    return 0;
}

