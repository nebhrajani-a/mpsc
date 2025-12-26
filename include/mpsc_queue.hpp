#ifndef MPSC_QUEUE_HPP
#define MPSC_QUEUE_HPP

#include <atomic>
#include <optional>

/**
 * Lock-Free Multi-Producer Single-Consumer (MPSC) Queue
 *
 * A high-performance concurrent queue that allows multiple producer threads
 * to enqueue items while a single consumer thread dequeues them.
 *
 * Design based on Dmitry Vyukov's intrusive MPSC queue:
 * http://www.1024cores.net/home/lock-free-algorithms/queues/intrusive-mpsc-node-based-queue
 *
 * Key properties:
 * - Lock-free for producers (wait-free enqueue)
 * - Wait-free for consumer (single consumer assumption)
 * - FIFO ordering per producer (global order interleaved)
 * - Uses acquire-release semantics for synchronization
 *
 * Memory ordering rationale:
 * - tail_.exchange uses acq_rel: release publishes the new node to other
 *   producers, acquire ensures we see the previous tail fully constructed
 * - prev->next.store uses release: ensures node data is visible before
 *   the consumer can follow the pointer
 * - head_->next.load uses acquire: synchronizes with producer's release
 *   store to see the node's data
 */
template <typename T>
class MPSCQueue {
public:
    struct Node {
        T data;
        std::atomic<Node*> next;

        Node() : data{}, next{nullptr} {}
        explicit Node(const T& value) : data{value}, next{nullptr} {}
        explicit Node(T&& value) : data{std::move(value)}, next{nullptr} {}
    };

private:
    // Head: only accessed by consumer, points to dummy/sentinel node
    Node* head_;

    // Tail: accessed by all producers via atomic operations
    std::atomic<Node*> tail_;

public:
    /**
     * Initialize queue with a dummy node.
     * The dummy simplifies edge cases by ensuring head != tail initially.
     */
    MPSCQueue() {
        Node* dummy = new Node();
        head_ = dummy;
        tail_.store(dummy, std::memory_order_relaxed);
    }

    ~MPSCQueue() {
        while (head_ != nullptr) {
            Node* next = head_->next.load(std::memory_order_relaxed);
            delete head_;
            head_ = next;
        }
    }

    // Non-copyable, non-movable
    MPSCQueue(const MPSCQueue&) = delete;
    MPSCQueue& operator=(const MPSCQueue&) = delete;
    MPSCQueue(MPSCQueue&&) = delete;
    MPSCQueue& operator=(MPSCQueue&&) = delete;

    /**
     * Enqueue an item. Thread-safe, lock-free.
     * Multiple producers can call this concurrently.
     */
    void enqueue(const T& value) {
        Node* node = new Node(value);
        Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    void enqueue(T&& value) {
        Node* node = new Node(std::move(value));
        Node* prev = tail_.exchange(node, std::memory_order_acq_rel);
        prev->next.store(node, std::memory_order_release);
    }

    /**
     * Dequeue an item. Single-consumer only.
     * Returns std::nullopt if queue is empty.
     *
     * Note: May briefly return nullopt during concurrent enqueue
     * (between tail exchange and next pointer store). The item
     * will be visible on the next dequeue call.
     */
    std::optional<T> dequeue() {
        Node* next = head_->next.load(std::memory_order_acquire);
        if (next == nullptr) {
            return std::nullopt;
        }
        T value = std::move(next->data);
        delete head_;
        head_ = next;
        return value;
    }

    /**
     * Check if queue appears empty.
     * Note: Result may be stale in concurrent context.
     */
    bool empty() const {
        return head_->next.load(std::memory_order_acquire) == nullptr;
    }
};

#endif // MPSC_QUEUE_HPP
