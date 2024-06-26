#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"

template <typename T>
class LockFreeQueue {
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;

    // Node structure for the queue
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(T value = T()) : data(std::move(value)), next(nullptr) {}
    };

    // AlignedAtomicNode structure for aligned atomic operations
    struct alignas(CACHE_LINE_SIZE) AlignedAtomicNode {
        std::atomic<Node*> ptr;
        AlignedAtomicNode(Node* p = nullptr) : ptr(p) {}
    };

    // Pool size and node pool for memory management
    size_t pool_size;
    std::vector<Node> node_pool;
    alignas(CACHE_LINE_SIZE) AlignedAtomicNode head;
    alignas(CACHE_LINE_SIZE) AlignedAtomicNode tail;
    std::atomic<size_t> pool_index; // Tracks next index in node_pool

    // Calculate pool size based on number of threads
    static size_t calculatePoolSize(size_t num_threads) {
        if (num_threads <= 16) return 1024;
        if (num_threads <= 32) return 2048;
        if (num_threads <= 64) return 4092;
        return 8192;
    }

    // Allocate a node from the pool or dynamically
    Node* allocate_node(T value) {
        size_t index = pool_index.fetch_add(1, std::memory_order_relaxed);
        if (index < pool_size) {
            return new (&node_pool[index]) Node(std::move(value));
        } else {
            return new Node(std::move(value));
        }
    }

    // Deallocate a node
    void deallocate_node(Node* node) {
        ptrdiff_t index = node - &node_pool[0];
        if (index >= 0 && index < static_cast<ptrdiff_t>(pool_size)) {
            node->~Node();
        } else {
            delete node;
        }
    }

public:
    // Constructor initializes the queue with a dummy node
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

    // Destructor cleans up all nodes in the queue
    ~LockFreeQueue() {
        Node* current = head.ptr.load(std::memory_order_acquire);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            deallocate_node(current);
            current = next;
        }
    }

    // Enqueue an item into the queue
    void enqueue(T value) {
        Node* newNode = allocate_node(std::move(value));
        while (true) {
            Node* oldTail = tail.ptr.load(std::memory_order_acquire);
            Node* next = oldTail->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                if (oldTail->next.compare_exchange_weak(next, newNode,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed)) {
                    tail.ptr.compare_exchange_strong(oldTail, newNode,
                                                     std::memory_order_release,
                                                     std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.ptr.compare_exchange_strong(oldTail, next,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
            }
        }
    }

    // Dequeue an item from the queue
    bool dequeue(T& result) {
        while (true) {
            Node* oldHead = head.ptr.load(std::memory_order_acquire);
            Node* next = oldHead->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return false;
            }
            if (head.ptr.compare_exchange_weak(oldHead, next,
                                               std::memory_order_release,
                                               std::memory_order_relaxed)) {
                result = std::move(next->data);
                deallocate_node(oldHead);
                return true;
            }
        }
    }

    // Check if the queue is empty
    bool isEmpty() const {
        return head.ptr.load(std::memory_order_acquire)->next.load(std::memory_order_acquire) == nullptr;
    }

    // Steal operation for work stealing
    bool steal(T& result) {
        return dequeue(result);
    }
};

class ThreadPool {
private:
    // AlignedAtomic structure for aligned atomic operations
    struct alignas(64) AlignedAtomic {
        std::atomic<bool> value;
        AlignedAtomic(bool initial = false) : value(initial) {}
    };

    // AlignedAtomicSize structure for aligned atomic size operations
    struct alignas(64) AlignedAtomicSize {
        std::atomic<size_t> value;
        AlignedAtomicSize(size_t initial = 0) : value(initial) {}
    };

    std::vector<std::thread> workers; // Worker threads
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues; // Queues for tasks
    std::mutex mutex; // Mutex for synchronization
    std::condition_variable cv; // Condition variable for task waiting
    AlignedAtomic stop; // Atomic flag to stop threads
    AlignedAtomicSize next_queue; // Next queue index for task distribution
    const size_t num_threads; // Number of threads

    // Select a queue index for task distribution
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

        while (true) {
            std::function<void()> task;
            bool gotTask = queues[id]->dequeue(task);

            if (!gotTask) {
                size_t steal_attempts = adaptiveStealAttempts();
                for (size_t i = 0; i < steal_attempts; ++i) {
                    size_t victim = dist(rng);
                    if (victim != id && queues[victim]->steal(task)) {
                        gotTask = true;
                        break;
                    }
                }
            }

            if (gotTask) {
                task();
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

    // Calculate adaptive number of steal attempts
    size_t adaptiveStealAttempts() {
        if (num_threads <= 16) return num_threads / 2;
        if (num_threads <= 64) return num_threads / 4;
        return std::min(num_threads / 8, static_cast<size_t>(24));
    }

public:
    // Constructor initializes the thread pool with queues and worker threads
    explicit ThreadPool(size_t numThreads) 
        : stop(false), next_queue(0), num_threads(numThreads) {
        queues.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            queues.emplace_back(std::make_unique<LockFreeQueue<std::function<void()>>>(numThreads));
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this, i);
        }
    }

    // Enqueue a task into a selected queue
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();

        size_t index = selectQueue();
        queues[index]->enqueue([task]() { (*task)(); });

        cv.notify_one();
        return res;
    }

    // Destructor stops threads and cleans up
    ~ThreadPool() {
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();

        for (std::thread& worker : workers) {
            worker.join();
        }
    }
};



#endif // THREAD_POOL_H
