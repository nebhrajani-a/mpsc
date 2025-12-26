#include "mpsc_queue.hpp"

#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>
#include <set>
#include <atomic>
#include <chrono>

// ============================================================================
// Basic Functionality Tests
// ============================================================================

class MPSCQueueBasicTest : public ::testing::Test {
protected:
    MPSCQueue<int> int_queue;
    MPSCQueue<std::string> string_queue;
};

TEST_F(MPSCQueueBasicTest, EmptyQueueReturnsNullopt) {
    EXPECT_TRUE(int_queue.empty());
    EXPECT_EQ(int_queue.dequeue(), std::nullopt);
}

TEST_F(MPSCQueueBasicTest, SingleEnqueueDequeue) {
    int_queue.enqueue(42);
    EXPECT_FALSE(int_queue.empty());
    
    auto result = int_queue.dequeue();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), 42);
    EXPECT_TRUE(int_queue.empty());
}

TEST_F(MPSCQueueBasicTest, FIFOOrder) {
    for (int i = 0; i < 10; ++i) {
        int_queue.enqueue(i);
    }
    
    for (int i = 0; i < 10; ++i) {
        auto result = int_queue.dequeue();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), i);
    }
    
    EXPECT_TRUE(int_queue.empty());
}

TEST_F(MPSCQueueBasicTest, InterleavedOperations) {
    int_queue.enqueue(1);
    int_queue.enqueue(2);
    EXPECT_EQ(int_queue.dequeue().value(), 1);
    
    int_queue.enqueue(3);
    EXPECT_EQ(int_queue.dequeue().value(), 2);
    EXPECT_EQ(int_queue.dequeue().value(), 3);
    EXPECT_EQ(int_queue.dequeue(), std::nullopt);
}

TEST_F(MPSCQueueBasicTest, StringValues) {
    string_queue.enqueue("hello");
    string_queue.enqueue("world");
    
    EXPECT_EQ(string_queue.dequeue().value(), "hello");
    EXPECT_EQ(string_queue.dequeue().value(), "world");
}

TEST_F(MPSCQueueBasicTest, MoveSemantics) {
    std::string s = "move me";
    string_queue.enqueue(std::move(s));
    
    auto result = string_queue.dequeue();
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result.value(), "move me");
}

TEST_F(MPSCQueueBasicTest, LargeNumberOfItems) {
    const int N = 100000;
    
    for (int i = 0; i < N; ++i) {
        int_queue.enqueue(i);
    }
    
    for (int i = 0; i < N; ++i) {
        auto result = int_queue.dequeue();
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(result.value(), i);
    }
}

// ============================================================================
// Concurrent Tests - Single Producer Single Consumer
// ============================================================================

class MPSCQueueSPSCTest : public ::testing::Test {
protected:
    static constexpr int NUM_ITEMS = 50000;
    // Timeout for concurrent tests to prevent infinite hangs on broken implementations
    static constexpr int TIMEOUT_SECONDS = 10;
};

TEST_F(MPSCQueueSPSCTest, ProducerConsumerCorrectness) {
    MPSCQueue<int> queue;
    std::vector<int> received;
    received.reserve(NUM_ITEMS);
    std::atomic<bool> producer_done{false};
    
    std::thread producer([&queue, &producer_done]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            queue.enqueue(i);
        }
        producer_done = true;
    });
    
    // Consumer in main thread with timeout
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    constexpr int MAX_EMPTY_SPINS = 1000000;  // Fail fast if nothing is being dequeued
    
    while (received.size() < static_cast<size_t>(NUM_ITEMS)) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            received.push_back(result.value());
            empty_spins = 0;
        } else {
            ++empty_spins;
            // If producer is done and we keep getting empty, something is wrong
            if (producer_done && empty_spins > MAX_EMPTY_SPINS) {
                break;
            }
        }
        
        // Timeout check
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            FAIL() << "Test timed out after " << TIMEOUT_SECONDS << " seconds. "
                   << "Received " << received.size() << "/" << NUM_ITEMS << " items. "
                   << "This likely means enqueue() or dequeue() is not implemented.";
        }
        
        std::this_thread::yield();
    }
    
    producer.join();
    
    // Verify we got all items
    ASSERT_EQ(received.size(), static_cast<size_t>(NUM_ITEMS)) 
        << "Did not receive all items. Implementation may be incomplete.";
    
    // Verify strict FIFO order for single producer
    for (int i = 0; i < NUM_ITEMS; ++i) {
        EXPECT_EQ(received[i], i) << "Order violation at index " << i;
    }
}

// ============================================================================
// Concurrent Tests - Multiple Producers Single Consumer
// ============================================================================

class MPSCQueueMPSCTest : public ::testing::Test {
protected:
    static constexpr int NUM_PRODUCERS = 4;
    static constexpr int ITEMS_PER_PRODUCER = 10000;
    static constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;
    static constexpr int TIMEOUT_SECONDS = 30;
};

TEST_F(MPSCQueueMPSCTest, AllItemsReceived) {
    MPSCQueue<int> queue;
    std::vector<std::thread> producers;
    std::set<int> received;
    std::atomic<int> producers_done{0};
    
    // Each producer enqueues unique values: [p*ITEMS_PER_PRODUCER, (p+1)*ITEMS_PER_PRODUCER)
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &producers_done, p]() {
            int base = p * ITEMS_PER_PRODUCER;
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                queue.enqueue(base + i);
            }
            ++producers_done;
        });
    }
    
    // Consumer with timeout
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    constexpr int MAX_EMPTY_SPINS = 1000000;
    
    while (received.size() < static_cast<size_t>(TOTAL_ITEMS)) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            // Check for duplicates
            ASSERT_EQ(received.count(result.value()), 0) 
                << "Duplicate value: " << result.value();
            received.insert(result.value());
            empty_spins = 0;
        } else {
            ++empty_spins;
            if (producers_done == NUM_PRODUCERS && empty_spins > MAX_EMPTY_SPINS) {
                break;
            }
        }
        
        // Timeout check
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            // Join threads before failing
            for (auto& t : producers) {
                t.join();
            }
            FAIL() << "Test timed out after " << TIMEOUT_SECONDS << " seconds. "
                   << "Received " << received.size() << "/" << TOTAL_ITEMS << " items.";
        }
        
        std::this_thread::yield();
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    // Verify we got all items
    EXPECT_EQ(received.size(), static_cast<size_t>(TOTAL_ITEMS));
    
    // Verify all expected values are present
    for (int i = 0; i < TOTAL_ITEMS; ++i) {
        EXPECT_EQ(received.count(i), 1) << "Missing value: " << i;
    }
}

TEST_F(MPSCQueueMPSCTest, PerProducerFIFOOrder) {
    // While global order isn't guaranteed, each producer's items should
    // appear in FIFO order relative to each other
    MPSCQueue<std::pair<int, int>> queue;  // (producer_id, sequence)
    std::vector<std::thread> producers;
    std::atomic<int> producers_done{0};
    
    std::vector<int> last_seen(NUM_PRODUCERS, -1);
    std::atomic<int> total_received{0};
    
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &producers_done, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                queue.enqueue({p, i});
            }
            ++producers_done;
        });
    }
    
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    constexpr int MAX_EMPTY_SPINS = 1000000;
    
    while (total_received < TOTAL_ITEMS) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            auto [producer_id, seq] = result.value();
            // Each producer's items must come in order
            EXPECT_GT(seq, last_seen[producer_id]) 
                << "FIFO violation for producer " << producer_id
                << ": got " << seq << " after " << last_seen[producer_id];
            last_seen[producer_id] = seq;
            ++total_received;
            empty_spins = 0;
        } else {
            ++empty_spins;
            if (producers_done == NUM_PRODUCERS && empty_spins > MAX_EMPTY_SPINS) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            for (auto& t : producers) {
                t.join();
            }
            FAIL() << "Test timed out. Received " << total_received.load() << "/" << TOTAL_ITEMS;
        }
        
        std::this_thread::yield();
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    EXPECT_EQ(total_received.load(), TOTAL_ITEMS);
}

TEST_F(MPSCQueueMPSCTest, SumIntegrity) {
    // Verify no data corruption by checking sum
    MPSCQueue<long> queue;
    std::vector<std::thread> producers;
    std::atomic<int> producers_done{0};
    
    std::atomic<long> expected_sum{0};
    std::atomic<long> actual_sum{0};
    std::atomic<int> items_received{0};
    
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        producers.emplace_back([&queue, &expected_sum, &producers_done, p]() {
            long local_sum = 0;
            long base = static_cast<long>(p) * ITEMS_PER_PRODUCER;
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                long value = base + i;
                queue.enqueue(value);
                local_sum += value;
            }
            expected_sum.fetch_add(local_sum, std::memory_order_relaxed);
            ++producers_done;
        });
    }
    
    auto start = std::chrono::steady_clock::now();
    int empty_spins = 0;
    constexpr int MAX_EMPTY_SPINS = 1000000;
    
    // Consumer
    while (items_received < TOTAL_ITEMS) {
        auto result = queue.dequeue();
        if (result.has_value()) {
            actual_sum.fetch_add(result.value(), std::memory_order_relaxed);
            ++items_received;
            empty_spins = 0;
        } else {
            ++empty_spins;
            if (producers_done == NUM_PRODUCERS && empty_spins > MAX_EMPTY_SPINS) {
                break;
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(TIMEOUT_SECONDS)) {
            for (auto& t : producers) {
                t.join();
            }
            FAIL() << "Test timed out. Received " << items_received.load() << "/" << TOTAL_ITEMS;
        }
        
        std::this_thread::yield();
    }
    
    for (auto& t : producers) {
        t.join();
    }
    
    EXPECT_EQ(items_received.load(), TOTAL_ITEMS);
    EXPECT_EQ(expected_sum.load(), actual_sum.load());
}
