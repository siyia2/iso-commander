#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"

// Define a macro for cache line size
#define CACHE_LINE_SIZE 64

// A global scalable lock-free thread pool for async tasks that includes work stealing
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(T value = T()) : data(std::move(value)), next(nullptr) {}
    };

    // Aligned structure to avoid false sharing
    struct alignas(CACHE_LINE_SIZE) AlignedAtomicNode {
        std::atomic<Node*> ptr;
        AlignedAtomicNode(Node* p = nullptr) : ptr(p) {}
        // Implement copy and move operations if needed
    };

    AlignedAtomicNode head;
    AlignedAtomicNode tail;

    static constexpr size_t POOL_SIZE = 4096;
    std::array<Node, POOL_SIZE> node_pool;
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> pool_index{0};

    // Allocate a node from a preallocated pool or dynamically
    Node* allocate_node(T value) {
        size_t index = pool_index.fetch_add(1, std::memory_order_relaxed);
        if (index < POOL_SIZE) {
            return new (&node_pool[index]) Node(std::move(value));
        } else {
            return new Node(std::move(value));
        }
    }

    // Deallocate a node either from the pool or dynamically
    void deallocate_node(Node* node) {
        size_t index = node - node_pool.data();
        if (index < POOL_SIZE) {
            node->~Node();  // Placement new was used, so call destructor explicitly
        } else {
            delete node;
        }
    }

public:
    LockFreeQueue() {
        Node* dummy = allocate_node(T());
        head.ptr.store(dummy, std::memory_order_relaxed);
        tail.ptr.store(dummy, std::memory_order_relaxed);
    }
    
    ~LockFreeQueue() {
        Node* current = head.ptr.load(std::memory_order_acquire);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            deallocate_node(current);
            current = next;
        }
    }

    // Enqueue operation
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

    // Dequeue operation
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

    // Steal operation (for work stealing thread pool)
    bool steal(T& result) {
        return dequeue(result);
    }
};

// Thread pool implementation
class ThreadPool {
private:
    struct alignas(CACHE_LINE_SIZE) AlignedAtomic {
        std::atomic<bool> value;
        AlignedAtomic(bool initial = false) : value(initial) {}
    };

    struct alignas(CACHE_LINE_SIZE) AlignedAtomicSize {
        std::atomic<size_t> value;
        AlignedAtomicSize(size_t initial = 0) : value(initial) {}
    };

public:
    explicit ThreadPool(size_t numThreads) 
        : stop(false), next_queue(0), num_threads(numThreads) {
        queues.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            queues.emplace_back(std::make_unique<LockFreeQueue<std::function<void()>>>());
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this, i);
        }
    }

    // Enqueue a task to be executed by the thread pool
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        
        size_t index = selectQueue();
        queues[index]->enqueue([task]() { (*task)(); });
        
        cv.notify_one();  // Notify one worker thread that a task is available
        return res;
    }

    ~ThreadPool() {
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();  // Notify all worker threads to stop

        for (std::thread& worker : workers) {
            worker.join();  // Join all worker threads
        }
    }

private:
    // Select a queue for task enqueueing
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
                task();  // Execute the task
            } else {
                // Wait for either stop or a non-empty queue
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(1), [this] { 
                    return stop.value.load(std::memory_order_acquire) || 
                           std::any_of(queues.begin(), queues.end(), 
                               [](const auto& q) { return !q->isEmpty(); });
                });
                
                // If all queues are empty and stop is true, exit the thread
                if (stop.value.load(std::memory_order_acquire) && 
                    std::all_of(queues.begin(), queues.end(), 
                        [](const auto& q) { return q->isEmpty(); })) {
                    return;
                }
            }
        }
    }

    // Determine the number of steal attempts based on the number of threads
    size_t adaptiveStealAttempts() {
        if (num_threads <= 4) return 1;
        if (num_threads <= 16) return num_threads / 2;
        if (num_threads <= 64) return num_threads - 1;
        return std::min(num_threads / 2, static_cast<size_t>(64));
    }

    std::vector<std::thread> workers;  // Worker threads
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues;  // Task queues
    std::mutex mutex;  // Mutex for condition variable
    std::condition_variable cv;  // Condition variable for synchronization
    AlignedAtomic stop;  // Atomic flag to signal threads to stop
    AlignedAtomicSize next_queue;  // Atomic counter for queue selection
    const size_t num_threads;  // Number of threads in the pool
};

#endif // THREAD_POOL_H
