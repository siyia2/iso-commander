// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "./headers.h"

/**
 * @brief A thread-safe, non-blocking Michael-Scott Lock-Free Queue.
 * * @details This implementation utilizes C++20 atomic shared_ptr to eliminate the 
 * classic ABA problem without requiring a complex garbage collector. 
 * * Key Robustness Features:
 * 1. Data Handoff: Uses std::unique_ptr internally to ensure that memory ownership 
 * of the task is transferred atomically. Only the thread that successfully 
 * swings the 'head' pointer via CAS takes ownership of the task data, 
 * preventing double-moves or heap corruption.
 * 2. Tail-Helping: Includes logic to allow threads to "help" move the tail 
 * pointer forward if it lags behind, preventing a single stalled thread 
 * from blocking the entire queue.
 * * @tparam T The type of elements stored in the queue (typically std::function).
 */
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        std::unique_ptr<T> data;
        std::atomic<std::shared_ptr<Node>> next;
        Node() : data(nullptr), next(nullptr) {}
        Node(T&& val) : data(std::make_unique<T>(std::move(val))), next(nullptr) {}
    };

    alignas(64) std::atomic<std::shared_ptr<Node>> head;
    alignas(64) std::atomic<std::shared_ptr<Node>> tail;

public:
    /** @brief Initializes the queue with a dummy node to simplify boundary conditions. */
    LockFreeQueue() {
        auto dummy = std::make_shared<Node>();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    /**
     * @brief Atomically enqueues an item.
     * @param value The item to move into the queue.
     */
    void enqueue(T value) {
        auto new_node = std::make_shared<Node>(std::move(value));
        while (true) {
            auto last = tail.load(std::memory_order_acquire);
            auto next = last->next.load(std::memory_order_acquire);

            if (last == tail.load(std::memory_order_acquire)) {
                if (next == nullptr) {
                    // Try to link the new node to the end of the list
                    if (last->next.compare_exchange_weak(next, new_node, 
                        std::memory_order_release, std::memory_order_relaxed)) {
                        
                        // Link successful; try to swing tail to the new node
                        tail.compare_exchange_weak(last, new_node, std::memory_order_release);
                        return;
                    }
                } else {
                    // Tail is lagging; help move it forward so other threads aren't blocked
                    tail.compare_exchange_weak(last, next, std::memory_order_release);
                }
            }
        }
    }

    /**
     * @brief Atomically dequeues an item.
     * @param result Reference where the popped item will be moved.
     * @return True if an item was retrieved; false if the queue was empty.
     */
    bool dequeue(T& result) {
        while (true) {
            auto first = head.load(std::memory_order_acquire);
            auto last = tail.load(std::memory_order_acquire);
            auto next = first->next.load(std::memory_order_acquire);

            if (first == head.load(std::memory_order_acquire)) {
                if (first == last) {
                    if (next == nullptr) return false; // Queue is empty
                    
                    // Tail is lagging behind head; help move it forward
                    tail.compare_exchange_weak(last, next, std::memory_order_release);
                } else {
                    // Pre-read data; ownership is only confirmed if CAS succeeds
                    if (head.compare_exchange_weak(first, next, std::memory_order_acq_rel)) {
                        if (next->data) {
                            result = std::move(*(next->data));
                            next->data.reset(); // Clear node memory immediately
                        }
                        return true;
                    }
                }
            }
        }
    }
};

/**
 * @brief High-performance ThreadPool using lock-free task management.
 * * @details This pool manages a set of worker threads that consume tasks from a 
 * LockFreeQueue. It utilizes a 64-bit atomic state bitfield (32-bit pending, 
 * 32-bit active) to track workload without the overhead of heavy mutex locking.
 * * Design Features:
 * 1. Automatic Lifetime: Designed as a Meyers Singleton to ensure background 
 * threads are joined before global object destruction.
 * 2. RAII Shutdown: Destructor ensures all tasks are completed and workers 
 * joined, preventing "zombie" threads from accessing a stale heap.
 * 3. Cache Alignment: Core atomics are aligned to 64-byte boundaries to 
 * eliminate False Sharing performance degradation.
 */
class ThreadPool {
private:
    const size_t num_threads;
    
    // Bits 63-32: Number of tasks in queue. Bits 31-0: Number of tasks currently running.
    alignas(64) std::atomic<uint64_t> task_state{0}; 

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    alignas(64) std::atomic<size_t> sleeping_threads{0};

    /** @brief Extracts the number of pending tasks from the packed 64-bit state. */
    uint64_t pendingFromState(uint64_t s) const { return s >> 32; }
    
    /** @brief Notifies 'waitAllTasksCompleted' if the pool has just finished the last task. */
    void notifyIfIdle(uint64_t prevState) {
        if (prevState == ACTIVE_ONE) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }
    }

    /** @brief Internal wrapper to manage task state transitions and execution. */
    void runTask(std::function<void()>& task) {
        // Transition: decrement pending count, increment active count
        task_state.fetch_add(static_cast<uint64_t>(-PENDING_ONE) + ACTIVE_ONE, std::memory_order_acq_rel);
        
        if (task) {
            try { task(); } catch (...) { /* Prevent exception propagation into worker thread */ }
        }

        // Transition: decrement active count
        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);
        notifyIfIdle(prev);
    }

    /** @brief Main loop for worker threads. */
    void workerThread() {
        while (true) {
            std::function<void()> task;

            if (task_queue.dequeue(task)) {
                runTask(task);
                continue;
            }

            if (stop.load(std::memory_order_acquire)) return;

            // Fallback to sleep if queue is empty
            {
                std::unique_lock<std::mutex> lock(mutex);
                sleeping_threads.fetch_add(1, std::memory_order_relaxed);
                cv.wait(lock, [this] {
                    return pendingFromState(task_state.load(std::memory_order_acquire)) > 0 || stop.load(std::memory_order_acquire);
                });
                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

public:
    /**
     * @brief Construct a new Thread Pool and spawns workers.
     * @param n Number of worker threads to maintain.
     */
    explicit ThreadPool(size_t n) : num_threads(n) {
        if (n == 0) throw std::invalid_argument("ThreadPool: n > 0 required");
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    /**
     * @brief Destructor. Blocks until all pending tasks are finished and workers are joined.
     */
    ~ThreadPool() {
        waitAllTasksCompleted();
        shutdown();
    }

    /**
     * @brief Submits a task to the pool.
     * @tparam F Function type.
     * @tparam Args Argument types.
     * @return std::future for retrieving the task result.
     */
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            [func = std::forward<F>(f),
             tup  = std::make_tuple(std::forward<Args>(args)...)]() mutable {
                return std::apply(std::move(func), std::move(tup));
            });

        std::future<return_type> result = task->get_future();

        task_state.fetch_add(PENDING_ONE, std::memory_order_release);
        
        task_queue.enqueue([task]() { 
            if (task && task->valid()) { 
                (*task)(); 
            }
        });

        // Wake a thread if any are sleeping
        if (sleeping_threads.load(std::memory_order_relaxed) > 0) {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_one();
        }

        return result;
    }

    /** @brief Blocks the calling thread until the queue is empty and all workers are idle. */
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return task_state.load(std::memory_order_acquire) == 0;
        });
    }
    
    /** * @brief Signals workers to exit and joins all threads. 
     * This is safe to call multiple times.
     */
    void shutdown() {
        if (stop.exchange(true, std::memory_order_acq_rel)) return;

        {
            std::lock_guard<std::mutex> lock(mutex);
            cv.notify_all();
        }

        for (auto& t : workers) {
            if (t.joinable()) t.join();
        }
        
        // Clear any orphaned tasks
        std::function<void()> residual;
        while (task_queue.dequeue(residual)) { residual = nullptr; }
    }

    /** @brief Returns true if no tasks are pending or active. */
    bool isIdle() const { return task_state.load(std::memory_order_acquire) == 0; }
    
    /** @brief Returns the total number of worker threads. */
    size_t threadCount() const { return num_threads; }
};

/**
 * @brief Thread-safe retrieval of the global ThreadPool singleton.
 * @details Uses a local static (Meyers Singleton) to ensure thread-safe 
 * initialization and a guaranteed destruction order relative to other global objects.
 */
inline ThreadPool& getStaticThreadPool() {
    static ThreadPool instance(std::min({static_cast<size_t>(maxThreads), MAX_USEFUL_THREADS}));
    return instance;
}

#endif // THREAD_POOL_H
