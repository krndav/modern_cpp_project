# Work-Stealing Thread Pool (C++17)

## Files
- `work_stealing_pool.hpp`: public API and templated `submit()`
- `work_stealing_pool.cpp`: worker queues, stealing, synchronization, shutdown
- `main.cpp`: example usage and tests
- `Makefile`: build/run/clean targets

## Build and run
```bash
make
make run
```

If your compiler is not named `g++-15`, override it:
```bash
make CXX=g++
```

Clean generated files:
```bash
make clean
```

This educational implementation uses mutex-protected per-worker deques. It demonstrates work stealing, but it is not a lock-free deque. The pool accepts submissions from external threads and drains accepted tasks on shutdown. Do not call `shutdown()` from one of the pool's own tasks.
