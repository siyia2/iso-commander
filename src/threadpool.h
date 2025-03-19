// SPDX-License-Identifier: LGPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "headers.h"


// A global lock-free threadpool for async tasks with work-stealing scalable from 1 to 192 threads
template <typename T>
class LockFreeQueue {
private:
    // Cache line size to avoid false sharing
    static constexpr size_t CACHE_LINE_SIZE = 64;

    // Node structure to hold data and next pointer
    struct Node {
        T data;
        std::atomic<Node*> next;
        std::atomic<uint64_t> timestamp;
        Node(T value = T()) : data(std::move(value)), next(nullptr), timestamp(0) {}
    };

    // Structure to align atomic pointers to cache lines
    struct alignas(CACHE_LINE_SIZE) AlignedAtomicNode {
        std::atomic<Node*> ptr;
        AlignedAtomicNode(Node* p = nullptr) : ptr(p) {}
    };

    size_t pool_size;                          // Size of the node pool
    std::vector<Node> node_pool;               // Pool of preallocated nodes
    alignas(CACHE_LINE_SIZE) AlignedAtomicNode head; // Head pointer of the queue
    alignas(CACHE_LINE_SIZE) AlignedAtomicNode tail; // Tail pointer of the queue
    std::atomic<size_t> pool_index;            // Index to track allocation in the node pool

    // Calculate pool size based on number of threads
    static size_t calculatePoolSize(size_t num_threads) {
        return std::min(num_threads * 128, static_cast<size_t>(16384));
    }

    // Allocate a new node, either from the pool or dynamically
    Node* allocate_node(T value) {
        size_t index = pool_index.fetch_add(1, std::memory_order_relaxed);
        if (index < pool_size) {
            Node* node = new (&node_pool[index]) Node(std::move(value));
            node->timestamp.store(1, std::memory_order_relaxed);
            return node;
        } else {
            Node* node = new Node(std::move(value));
            node->timestamp.store(1, std::memory_order_relaxed);
            return node;
        }
    }

    // Deallocate a node, either back to the pool or by deleting
    void deallocate_node(Node* node) {
        auto pool_begin = &node_pool[0];
        auto pool_end = pool_begin + pool_size;
        if (node >= pool_begin && node < pool_end) {
            node->~Node();
            node->timestamp.fetch_add(1, std::memory_order_relaxed);
        } else {
            delete node;
        }
    }

public:
    // Constructor to initialize the queue with a dummy node
    explicit LockFreeQueue(size_t num_threads)
        : pool_size(calculatePoolSize(num_threads)),
          node_pool(pool_size),
          head(nullptr),
          tail(nullptr),
          pool_index(0)
    {
        Node* dummy = allocate_node(T());
        head.ptr.store(dummy, std::memory_order_relaxed);
        tail.ptr.store(dummy, std::memory_order_relaxed);
    }

    // Destructor to clean up the nodes
    ~LockFreeQueue() {
        Node* current = head.ptr.load(std::memory_order_acquire);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            deallocate_node(current);
            current = next;
        }
        for (size_t i = 0; i < pool_size; ++i) {
            node_pool[i].~Node();
        }
    }

    // Enqueue a new item into the queue
    void enqueue(T value) {
        Node* new_node = allocate_node(std::move(value));
        while (true) {
            Node* old_tail = tail.ptr.load(std::memory_order_acquire);
            Node* next = old_tail->next.load(std::memory_order_acquire);
            uint64_t timestamp = old_tail->timestamp.load(std::memory_order_relaxed);
            if (next == nullptr) {
                if (old_tail->next.compare_exchange_weak(next, new_node,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed) &&
                    old_tail->timestamp.compare_exchange_weak(timestamp, timestamp + 1,
                                                              std::memory_order_release,
                                                              std::memory_order_relaxed)) {
                    tail.ptr.compare_exchange_strong(old_tail, new_node,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.ptr.compare_exchange_strong(old_tail, next,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
            }
        }
    }

    // Dequeue an item from the queue
    bool dequeue(T& result) {
        while (true) {
            Node* old_head = head.ptr.load(std::memory_order_acquire);
            Node* next = old_head->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return false;
            }
            if (head.ptr.compare_exchange_weak(old_head, next,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
                result = std::move(next->data);
                deallocate_node(old_head);
                return true;
            }
        }
    }

    // Check if the queue is empty
    bool isEmpty() const {
        return head.ptr.load(std::memory_order_acquire)->next.load(std::memory_order_acquire) == nullptr;
    }

    // Steal an item from the queue (same as dequeue)
    bool steal(T& result) {
        return dequeue(result);
    }

    // Enqueue a batch of items into the queue
    template <typename InputIt>
    void enqueue_batch(InputIt first, InputIt last) {
        std::vector<Node*> new_nodes;
        for (auto it = first; it != last; ++it) {
            new_nodes.push_back(allocate_node(std::move(*it)));
        }

        if (new_nodes.empty()) return;

        while (true) {
            Node* old_tail = tail.ptr.load(std::memory_order_acquire);
            Node* next = old_tail->next.load(std::memory_order_acquire);
            uint64_t timestamp = old_tail->timestamp.load(std::memory_order_relaxed);

            if (next == nullptr) {
                for (size_t i = 0; i < new_nodes.size() - 1; ++i) {
                    new_nodes[i]->next.store(new_nodes[i + 1], std::memory_order_relaxed);
                }

                if (old_tail->next.compare_exchange_weak(next, new_nodes[0],
                                                         std::memory_order_release,
                                                         std::memory_order_relaxed) &&
                    old_tail->timestamp.compare_exchange_weak(timestamp, timestamp + 1,
                                                              std::memory_order_release,
                                                              std::memory_order_relaxed)) {
                    tail.ptr.compare_exchange_strong(old_tail, new_nodes.back(),
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.ptr.compare_exchange_strong(old_tail, next,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
            }
        }
    }

    // Dequeue a batch of items from the queue
    template <typename OutputIt>
    size_t dequeue_batch(OutputIt out, size_t max_items) {
        size_t dequeued = 0;
        while (dequeued < max_items) {
            Node* old_head = head.ptr.load(std::memory_order_acquire);
            Node* next = old_head->next.load(std::memory_order_acquire);

            if (next == nullptr) {
                return dequeued;
            }

            if (head.ptr.compare_exchange_weak(old_head, next,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
                *out++ = std::move(next->data);
                deallocate_node(old_head);
                ++dequeued;
            }
        }
        return dequeued;
    }
};

class ThreadPool {
private:
    // Atomic boolean with cache line alignment to avoid false sharing
    struct alignas(64) AlignedAtomic {
        std::atomic<bool> value;
        AlignedAtomic(bool initial = false) : value(initial) {}
    };

    // Atomic size_t with cache line alignment to avoid false sharing
    struct alignas(64) AlignedAtomicSize {
        std::atomic<size_t> value;
        AlignedAtomicSize(size_t initial = 0) : value(initial) {}
    };

    std::vector<std::thread> workers; // Worker threads
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues; // Queues for each thread
    std::mutex mutex; // Mutex for condition variable
    std::condition_variable cv; // Condition variable for synchronization
    AlignedAtomic stop; // Atomic flag to stop the thread pool
    AlignedAtomicSize next_queue; // Index to select next queue for task
    const size_t num_threads; // Number of threads in the pool
    std::atomic<size_t> active_tasks; // Counter for active tasks
    static constexpr size_t BATCH_SIZE = 32; // Batch size for notification
    std::atomic<size_t> enqueued_tasks{0}; // Counter for enqueued tasks

    // Select a queue to enqueue a new task
    size_t selectQueue() {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<size_t> dist(0, num_threads - 1);

        size_t base = next_queue.value.fetch_add(1, std::memory_order_relaxed) % num_threads;
        size_t random_offset = dist(rng) % (num_threads / 4 + 1);
        return (base + random_offset) % num_threads;
    }

    // Worker thread function
    void workerThread(size_t id) {
        std::mt19937 rng(id);
        std::uniform_int_distribution<size_t> dist(0, num_threads - 1);
        std::exponential_distribution<> exp_dist(1.0);

        while (true) {
            std::function<void()> task;
            bool got_task = queues[id]->dequeue(task);

            if (!got_task) {
                size_t steal_attempts = adaptiveStealAttempts();
                for (size_t i = 0; i < steal_attempts; ++i) {
                    size_t victim = dist(rng);
                    if (victim != id && queues[victim]->steal(task)) {
                        got_task = true;
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(
                        static_cast<size_t>(exp_dist(rng))));
                }
            }

            if (got_task) {
                ++active_tasks;
                task();
                --active_tasks;
            } else {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(1), [this] {
                    return stop.value.load(std::memory_order_acquire) ||
                           std::any_of(queues.begin(), queues.end(),
                               [](const auto& q) { return !q->isEmpty(); });
                });

                if (stop.value.load(std::memory_order_acquire) &&
                    std::all_of(queues.begin(), queues.end(),
                        [](const auto& q) { return q->isEmpty(); })) {
                    return;
                }
            }
        }
    }

    // Calculate adaptive steal attempts based on number of threads
    size_t adaptiveStealAttempts() {
        if (num_threads <= 2) return 1;
        return std::min(num_threads / 2, static_cast<size_t>(64));
    }

public:
    // Constructor to initialize the thread pool
    explicit ThreadPool(size_t numThreads)
        : stop(false), next_queue(0), num_threads(numThreads), active_tasks(0) {
        queues.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            queues.emplace_back(std::make_unique<LockFreeQueue<std::function<void()>>>(numThreads));
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this, i);
        }
    }

    // Enqueue a task into the pool and return a future for its result
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();

        size_t index = selectQueue();
        queues[index]->enqueue([task]() { (*task)(); });

        if (++enqueued_tasks % BATCH_SIZE == 0) {
            cv.notify_all();
        }

        return res;
    }

    // Destructor to clean up the threads and queues
    ~ThreadPool() {
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();

        for (std::thread& worker : workers) {
            worker.join();
        }
    }

    // Wait for all tasks to complete
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return active_tasks.load(std::memory_order_acquire) == 0 &&
                   std::all_of(queues.begin(), queues.end(),
                               [](const auto& q) { return q->isEmpty(); });
        });
    }
};

#endif // THREAD_POOL_H
