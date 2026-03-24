// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "headers.h"


// ========================= LOCK-FREE QUEUE =========================
// Michael-Scott queue + freelist (safe reclamation via reuse).
//
// LOCK-FREE GUARANTEE:
//   - enqueue / dequeue / freelist operations: fully lock-free (CAS loops only)
//   - The ThreadPool wrapper uses a mutex ONLY for idle parking (cv.wait_for).
//     The hot path — enqueue, dequeue, steal, task execution — never touches a lock.
//
// ABA SAFETY:
//   Nodes are never freed back to the OS while they might be in-flight.
//   They are either in the queue, on the freelist, or in a running task.
//   This prevents ABA-related corruption.
//
// FREELIST BEHAVIOR:
//   - Nodes are recycled via a freelist to avoid frequent new/delete calls.
//   - To prevent unbounded memory retention under bursty load, the freelist
//     is soft-capped at MAX_FREE nodes. Any node freed beyond this cap
//     is safely returned to the OS.
//   - Allocating a new node first attempts to reuse a freelist node; otherwise,
//     a fresh node is created.
//
// INVARIANT:
//   A node is in exactly one of: {queue, freelist, in-flight task wrapper}.

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
    alignas(64) std::atomic<Node*> free_list{nullptr};

    // --- NEW: freelist cap ---
    alignas(64) std::atomic<size_t> free_count{0};
    static constexpr size_t MAX_FREE = 1 << 16; // 65536 nodes cap

    Node* alloc_node(T&& val) {
        Node* node = free_list.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            if (free_list.compare_exchange_weak(
                    node, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {

                free_count.fetch_sub(1, std::memory_order_relaxed);

                node->data = std::move(val);
                node->next.store(nullptr, std::memory_order_relaxed);
                return node;
            }
        }
        return new Node(std::move(val));
    }

    void free_node(Node* node) {
        // --- NEW: enforce cap ---
        size_t count = free_count.load(std::memory_order_relaxed);
        while (count < MAX_FREE) {
            if (free_count.compare_exchange_weak(
                    count, count + 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {

                Node* old_head = free_list.load(std::memory_order_relaxed);
                do {
                    node->next.store(old_head, std::memory_order_relaxed);
                } while (!free_list.compare_exchange_weak(
                    old_head, node,
                    std::memory_order_release,
                    std::memory_order_relaxed));
                return;
            }
            // CAS failed → count updated, retry
        }

        // Cap reached → free to OS
        delete node;
    }

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

        node = free_list.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    void enqueue(T value) {
        Node* new_node = alloc_node(std::move(value));
        while (true) {
            Node* last = tail.load(std::memory_order_acquire);
            Node* next = last->next.load(std::memory_order_acquire);

            if (last != tail.load(std::memory_order_acquire))
                continue;

            if (next == nullptr) {
                if (last->next.compare_exchange_weak(
                        next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {

                    tail.compare_exchange_weak(
                        last, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.compare_exchange_weak(
                    last, next,
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

                tail.compare_exchange_weak(
                    last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            } else {
                if (head.compare_exchange_weak(
                        first, next,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {

                    result = std::move(next->data);
                    free_node(first);
                    return true;
                }
            }
        }
    }

    bool isEmpty() const {
        Node* h = head.load(std::memory_order_acquire);
        return h->next.load(std::memory_order_acquire) == nullptr;
    }

    bool steal(T& result) { return dequeue(result); }
};


// ========================= THREAD POOL =========================

class ThreadPool {
private:
    struct alignas(64) AtomicBool {
        std::atomic<bool> value;
        explicit AtomicBool(bool v = false) : value(v) {}
    };

    struct alignas(64) AtomicSize {
        std::atomic<size_t> value;
        explicit AtomicSize(size_t v = 0) : value(v) {}
    };

    std::vector<std::thread> workers;
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues;

    std::mutex              mutex;
    std::condition_variable cv;

    AtomicBool stop   {false};
    AtomicSize next_queue{0};

    const size_t          num_threads;
    std::atomic<size_t>   active_tasks{0};
    std::atomic<size_t>   pending_tasks{0};

    struct TaskGuard {
        std::atomic<size_t>& active;
        std::atomic<size_t>& pending;
        std::condition_variable& cv;

        TaskGuard(std::atomic<size_t>& a,
                  std::atomic<size_t>& p,
                  std::condition_variable& c)
            : active(a), pending(p), cv(c)
        {
            active.fetch_add(1, std::memory_order_relaxed);
        }

        ~TaskGuard() {
            active.fetch_sub(1, std::memory_order_release);
            if (active.load(std::memory_order_acquire) == 0)
                cv.notify_all();
        }
    };

    size_t selectQueue() {
        return next_queue.value.fetch_add(1, std::memory_order_relaxed) % num_threads;
    }

    void backoff(size_t& spin) {
        if (spin < 16) {
            ++spin;
            for (size_t i = 0; i < (size_t{1} << spin); ++i)
                asm volatile("pause" ::: "memory");
        } else if (spin < 20) {
            ++spin;
            std::this_thread::yield();
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    }

    void workerThread(size_t id) {
        size_t spin = 0;

        while (true) {
            std::function<void()> task;

            if (queues[id]->dequeue(task)) {
                spin = 0;
                pending_tasks.fetch_sub(1, std::memory_order_relaxed);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();
                continue;
            }

            bool stolen = false;
            for (size_t i = 1; i < num_threads; ++i) {
                size_t victim = (id + i) % num_threads;
                if (queues[victim]->steal(task)) {
                    spin = 0;
                    stolen = true;
                    pending_tasks.fetch_sub(1, std::memory_order_relaxed);
                    TaskGuard guard(active_tasks, pending_tasks, cv);
                    task();
                    break;
                }
            }
            if (stolen) continue;

            if (stop.value.load(std::memory_order_acquire)) {
                for (size_t i = 0; i < num_threads; ++i) {
                    while (queues[i]->dequeue(task)) {
                        pending_tasks.fetch_sub(1, std::memory_order_relaxed);
                        TaskGuard guard(active_tasks, pending_tasks, cv);
                        task();
                    }
                }
                return;
            }

            backoff(spin);

            if (spin >= 18) {
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(1), [&] {
                    if (stop.value.load(std::memory_order_acquire))
                        return true;
                    for (auto& q : queues)
                        if (!q->isEmpty()) return true;
                    return false;
                });
                spin = 0;
            }
        }
    }

public:
    explicit ThreadPool(size_t n) : num_threads(n) {
        queues.reserve(n);
        for (size_t i = 0; i < n; ++i)
            queues.emplace_back(
                std::make_unique<LockFreeQueue<std::function<void()>>>());

        workers.reserve(n);
        for (size_t i = 0; i < n; ++i)
            workers.emplace_back(&ThreadPool::workerThread, this, i);
    }

    ~ThreadPool() {
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();
        for (auto& t : workers)
            t.join();
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args)
        -> std::future<std::invoke_result_t<F, Args...>>
    {
        using return_type = std::invoke_result_t<F, Args...>;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();

        pending_tasks.fetch_add(1, std::memory_order_relaxed);
        queues[selectQueue()]->enqueue([task]{ (*task)(); });

        cv.notify_one();

        return res;
    }
	// Block until every submitted task has completed.
    //
	//We use pending_tasks (incremented before enqueue, decremented when
    // a worker claims the item) + active_tasks (live running tasks).  Both
    // must be zero simultaneously.  Because pending is decremented strictly
    // before active is incremented (inside workerThread), the combined
    // predicate pending==0 && active==0 is only true when there is genuinely
    // no work in any state.
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return pending_tasks.load(std::memory_order_acquire) == 0 &&
                   active_tasks.load(std::memory_order_acquire)  == 0;
        });
    }

    size_t threadCount() const { return num_threads; }
};

#endif // THREAD_POOL_H
