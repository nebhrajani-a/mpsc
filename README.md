# Lock-Free MPSC Queue

A simple lock-free multi-producer single-consumer (MPSC) queue implementation in C++17.

## Overview

This queue allows multiple producer threads to concurrently enqueue
items while a single consumer thread dequeues them. It's based on
Dmitry Vyukov's intrusive MPSC queue.

## Usage

```cpp
#include "mpsc_queue.hpp"

MPSCQueue<int> queue;

// Producer threads (can be multiple)
queue.enqueue(42);
queue.enqueue(std::move(value));

// Consumer thread (single)
if (auto item = queue.dequeue()) {
    process(*item);
}

// Check if empty (best-effort in concurrent context)
if (!queue.empty()) {
    // ...
}
```

## Building

Requires Bazel 8.0+ and a C++17 compiler.

```bash
# Build everything
bazel build //...

# Run tests
bazel test //...

# Run with ThreadSanitizer (recommended)
bazel test --config=tsan //...

# Run benchmarks
bazel run --config=opt //:mpsc_queue_benchmark
```

## License

MIT
