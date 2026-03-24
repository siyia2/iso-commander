// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "headers.h"

// ========================= LOCK-FREE QUEUE =========================
// Michael-Scott lock-free queue (no freelist, simple new/delete).
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        std::atomic<Node*> next;
        T data;
        Node() : next(nullptr) {}
        Node(T&& val) : next(nullptr), data(std::move(val)) {}
    };

    alignas(64) std::atomic<Node*> head;
    alignas(64) std::atomic<Node*> tail;

public:
    LockFreeQueue() {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    ~LockFreeQueue() {
        Node* node = head.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    void enqueue(T value) {
        Node* new_node = new Node(std::move(value));
        while (true) {
            Node* last = tail.load(std::memory_order_acquire);
            Node* next = last->next.load(std::memory_order_acquire);
            if (last != tail.load(std::memory_order_acquire))
                continue;
            if (next == nullptr) {
                if (last->next.compare_exchange_weak(next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    tail.compare_exchange_weak(last, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }
    }

    bool dequeue(T& result) {
        while (true) {
            Node* first = head.load(std::memory_order_acquire);
            Node* last  = tail.load(std::memory_order_acquire);
            Node* next  = first->next.load(std::memory_order_acquire);
            if (first != head.load(std::memory_order_acquire))
                continue;
            if (first == last) {
                if (next == nullptr)
                    return false;
                tail.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            } else {
                if (head.compare_exchange_weak(first, next,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {
                    result = std::move(next->data);
                    delete first;
                    return true;
                }
            }
        }
    }

    bool isEmpty() const {
        Node* h = head.load(std::memory_order_acquire);
        return h->next.load(std::memory_order_acquire) == nullptr;
    }
};


// ========================= THREAD POOL =========================
// Simplified pool for I/O-bound workloads (mounting/unmounting ISOs, etc.)
// - Single global lock‑free queue
// - All threads compete for the same queue
// - On enqueue, only wake threads when queue transitions from empty to non-empty

class ThreadPool {
private:
    // Cache-aligned atomic bool to prevent false sharing
    struct alignas(64) AtomicBool {
        std::atomic<bool> value;
        explicit AtomicBool(bool v = false) : value(v) {}
    };

    // Cache-aligned atomic size_t
    struct alignas(64) AtomicSize {
        std::atomic<size_t> value;
        explicit AtomicSize(size_t v = 0) : value(v) {}
    };

    std::vector<std::thread> workers;                 // Worker threads
    LockFreeQueue<std::function<void()>> task_queue;  // Global task queue

    std::mutex              mutex;      // Guards condition variable only
    std::condition_variable cv;         // For parking idle threads
    std::atomic<size_t>     sleeping_threads{0};  // Count of parked threads

    AtomicBool stop   {false};   // Signal to stop all threads

    const size_t          num_threads;      // Number of worker threads
    std::atomic<size_t>   active_tasks{0};  // Tasks currently running
    std::atomic<size_t>   pending_tasks{0}; // Tasks queued but not started

    // RAII guard for task execution tracking
    struct TaskGuard {
        std::atomic<size_t>& active;
        std::atomic<size_t>& pending;
        std::condition_variable& cv;

        TaskGuard(std::atomic<size_t>& a,
                  std::atomic<size_t>& p,
                  std::condition_variable& c)
            : active(a), pending(p), cv(c)
        {
            active.fetch_add(1, std::memory_order_release);
        }

        ~TaskGuard() {
            active.fetch_sub(1, std::memory_order_release);
            if (active.load(std::memory_order_acquire) == 0 &&
                pending.load(std::memory_order_acquire) == 0) {
                cv.notify_all();
            }
        }
    };

    // Main worker thread function
    void workerThread() {
        while (true) {
            std::function<void()> task;
            bool has_work = task_queue.dequeue(task);

            if (has_work) {
                // Found a task – execute it
                pending_tasks.fetch_sub(1, std::memory_order_release);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();  // Task may throw, but guard ensures proper cleanup
                continue;
            }

            // No work – check if we're stopping
            if (stop.value.load(std::memory_order_acquire)) {
                // Drain all remaining tasks before exiting
                while (task_queue.dequeue(task)) {
                    pending_tasks.fetch_sub(1, std::memory_order_release);
                    TaskGuard guard(active_tasks, pending_tasks, cv);
                    task();
                }
                return;
            }

            // One final check before sleeping – race condition prevention
            if (task_queue.dequeue(task)) {
                pending_tasks.fetch_sub(1, std::memory_order_release);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();
                continue;
            }

            // No work – go to sleep
            sleeping_threads.fetch_add(1, std::memory_order_relaxed);

            std::unique_lock<std::mutex> lock(mutex);

            // Double-check conditions before actually sleeping
            bool should_sleep = true;

            // Re-check the queue (might have become non‑empty)
            if (!task_queue.isEmpty()) {
                should_sleep = false;
            }

            // Re-check stop flag
            if (stop.value.load(std::memory_order_acquire)) {
                should_sleep = false;
            }

            if (should_sleep) {
                cv.wait(lock, [this] {
                    // Wake up if:
                    // 1. Pool is stopping
                    if (stop.value.load(std::memory_order_acquire))
                        return true;
                    // 2. There is work in the queue
                    return !task_queue.isEmpty();
                });
            }

            sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
            // Continue loop – will try to dequeue again
        }
    }

public:
    // Constructor: Create thread pool with specified number of threads
    explicit ThreadPool(size_t n) : num_threads(n) {
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i)
            workers.emplace_back(&ThreadPool::workerThread, this);
    }

    // Destructor: Clean shutdown of all threads
    ~ThreadPool() {
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();
        for (auto& t : workers)
            t.join();
    }

    // Enqueue a task and return a future for its result
    // Thread-safe, can be called from any thread
    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();

        // Update task counts
        pending_tasks.fetch_add(1, std::memory_order_release);

        // Check if queue is empty BEFORE enqueueing
        bool was_empty = task_queue.isEmpty();
        
        // Enqueue the task
        task_queue.enqueue([task]{ (*task)(); });

        // Only wake sleeping threads if we transitioned from empty to non-empty
        if (was_empty) {
            size_t sleeping = sleeping_threads.load(std::memory_order_acquire);
            if (sleeping > 0) {
                cv.notify_all();
            }
        }

        return res;
    }

    // Block until all submitted tasks have completed
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            auto pending = pending_tasks.load(std::memory_order_acquire);
            auto active = active_tasks.load(std::memory_order_acquire);
            return pending == 0 && active == 0;
        });
    }

    // Check if pool is completely idle (no pending or active tasks)
    bool isIdle() const {
        return pending_tasks.load(std::memory_order_acquire) == 0 &&
               active_tasks.load(std::memory_order_acquire) == 0;
    }

    // Get number of worker threads in pool
    size_t threadCount() const { return num_threads; }

    // Get number of tasks waiting to be processed
    size_t pendingCount() const {
        return pending_tasks.load(std::memory_order_acquire);
    }

    // Get number of tasks currently being executed
    size_t activeCount() const {
        return active_tasks.load(std::memory_order_acquire);
    }

    // Get number of threads currently sleeping (idle)
    size_t sleepingCount() const {
        return sleeping_threads.load(std::memory_order_acquire);
    }

    // Wait for all tasks to complete, then stop the pool
    void waitAndStop() {
        waitAllTasksCompleted();
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();
    }
};

#endif // THREAD_POOL_H
