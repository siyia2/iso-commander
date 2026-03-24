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
        // Delete all nodes still in the queue
        Node* node = head.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    // Disable copy/move
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
                    // Recycle the old dummy node
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
// Optimized pool for I/O-bound workloads with correct sleeping semantics
class ThreadPool {
public:
    explicit ThreadPool(size_t n) 
        : stop(false)
        , pending_tasks(0)
        , active_tasks(0)
        , sleeping_threads(0)
        , num_threads(n) {
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
        for (auto& t : workers) {
            if (t.joinable())
                t.join();
        }
    }

    // Disable copy/move
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

        // Wake one thread (if any) – simple and safe
        cv.notify_one();

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

    size_t threadCount() const { return num_threads; }

    size_t pendingCount() const {
        return pending_tasks.load(std::memory_order_acquire);
    }

    size_t activeCount() const {
        return active_tasks.load(std::memory_order_acquire);
    }

    size_t sleepingCount() const {
        std::lock_guard<std::mutex> lock(mutex);
        return sleeping_threads;
    }

    void waitAndStop() {
        waitAllTasksCompleted();
        {
            std::lock_guard<std::mutex> lock(mutex);
            stop = true;
        }
        cv.notify_all();
    }

private:
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

            // Try to grab a task without blocking
            if (task_queue.dequeue(task)) {
                pending_tasks.fetch_sub(1, std::memory_order_release);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();
                continue;
            }

            // No task – check if we should stop
            if (stop.load(std::memory_order_acquire)) {
                // Drain remaining tasks (rare)
                while (task_queue.dequeue(task)) {
                    pending_tasks.fetch_sub(1, std::memory_order_release);
                    TaskGuard guard(active_tasks, pending_tasks, cv);
                    task();
                }
                return;
            }

            // Sleep until work arrives or stop is signalled
            {
                std::unique_lock<std::mutex> lock(mutex);
                ++sleeping_threads;
                cv.wait(lock, [this] {
                    return !task_queue.isEmpty() ||
                           stop.load(std::memory_order_acquire);
                });
                --sleeping_threads;
            }
        }
    }

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> task_queue;

    mutable std::mutex mutex;
    std::condition_variable cv;

    std::atomic<bool> stop;
    std::atomic<size_t> pending_tasks;
    std::atomic<size_t> active_tasks;
    
    size_t sleeping_threads;   // protected by mutex
    const size_t num_threads;
};

#endif // THREAD_POOL_H
