// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "./headers.h"

// ========================= LOCK-FREE QUEUE =========================
// Michael-Scott lock-free queue (without node reuse to avoid ABA)
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

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

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
                        std::memory_order_acquire);
                    return;
                }
            } else {
                tail.compare_exchange_weak(last, next,
                    std::memory_order_release,
                    std::memory_order_acquire);
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
                    std::memory_order_acquire);
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
class ThreadPool {
private:
    // ---- Fixed declaration order to match initializer list ----
    const size_t num_threads;                        // must come first
    std::vector<std::function<void()>> func_pool;    // second (kept for compatibility, not used)
    std::atomic<size_t> sleeping_threads;            // third

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    mutable std::mutex mutex;
    std::condition_variable cv;

    std::atomic<bool> stop;
    std::atomic<size_t> pending_tasks;
    std::atomic<size_t> active_tasks;

    // Helper struct to track active tasks
    struct TaskGuard {
        std::atomic<size_t>& active;
        std::atomic<size_t>& pending;
        std::condition_variable& cv;

        TaskGuard(std::atomic<size_t>& a,
                  std::atomic<size_t>& p,
                  std::condition_variable& c)
            : active(a), pending(p), cv(c) {
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

    void workerThread() {
        while (true) {
            std::function<void()> task;

            // Try to dequeue a task
            if (task_queue.dequeue(task)) {
                pending_tasks.fetch_sub(1, std::memory_order_release);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();
                continue;
            }

            // Check stop flag before going to sleep
            if (stop.load(std::memory_order_acquire)) {
                // Process any remaining tasks before exiting
                while (task_queue.dequeue(task)) {
                    pending_tasks.fetch_sub(1, std::memory_order_release);
                    TaskGuard guard(active_tasks, pending_tasks, cv);
                    task();
                }
                return;
            }

            // Go to sleep
            {
                std::unique_lock<std::mutex> lock(mutex);
                
                // Double-check stop flag after acquiring mutex to avoid missing
                // a shutdown signal that arrived between the previous check and lock acquisition
                if (stop.load(std::memory_order_acquire)) {
                    // Process remaining tasks before exit
                    while (task_queue.dequeue(task)) {
                        pending_tasks.fetch_sub(1, std::memory_order_release);
                        TaskGuard guard(active_tasks, pending_tasks, cv);
                        task();
                    }
                    return;
                }
                
                // Use relaxed memory order for sleeping_threads since it's only used for monitoring
                // and approximate counts are sufficient for performance optimizations
                sleeping_threads.fetch_add(1, std::memory_order_relaxed);
                
                // Wait until either:
                // - New task arrives (queue not empty)
                // - Shutdown initiated
                cv.wait(lock, [this] {
                    return !task_queue.isEmpty() || 
                           stop.load(std::memory_order_acquire);
                });
                
                sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
                
                // After waking up, check if we should exit
                // Exit only if stop is true AND queue is empty
                if (stop.load(std::memory_order_acquire) && task_queue.isEmpty()) {
                    return;
                }
            }
        }
    }

public:
    explicit ThreadPool(size_t n)
        : num_threads(n),
          func_pool(0),
          sleeping_threads(0),
          stop(false),
          pending_tasks(0),
          active_tasks(0)
    {
        if (n == 0) {
            throw std::invalid_argument("ThreadPool: number of threads must be greater than 0");
        }
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    ~ThreadPool() {
        // First, wait for all pending and active tasks to complete
        // This ensures no tasks are lost when the thread pool is destroyed
        // Note: This can deadlock if tasks have circular dependencies waiting on each other
        waitAllTasksCompleted();
        
        // Signal all threads to stop
        stop.store(true, std::memory_order_release);
        
        // Wake up all threads to check the stop flag
        cv.notify_all();
        
        // Wait for all threads to finish
        for (auto& t : workers) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        std::future<return_type> result = task->get_future();

        pending_tasks.fetch_add(1, std::memory_order_release);
        task_queue.enqueue([task]() { (*task)(); });

        // Optimized wake-up strategy:
        // - If all threads are sleeping, wake them all to distribute work better
        // - Otherwise, wake just one thread to avoid unnecessary context switches
        if (sleeping_threads.load(std::memory_order_relaxed) == num_threads) {
            cv.notify_all();
        } else {
            cv.notify_one();
        }
        
        return result;
    }

    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return pending_tasks.load(std::memory_order_acquire) == 0 &&
                   active_tasks.load(std::memory_order_acquire) == 0;
        });
    }

    bool isIdle() const {
        return pending_tasks.load(std::memory_order_acquire) == 0 &&
               active_tasks.load(std::memory_order_acquire) == 0;
    }

    size_t threadCount() const { 
        return num_threads; 
    }
    
    size_t pendingCount() const { 
        return pending_tasks.load(std::memory_order_acquire); 
    }
    
    size_t activeCount() const { 
        return active_tasks.load(std::memory_order_acquire); 
    }

    size_t sleepingCount() const {
        // Relaxed memory order is sufficient for monitoring
        return sleeping_threads.load(std::memory_order_relaxed);
    }

    void waitAndStop() {
        waitAllTasksCompleted();
        stop.store(true, std::memory_order_release);
        cv.notify_all();
    }
};

#endif // THREAD_POOL_H
