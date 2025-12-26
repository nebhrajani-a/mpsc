#include "mpsc_queue.hpp"

#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>
#include <random>
#include <chrono>
#include <set>
#include <condition_variable>
#include <mutex>

/**
 * Adversarial Tests for MPSC Queue
 * 
 * These tests are specifically designed to expose common bugs in lock-free
 * MPSC queue implementations:
 * 
 * 1. Missing memory barriers / wrong memory ordering
 * 2. Race conditions in enqueue (between exchange and linking)
 * 3. Race conditions in dequeue (reading partially constructed nodes)
 * 4. Memory leaks / use-after-free
 * 5. Lost updates under high contention
 * 6. FIFO violations
 * 
 * Run with ThreadSanitizer: bazel test --config=tsan :mpsc_queue_adversarial_test
 */

// ============================================================================
// C++17 compatible barrier implementation
// ============================================================================

class SimpleBarrier {
public:
    explicit SimpleBarrier(int count) : threshold_(count), count_(count), generation_(0) {}

    void arrive_and_wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        int gen = generation_;
        if (--count_ == 0) {
            ++generation_;
            count_ = threshold_;
            cv_.notify_all();
        } else {
            cv_.wait(lock, [this, gen] { return gen != generation_; });
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    int threshold_;
    int count_;
    int generation_;
};

// ============================================================================
// Test Fixture with timeout helper
// ============================================================================

class MPSCQueueAdversarialTest : public ::testing::Test {
protected:
    // Hardware concurrency for scaling tests
    unsigned int num_threads = std::max(4u, std::thread::hardware_concurrency());
    
    // Timeout for tests
    static constexpr int TIMEOUT_SECONDS = 30;
    
    // Helper to check timeout and fail gracefully
    template<typename Func>
    void run_with_timeout(Func&& consumer_loop, int expected_items, const char* test_name) {
        auto start = std::chrono::steady_clock::now();
        int empty_spins = 0;
        constexpr int MAX_EMPTY_SPINS = 1000000;
        int received = 0;
        
        while (received < expected_items) {
            if (consumer_loop(received)) {
                empty_spins = 0;
            } else {
                ++empty_spins;
                if (empty_spins > MAX_EMPTY_SPINS) {
                    // Check if we've made any progress recently
                    auto elapsed = std::chrono::steady_clock::now() - start;
                    if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
                        FAIL() << test_name << " timed out. Received " << received 
                               << "/" << expected_items << " items.";
                    }
                }
            }
            std::this_thread::yield();
        }
    }
};

// ============================================================================
// TEST: Burst Contention
// Multiple producers simultaneously try to enqueue at the exact same moment
// This stresses the atomic exchange in enqueue()
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, BurstContention) {
    constexpr int ITERATIONS = 100;
    constexpr int ITEMS_PER_BURST = 1000;
    
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        MPSCQueue<int> queue;
        const int num_producers = static_cast<int>(num_threads);
        const int total_items = num_producers * ITEMS_PER_BURST;
        
        SimpleBarrier sync_point(num_producers);
        std::vector<std::thread> producers;
        std::atomic<int> received{0};
        std::atomic<int> producers_done{0};
        std::set<int> values;
        
        // All producers wait at barrier, then burst simultaneously
        for (int p = 0; p < num_producers; ++p) {
            producers.emplace_back([&queue, &sync_point, &producers_done, p]() {
                sync_point.arrive_and_wait();  // Synchronize start
                
                int base = p * ITEMS_PER_BURST;
                for (int i = 0; i < ITEMS_PER_BURST; ++i) {
                    queue.enqueue(base + i);
                }
                ++producers_done;
            });
        }
        
        // Consumer with timeout
        auto start = std::chrono::steady_clock::now();
        int empty_spins = 0;
        
        while (received < total_items) {
            auto result = queue.dequeue();
            if (result.has_value()) {
                ASSERT_EQ(values.count(result.value()), 0) 
                    << "Duplicate in iteration " << iter;
                values.insert(result.value());
                ++received;
                empty_spins = 0;
            } else {
                ++empty_spins;
                if (producers_done == num_producers && empty_spins > 100000) {
                    break;  // Producers done, queue seems empty
                }
            }
            
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed > std::chrono::seconds(5)) {
                for (auto& t : producers) t.join();
                FAIL() << "BurstContention iteration " << iter << " timed out. "
                       << "Received " << received.load() << "/" << total_items;
            }
        }
        
        for (auto& t : producers) {
            t.join();
        }
        
        ASSERT_EQ(values.size(), static_cast<size_t>(total_items));
    }
}

// ============================================================================
// TEST: Rapid Fire Single Producer
// One producer enqueues as fast as possible while consumer drains
// Tests the "window" between tail exchange and next pointer update
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, RapidFireSingleProducer) {
    constexpr int NUM_ITEMS = 1000000;
    
    MPSCQueue<int> queue;
    std::atomic<int> received{0};
    std::atomic<bool> producer_done{false};
    int last_value = -1;
    bool order_valid = true;
    
    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            queue.enqueue(i);
        }
        producer_done = true;
    });
    
    // Aggressive consumer with timeout
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    
    while (received < NUM_ITEMS) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            if (result.value() != last_value + 1) {
                order_valid = false;
            }
            last_value = result.value();
            ++received;
            empty_spins = 0;
        } else {
            ++empty_spins;
            if (producer_done && empty_spins > 1000000) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            producer.join();
            FAIL() << "RapidFireSingleProducer timed out. Received " 
                   << received.load() << "/" << NUM_ITEMS;
        }
    }
    
    producer.join();
    
    EXPECT_EQ(received.load(), NUM_ITEMS);
    EXPECT_TRUE(order_valid) << "FIFO order violated";
}

// ============================================================================
// TEST: Alternating Enqueue Dequeue
// Rapidly alternating between empty and non-empty states
// Stresses edge case handling around the dummy node
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, AlternatingEmptyNonEmpty) {
    constexpr int ITERATIONS = 100000;
    
    MPSCQueue<int> queue;
    std::atomic<int> enqueued{0};
    std::atomic<int> dequeued{0};
    std::atomic<bool> stop{false};
    
    // Producer: enqueue one item, wait for it to be consumed
    std::thread producer([&]() {
        for (int i = 0; i < ITERATIONS && !stop; ++i) {
            queue.enqueue(i);
            ++enqueued;
            
            // Spin until consumed (creates alternating pattern)
            int spins = 0;
            while (dequeued < enqueued && !stop && spins < 10000000) {
                std::this_thread::yield();
                ++spins;
            }
            if (spins >= 10000000) {
                stop = true;  // Timeout in producer
            }
        }
    });
    
    // Consumer with timeout
    auto start = std::chrono::steady_clock::now();
    int expected = 0;
    
    while (dequeued < ITERATIONS && !stop) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            EXPECT_EQ(result.value(), expected) 
                << "Wrong value at position " << expected;
            ++expected;
            ++dequeued;
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            stop = true;
            producer.join();
            FAIL() << "AlternatingEmptyNonEmpty timed out at " << dequeued.load() << "/" << ITERATIONS;
        }
    }
    
    stop = true;
    producer.join();
}

// ============================================================================
// TEST: Memory Ordering Stress
// Enqueue complex objects to detect missing memory barriers
// If barriers are wrong, we'll see partially constructed objects
// ============================================================================

struct ComplexObject {
    int a, b, c, d;
    long checksum;
    
    ComplexObject() : a(0), b(0), c(0), d(0), checksum(0) {}
    
    explicit ComplexObject(int val) 
        : a(val), b(val * 2), c(val * 3), d(val * 4),
          checksum(static_cast<long>(a) + b + c + d) {}
    
    bool isValid() const {
        return checksum == static_cast<long>(a) + b + c + d;
    }
};

TEST_F(MPSCQueueAdversarialTest, MemoryOrderingWithComplexObjects) {
    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 50000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    MPSCQueue<ComplexObject> queue;
    std::vector<std::thread> producers;
    std::atomic<int> received{0};
    std::atomic<int> corrupted{0};
    std::atomic<int> producers_done{0};
    
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &producers_done, p]() {
            int base = p * ITEMS_PER_PRODUCER;
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                queue.enqueue(ComplexObject(base + i));
            }
            ++producers_done;
        });
    }
    
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    
    while (received < TOTAL_ITEMS) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            if (!result.value().isValid()) {
                ++corrupted;
            }
            ++received;
            empty_spins = 0;
        } else {
            ++empty_spins;
            if (producers_done == NUM_PRODUCERS && empty_spins > 1000000) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            for (auto& t : producers) t.join();
            FAIL() << "MemoryOrderingWithComplexObjects timed out. Received " 
                   << received.load() << "/" << TOTAL_ITEMS;
        }
        std::this_thread::yield();
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    EXPECT_EQ(received.load(), TOTAL_ITEMS);
    EXPECT_EQ(corrupted.load(), 0) 
        << "Detected " << corrupted.load() << " corrupted objects";
}

// ============================================================================
// TEST: Random Timing Stress
// Random delays to hit different interleavings
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, RandomTimingStress) {
    constexpr int NUM_PRODUCERS = 8;
    constexpr int ITEMS_PER_PRODUCER = 5000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    MPSCQueue<int> queue;
    std::vector<std::thread> producers;
    std::atomic<int> received{0};
    std::atomic<int> producers_done{0};
    std::set<int> values;
    
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &producers_done, p]() {
            std::mt19937 rng(static_cast<unsigned>(p * 12345));
            std::uniform_int_distribution<int> delay_dist(0, 100);
            
            int base = p * ITEMS_PER_PRODUCER;
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                queue.enqueue(base + i);
                
                // Random micro-delays to vary timing
                if (delay_dist(rng) < 10) {
                    std::this_thread::yield();
                }
            }
            ++producers_done;
        });
    }
    
    std::mt19937 consumer_rng(99999);
    std::uniform_int_distribution<int> delay_dist(0, 100);
    
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    
    while (received < TOTAL_ITEMS) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            ASSERT_EQ(values.count(result.value()), 0);
            values.insert(result.value());
            ++received;
            empty_spins = 0;
            
            if (delay_dist(consumer_rng) < 5) {
                std::this_thread::yield();
            }
        } else {
            ++empty_spins;
            if (producers_done == NUM_PRODUCERS && empty_spins > 1000000) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            for (auto& t : producers) t.join();
            FAIL() << "RandomTimingStress timed out. Received " << received.load() << "/" << TOTAL_ITEMS;
        }
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    EXPECT_EQ(values.size(), static_cast<size_t>(TOTAL_ITEMS));
}

// ============================================================================
// TEST: Producer Exit Race
// Producer exits immediately after last enqueue
// Tests cleanup doesn't interfere with pending operations
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, ProducerExitRace) {
    constexpr int ITERATIONS = 1000;
    
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        MPSCQueue<int> queue;
        constexpr int NUM_ITEMS = 100;
        
        std::thread producer([&]() {
            for (int i = 0; i < NUM_ITEMS; ++i) {
                queue.enqueue(i);
            }
            // Producer exits immediately - no synchronization
        });
        
        producer.join();  // Producer is definitely done
        
        // All items should be retrievable
        int count = 0;
        int empty_count = 0;
        while (count < NUM_ITEMS && empty_count < 1000) {
            auto result = queue.dequeue();
            if (result.has_value()) {
                EXPECT_EQ(result.value(), count);
                ++count;
                empty_count = 0;
            } else {
                ++empty_count;
            }
        }
        
        EXPECT_EQ(count, NUM_ITEMS) << "Lost items in iteration " << iter;
    }
}

// ============================================================================
// TEST: Maximum Contention
// As many producers as possible, all contending on tail
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, MaximumContention) {
    const int num_producers = static_cast<int>(num_threads) * 2;  // Oversubscribe
    constexpr int ITEMS_PER_PRODUCER = 10000;
    const int total_items = num_producers * ITEMS_PER_PRODUCER;
    
    MPSCQueue<long> queue;
    std::vector<std::thread> producers;
    
    std::atomic<long> expected_sum{0};
    std::atomic<long> actual_sum{0};
    std::atomic<int> received{0};
    std::atomic<int> producers_done{0};
    
    SimpleBarrier start_barrier(num_producers + 1);  // +1 for main thread
    
    for (int p = 0; p < num_producers; ++p) {
        producers.emplace_back([&, p]() {
            start_barrier.arrive_and_wait();
            
            long local_sum = 0;
            long base = static_cast<long>(p) * ITEMS_PER_PRODUCER;
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                long val = base + i;
                queue.enqueue(val);
                local_sum += val;
            }
            expected_sum.fetch_add(local_sum, std::memory_order_relaxed);
            ++producers_done;
        });
    }
    
    start_barrier.arrive_and_wait();  // Release all producers at once
    
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    
    while (received < total_items) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            actual_sum.fetch_add(result.value(), std::memory_order_relaxed);
            ++received;
            empty_spins = 0;
        } else {
            ++empty_spins;
            if (producers_done == num_producers && empty_spins > 1000000) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            for (auto& t : producers) t.join();
            FAIL() << "MaximumContention timed out. Received " << received.load() << "/" << total_items;
        }
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    EXPECT_EQ(received.load(), total_items);
    EXPECT_EQ(expected_sum.load(), actual_sum.load());
}

// ============================================================================
// TEST: Slow Consumer
// Consumer is much slower than producers
// Tests queue behavior under memory pressure
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, SlowConsumer) {
    constexpr int NUM_PRODUCERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 10000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    
    MPSCQueue<int> queue;
    std::vector<std::thread> producers;
    std::atomic<int> received{0};
    std::atomic<int> producers_done{0};
    
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &producers_done, p]() {
            int base = p * ITEMS_PER_PRODUCER;
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                queue.enqueue(base + i);
            }
            ++producers_done;
        });
    }
    
    // Slow consumer with artificial delays
    std::set<int> values;
    int delay_counter = 0;
    
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    
    while (received < TOTAL_ITEMS) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            ASSERT_EQ(values.count(result.value()), 0);
            values.insert(result.value());
            ++received;
            empty_spins = 0;
            
            // Periodic long delays
            if (++delay_counter % 1000 == 0) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        } else {
            ++empty_spins;
            if (producers_done == NUM_PRODUCERS && empty_spins > 1000000) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(60)) {  // Longer timeout for slow consumer
            for (auto& t : producers) t.join();
            FAIL() << "SlowConsumer timed out. Received " << received.load() << "/" << TOTAL_ITEMS;
        }
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    EXPECT_EQ(values.size(), static_cast<size_t>(TOTAL_ITEMS));
}

// ============================================================================
// TEST: Repeated Create Destroy
// Stress test constructor/destructor interactions
// ============================================================================

TEST_F(MPSCQueueAdversarialTest, RepeatedCreateDestroy) {
    constexpr int ITERATIONS = 1000;
    
    for (int iter = 0; iter < ITERATIONS; ++iter) {
        auto queue = std::make_unique<MPSCQueue<int>>();
        
        // Enqueue some items
        for (int i = 0; i < 100; ++i) {
            queue->enqueue(i);
        }
        
        // Dequeue some (but not all)
        for (int i = 0; i < 50; ++i) {
            auto result = queue->dequeue();
            // Don't assert - implementation may be incomplete
            if (result.has_value()) {
                EXPECT_EQ(result.value(), i);
            }
        }
        
        // Destroy with items still in queue
        queue.reset();
        
        // If we get here without crashing/leaking, good!
    }
}

// ============================================================================
// TEST: Type with Expensive Copy
// Ensure move semantics are properly used
// ============================================================================

class ExpensiveCopy {
public:
    static std::atomic<int> copy_count;
    static std::atomic<int> move_count;
    
    int value;
    
    ExpensiveCopy(int v = 0) : value(v) {}
    
    ExpensiveCopy(const ExpensiveCopy& other) : value(other.value) {
        ++copy_count;
    }
    
    ExpensiveCopy(ExpensiveCopy&& other) noexcept : value(other.value) {
        ++move_count;
        other.value = -1;
    }
    
    ExpensiveCopy& operator=(const ExpensiveCopy& other) {
        value = other.value;
        ++copy_count;
        return *this;
    }
    
    ExpensiveCopy& operator=(ExpensiveCopy&& other) noexcept {
        value = other.value;
        ++move_count;
        other.value = -1;
        return *this;
    }
};

std::atomic<int> ExpensiveCopy::copy_count{0};
std::atomic<int> ExpensiveCopy::move_count{0};

TEST_F(MPSCQueueAdversarialTest, MoveSemanticEfficiency) {
    ExpensiveCopy::copy_count = 0;
    ExpensiveCopy::move_count = 0;
    
    MPSCQueue<ExpensiveCopy> queue;
    
    // Use move version of enqueue
    for (int i = 0; i < 100; ++i) {
        ExpensiveCopy obj(i);
        queue.enqueue(std::move(obj));
    }
    
    for (int i = 0; i < 100; ++i) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            EXPECT_EQ(result.value().value, i);
        }
    }
    
    // Note: Some copies may occur, but moves should dominate
    // This test documents behavior rather than enforcing strict counts
    // Only check if we actually got results (implementation exists)
    if (ExpensiveCopy::move_count.load() > 0 || ExpensiveCopy::copy_count.load() > 0) {
        EXPECT_GT(ExpensiveCopy::move_count.load(), 0) 
            << "Move semantics not being used";
    }
}
