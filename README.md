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

## Benchmarks
On a MacBook Pro 2019, Intel i9-9880H, 16GB RAM.

```
---------------------------------------------------------------------------------------------------
Benchmark                                         Time             CPU   Iterations UserCounters...
---------------------------------------------------------------------------------------------------
BM_LockFree_SingleThread_Enqueue/1000         46838 ns        46820 ns        13663 items_per_second=21.3582M/s
BM_LockFree_SingleThread_Enqueue/4096        227993 ns       227606 ns         3260 items_per_second=17.996M/s
BM_LockFree_SingleThread_Enqueue/32768      1678039 ns      1677157 ns          383 items_per_second=19.5378M/s
BM_LockFree_SingleThread_Enqueue/262144    13977271 ns     13966444 ns           45 items_per_second=18.7696M/s
BM_LockFree_SingleThread_Enqueue/1000000   61655188 ns     61404692 ns           13 items_per_second=16.2854M/s
BM_Mutex_SingleThread_Enqueue/1000            67057 ns        67012 ns         9418 items_per_second=14.9227M/s
BM_Mutex_SingleThread_Enqueue/4096           302327 ns       302046 ns         2503 items_per_second=13.5608M/s
BM_Mutex_SingleThread_Enqueue/32768         2223779 ns      2222381 ns          302 items_per_second=14.7445M/s
BM_Mutex_SingleThread_Enqueue/262144       20663727 ns     20458250 ns           36 items_per_second=12.8136M/s
BM_Mutex_SingleThread_Enqueue/1000000      81666629 ns     77197667 ns            9 items_per_second=12.9538M/s
BM_LockFree_SingleThread_Dequeue/1000         27661 ns        27602 ns        24014 items_per_second=36.229M/s
BM_LockFree_SingleThread_Dequeue/4096        120319 ns       119891 ns         6250 items_per_second=34.1644M/s
BM_LockFree_SingleThread_Dequeue/32768       909017 ns       908248 ns          709 items_per_second=36.0782M/s
BM_LockFree_SingleThread_Dequeue/262144     7466772 ns      7454378 ns           98 items_per_second=35.1665M/s
BM_LockFree_SingleThread_Dequeue/1000000   27869852 ns     27848920 ns           25 items_per_second=35.908M/s
BM_LockFree_RoundTrip                          46.4 ns         46.3 ns     14732407
BM_Mutex_RoundTrip                             80.7 ns         80.6 ns      9538214
BM_LockFree_MultiProducer/1                 1345271 ns        51162 ns        12821 items_per_second=195.458M/s
BM_LockFree_MultiProducer/2                 1726718 ns        62023 ns        12114 items_per_second=322.463M/s
BM_LockFree_MultiProducer/4                 2892013 ns        98094 ns         1000 items_per_second=407.772M/s
BM_LockFree_MultiProducer/8                 5643695 ns       214621 ns         1000 items_per_second=372.75M/s
BM_LockFree_MultiProducer/16                9650245 ns       409915 ns         1000 items_per_second=390.325M/s
BM_Mutex_MultiProducer/1                    3826747 ns        54614 ns         1000 items_per_second=183.103M/s
BM_Mutex_MultiProducer/2                    4640600 ns        69226 ns         1000 items_per_second=288.909M/s
BM_Mutex_MultiProducer/4                    8755955 ns        93112 ns         1000 items_per_second=429.59M/s
BM_Mutex_MultiProducer/8                   18031429 ns       214401 ns         1000 items_per_second=373.133M/s
BM_Mutex_MultiProducer/16                 109144272 ns       660180 ns          100 items_per_second=242.358M/s
BM_LockFree_HighContention                  6400436 ns       309453 ns         1207 items_per_second=25.8521M/s
BM_Mutex_HighContention                    11647910 ns       241086 ns         1000 items_per_second=33.1832M/s
BM_LockFree_IntPayload                         73.2 ns         70.8 ns      9635237
BM_LockFree_LongPayload                        65.5 ns         64.6 ns     10216890
BM_LockFree_StringPayload                       129 ns          128 ns      5213531
BM_LockFree_SustainedThroughput/0              54.3 ns         54.0 ns     12846158
BM_LockFree_SustainedThroughput/10             52.8 ns         52.2 ns     11928091
BM_LockFree_SustainedThroughput/100            54.4 ns         53.8 ns     11291780
BM_LockFree_SustainedThroughput/1000           52.5 ns         52.0 ns     12751617
BM_LockFree_EmptyDequeue                      0.880 ns        0.873 ns    828206342
BM_Mutex_EmptyDequeue                          20.5 ns         20.4 ns     34635805
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
