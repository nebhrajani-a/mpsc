#include "mpsc_queue.hpp"

#include <benchmark/benchmark.h>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <queue>

// ============================================================================
// Baseline: Mutex-based queue for comparison
// Uses linked list with node allocation (same as lock-free) for fair comparison
// ============================================================================

template <typename T>
class MutexQueue {
    struct Node {
        T data;
        Node* next;
        Node() : data{}, next{nullptr} {}
        explicit Node(const T& value) : data{value}, next{nullptr} {}
        explicit Node(T&& value) : data{std::move(value)}, next{nullptr} {}
    };
    
    Node* head_;
    Node* tail_;
    mutable std::mutex mutex_;

public:
    MutexQueue() {
        Node* dummy = new Node();
        head_ = dummy;
        tail_ = dummy;
    }
    
    ~MutexQueue() {
        while (head_ != nullptr) {
            Node* tmp = head_->next;
            delete head_;
            head_ = tmp;
        }
    }
    
    MutexQueue(const MutexQueue&) = delete;
    MutexQueue& operator=(const MutexQueue&) = delete;

    void enqueue(const T& value) {
        Node* node = new Node(value);  // Allocate outside lock for fairness
        std::lock_guard<std::mutex> lock(mutex_);
        tail_->next = node;
        tail_ = node;
    }
    
    void enqueue(T&& value) {
        Node* node = new Node(std::move(value));
        std::lock_guard<std::mutex> lock(mutex_);
        tail_->next = node;
        tail_ = node;
    }
    
    std::optional<T> dequeue() {
        std::lock_guard<std::mutex> lock(mutex_);
        Node* next = head_->next;
        if (next == nullptr) {
            return std::nullopt;
        }
        T value = std::move(next->data);
        Node* old_head = head_;
        head_ = next;
        delete old_head;  // Delete inside lock for simplicity
        return value;
    }
    
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return head_->next == nullptr;
    }
};

// ============================================================================
// Benchmark: Single-threaded enqueue throughput
// ============================================================================

static void BM_LockFree_SingleThread_Enqueue(benchmark::State& state) {
    for (auto _ : state) {
        MPSCQueue<int> queue;
        for (int i = 0; i < state.range(0); ++i) {
            queue.enqueue(i);
        }
        benchmark::DoNotOptimize(queue);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_LockFree_SingleThread_Enqueue)->Range(1000, 1000000);

static void BM_Mutex_SingleThread_Enqueue(benchmark::State& state) {
    for (auto _ : state) {
        MutexQueue<int> queue;
        for (int i = 0; i < state.range(0); ++i) {
            queue.enqueue(i);
        }
        benchmark::DoNotOptimize(queue);
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_Mutex_SingleThread_Enqueue)->Range(1000, 1000000);

// ============================================================================
// Benchmark: Single-threaded dequeue throughput
// ============================================================================

static void BM_LockFree_SingleThread_Dequeue(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        MPSCQueue<int> queue;
        for (int i = 0; i < state.range(0); ++i) {
            queue.enqueue(i);
        }
        state.ResumeTiming();
        
        for (int i = 0; i < state.range(0); ++i) {
            auto result = queue.dequeue();
            benchmark::DoNotOptimize(result);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0));
}
BENCHMARK(BM_LockFree_SingleThread_Dequeue)->Range(1000, 1000000);

// ============================================================================
// Benchmark: Round-trip latency (enqueue + dequeue single item)
// ============================================================================

static void BM_LockFree_RoundTrip(benchmark::State& state) {
    MPSCQueue<int> queue;
    
    for (auto _ : state) {
        queue.enqueue(42);
        auto result = queue.dequeue();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_LockFree_RoundTrip);

static void BM_Mutex_RoundTrip(benchmark::State& state) {
    MutexQueue<int> queue;
    
    for (auto _ : state) {
        queue.enqueue(42);
        auto result = queue.dequeue();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_Mutex_RoundTrip);

// ============================================================================
// Benchmark: Multi-producer throughput
// ============================================================================

static void BM_LockFree_MultiProducer(benchmark::State& state) {
    const int num_producers = static_cast<int>(state.range(0));
    constexpr int kItemsPerProducer = 10000;
    const int total_items = num_producers * kItemsPerProducer;
    
    for (auto _ : state) {
        MPSCQueue<int> queue;
        std::vector<std::thread> producers;
        std::atomic<int> received{0};
        
        // Consumer thread
        std::thread consumer([&]() {
            while (received < total_items) {
                auto result = queue.dequeue();
                if (result.has_value()) {
                    ++received;
                }
            }
        });
        
        // Producer threads
        for (int p = 0; p < num_producers; ++p) {
            producers.emplace_back([&queue, p]() {
                int base = p * kItemsPerProducer;
                for (int i = 0; i < kItemsPerProducer; ++i) {
                    queue.enqueue(base + i);
                }
            });
        }
        
        for (auto& t : producers) {
            t.join();
        }
        consumer.join();
    }
    
    state.SetItemsProcessed(state.iterations() * total_items);
}
BENCHMARK(BM_LockFree_MultiProducer)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

static void BM_Mutex_MultiProducer(benchmark::State& state) {
    const int num_producers = static_cast<int>(state.range(0));
    constexpr int kItemsPerProducer = 10000;
    const int total_items = num_producers * kItemsPerProducer;
    
    for (auto _ : state) {
        MutexQueue<int> queue;
        std::vector<std::thread> producers;
        std::atomic<int> received{0};
        
        // Consumer thread
        std::thread consumer([&]() {
            while (received < total_items) {
                auto result = queue.dequeue();
                if (result.has_value()) {
                    ++received;
                }
            }
        });
        
        // Producer threads
        for (int p = 0; p < num_producers; ++p) {
            producers.emplace_back([&queue, p]() {
                int base = p * kItemsPerProducer;
                for (int i = 0; i < kItemsPerProducer; ++i) {
                    queue.enqueue(base + i);
                }
            });
        }
        
        for (auto& t : producers) {
            t.join();
        }
        consumer.join();
    }
    
    state.SetItemsProcessed(state.iterations() * total_items);
}
BENCHMARK(BM_Mutex_MultiProducer)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

// ============================================================================
// Benchmark: Contention-heavy scenario
// Many producers, small batches with forced context switches
// ============================================================================

static void BM_LockFree_HighContention(benchmark::State& state) {
    constexpr int kNumProducers = 8;
    constexpr int kItemsPerProducer = 1000;
    constexpr int kTotalItems = kNumProducers * kItemsPerProducer;
    
    for (auto _ : state) {
        MPSCQueue<int> queue;
        std::vector<std::thread> producers;
        std::atomic<int> received{0};
        
        std::thread consumer([&]() {
            while (received < kTotalItems) {
                if (queue.dequeue().has_value()) {
                    ++received;
                }
            }
        });
        
        for (int p = 0; p < kNumProducers; ++p) {
            producers.emplace_back([&queue]() {
                for (int i = 0; i < kItemsPerProducer; ++i) {
                    queue.enqueue(i);
                    std::this_thread::yield();  // Force context switches
                }
            });
        }
        
        for (auto& t : producers) {
            t.join();
        }
        consumer.join();
    }
    
    state.SetItemsProcessed(state.iterations() * kTotalItems);
}
BENCHMARK(BM_LockFree_HighContention);

static void BM_Mutex_HighContention(benchmark::State& state) {
    constexpr int kNumProducers = 8;
    constexpr int kItemsPerProducer = 1000;
    constexpr int kTotalItems = kNumProducers * kItemsPerProducer;
    
    for (auto _ : state) {
        MutexQueue<int> queue;
        std::vector<std::thread> producers;
        std::atomic<int> received{0};
        
        std::thread consumer([&]() {
            while (received < kTotalItems) {
                if (queue.dequeue().has_value()) {
                    ++received;
                }
            }
        });
        
        for (int p = 0; p < kNumProducers; ++p) {
            producers.emplace_back([&queue]() {
                for (int i = 0; i < kItemsPerProducer; ++i) {
                    queue.enqueue(i);
                    std::this_thread::yield();
                }
            });
        }
        
        for (auto& t : producers) {
            t.join();
        }
        consumer.join();
    }
    
    state.SetItemsProcessed(state.iterations() * kTotalItems);
}
BENCHMARK(BM_Mutex_HighContention);

// ============================================================================
// Benchmark: Different payload sizes using long vs int
// ============================================================================

static void BM_LockFree_IntPayload(benchmark::State& state) {
    MPSCQueue<int> queue;
    for (auto _ : state) {
        queue.enqueue(42);
        benchmark::DoNotOptimize(queue.dequeue());
    }
}
BENCHMARK(BM_LockFree_IntPayload);

static void BM_LockFree_LongPayload(benchmark::State& state) {
    MPSCQueue<long> queue;
    for (auto _ : state) {
        queue.enqueue(42L);
        benchmark::DoNotOptimize(queue.dequeue());
    }
}
BENCHMARK(BM_LockFree_LongPayload);

static void BM_LockFree_StringPayload(benchmark::State& state) {
    MPSCQueue<std::string> queue;
    std::string payload = "benchmark test string payload";
    for (auto _ : state) {
        queue.enqueue(payload);
        benchmark::DoNotOptimize(queue.dequeue());
    }
}
BENCHMARK(BM_LockFree_StringPayload);

// ============================================================================
// Benchmark: Sustained throughput at different queue depths
// ============================================================================

static void BM_LockFree_SustainedThroughput(benchmark::State& state) {
    const int queue_depth = static_cast<int>(state.range(0));
    
    MPSCQueue<int> queue;
    
    // Pre-fill to target depth
    for (int i = 0; i < queue_depth; ++i) {
        queue.enqueue(i);
    }
    
    int counter = queue_depth;
    for (auto _ : state) {
        queue.enqueue(counter++);
        benchmark::DoNotOptimize(queue.dequeue());
    }
}
BENCHMARK(BM_LockFree_SustainedThroughput)->Arg(0)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================================
// Benchmark: Empty dequeue (fast path)
// ============================================================================

static void BM_LockFree_EmptyDequeue(benchmark::State& state) {
    MPSCQueue<int> queue;
    for (auto _ : state) {
        auto result = queue.dequeue();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_LockFree_EmptyDequeue);

static void BM_Mutex_EmptyDequeue(benchmark::State& state) {
    MutexQueue<int> queue;
    for (auto _ : state) {
        auto result = queue.dequeue();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_Mutex_EmptyDequeue);

BENCHMARK_MAIN();
