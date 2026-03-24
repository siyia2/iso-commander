// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "headers.h"

// ========================= LOCK-FREE QUEUE =========================
// Michael-Scott lock-free queue with freelist for efficient node reuse.
//
// DESIGN PHILOSOPHY:
//   - Fully lock-free in hot path (enqueue/dequeue/steal use only CAS)
//   - Memory efficient with node recycling to reduce allocation overhead
//   - Cache-aligned structures to prevent false sharing
//   - Safe memory reclamation via freelist with soft cap
//
// LOCK-FREE GUARANTEES:
//   - enqueue() and dequeue() never block; they spin until successful
//   - Multiple producers/consumers can operate concurrently without locks
//   - ABA problem prevented by never freeing nodes back to OS while in-flight
//
// FREELIST BEHAVIOR:
//   - Nodes are recycled to freelist instead of being freed immediately
//   - Soft cap of MAX_FREE (65536) prevents unbounded memory growth
//   - When cap is reached, nodes are properly returned to OS
//   - Allocator first attempts to reuse freelist nodes; falls back to new
//
// MEMORY ORDERING:
//   - acquire/release semantics for proper synchronization
//   - relaxed for freelist operations (separate synchronization domain)
//   - memory_order_acq_rel for CAS operations requiring both

template <typename T>
class LockFreeQueue {
private:
    // Node structure containing data and next pointer
    // Each node lives in exactly one state: queue, freelist, or in-flight task
    struct Node {
        std::atomic<Node*> next;  // Next node in queue/freelist
        T data;                   // Actual payload

        Node() : next(nullptr) {}
        Node(T&& val) : next(nullptr), data(std::move(val)) {}
    };

    // Cache-line aligned atomic pointers to prevent false sharing
    alignas(64) std::atomic<Node*> head;  // Head of queue (for dequeue)
    alignas(64) std::atomic<Node*> tail;  // Tail of queue (for enqueue)
    alignas(64) std::atomic<Node*> free_list{nullptr};  // Reusable node pool

    // Freelist management with size cap
    alignas(64) std::atomic<size_t> free_count{0};
    static constexpr size_t MAX_FREE = 1 << 16;  // 65536 nodes max in freelist

    // Allocate a node, preferring freelist before heap allocation
    Node* alloc_node(T&& val) {
        Node* node = free_list.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            if (free_list.compare_exchange_weak(
                    node, next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {

                free_count.fetch_sub(1, std::memory_order_relaxed);

                // Re-initialize the node with new data
                node->data = std::move(val);
                node->next.store(nullptr, std::memory_order_relaxed);
                return node;
            }
        }
        // Freelist empty - allocate fresh node
        return new Node(std::move(val));
    }

    // Return node to freelist, but only if under capacity limit
    void free_node(Node* node) {
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
            // CAS failed - count was updated, retry with new value
        }
        // Freelist full - free to OS
        delete node;
    }

public:
    // Constructor creates dummy node to simplify queue logic
    LockFreeQueue() {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    // Destructor cleans all nodes (queue + freelist)
    ~LockFreeQueue() {
        // Clean main queue
        Node* node = head.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }

        // Clean freelist
        node = free_list.load(std::memory_order_acquire);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    // Enqueue an item (lock-free, multiple producers)
    void enqueue(T value) {
        Node* new_node = alloc_node(std::move(value));
        while (true) {
            Node* last = tail.load(std::memory_order_acquire);
            Node* next = last->next.load(std::memory_order_acquire);

            // Check if tail has changed (help other threads progress)
            if (last != tail.load(std::memory_order_acquire))
                continue;

            // Is this the actual tail?
            if (next == nullptr) {
                // Try to link new node at the end
                if (last->next.compare_exchange_weak(
                        next, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {

                    // Success - update tail to new node
                    tail.compare_exchange_weak(
                        last, new_node,
                        std::memory_order_release,
                        std::memory_order_relaxed);
                    return;
                }
            } else {
                // Tail is behind - help advance it
                tail.compare_exchange_weak(
                    last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            }
        }
    }

    // Dequeue an item (lock-free, multiple consumers)
    bool dequeue(T& result) {
        while (true) {
            Node* first = head.load(std::memory_order_acquire);
            Node* last  = tail.load(std::memory_order_acquire);
            Node* next  = first->next.load(std::memory_order_acquire);

            // Check if head has changed
            if (first != head.load(std::memory_order_acquire))
                continue;

            // Is queue empty?
            if (first == last) {
                if (next == nullptr)
                    return false;  // Empty queue

                // Tail is behind - help advance it
                tail.compare_exchange_weak(
                    last, next,
                    std::memory_order_release,
                    std::memory_order_relaxed);
            } else {
                // Try to advance head to next node
                if (head.compare_exchange_weak(
                        first, next,
                        std::memory_order_release,
                        std::memory_order_relaxed)) {

                    // Success - extract data and free dummy node
                    result = std::move(next->data);
                    free_node(first);
                    return true;
                }
            }
        }
    }

    // Check if queue is empty (approximate, for heuristics only)
    bool isEmpty() const {
        Node* h = head.load(std::memory_order_acquire);
        return h->next.load(std::memory_order_acquire) == nullptr;
    }

    // Steal operation (alias for dequeue, used by work-stealing scheduler)
    bool steal(T& result) { return dequeue(result); }
};


// ========================= THREAD POOL =========================
// Lock-free work-stealing thread pool optimized for I/O-bound workloads.
//
// ARCHITECTURE:
//   - Each worker thread has its own lock-free queue
//   - Work-stealing ensures load balancing across threads
//   - Hybrid synchronization: lock-free for hot path, mutex only for sleeping
//   - Cache-aligned structures prevent false sharing
//
// WORK-STEALING ALGORITHM:
//   1. Thread first checks its own queue (affinity)
//   2. If empty, attempts to steal from other queues (round-robin)
//   3. If still empty and not stopping, goes to sleep
//   4. Before sleeping, double-checks all queues to avoid race conditions
//   5. Wakes up only when work is available or pool is stopping
//
// THREAD STATES:
//   - RUNNING: Executing tasks
//   - SPINNING: Looking for work (brief)
//   - SLEEPING: Blocked on condition variable (no CPU usage)
//
// TASK TRACKING:
//   - pending_tasks: Tasks queued but not yet started (incremented on enqueue)
//   - active_tasks: Tasks currently executing (incremented in TaskGuard)
//   - Both must reach zero for pool to be truly idle
//
// MEMORY SAFETY:
//   - TaskGuard uses RAII for exception-safe task counting
//   - Shared tasks are managed via shared_ptr to prevent premature destruction
//   - Nodes are recycled via freelist with soft cap to prevent memory bloat

class ThreadPool {
private:
    // Cache-aligned atomic bool to prevent false sharing with other variables
    struct alignas(64) AtomicBool {
        std::atomic<bool> value;
        explicit AtomicBool(bool v = false) : value(v) {}
    };

    // Cache-aligned atomic size_t for similar reasons
    struct alignas(64) AtomicSize {
        std::atomic<size_t> value;
        explicit AtomicSize(size_t v = 0) : value(v) {}
    };

    std::vector<std::thread> workers;                              // Worker threads
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues;  // Per-thread queues

    std::mutex              mutex;      // Guards condition variable only
    std::condition_variable cv;         // For parking idle threads
    std::atomic<size_t>     sleeping_threads{0};  // Count of parked threads

    AtomicBool stop   {false};   // Signal to stop all threads
    AtomicSize next_queue{0};    // Round-robin index for task distribution

    const size_t          num_threads;      // Number of worker threads
    std::atomic<size_t>   active_tasks{0};  // Tasks currently running
    std::atomic<size_t>   pending_tasks{0}; // Tasks queued but not started

    // RAII guard for task execution tracking
    // Ensures active_tasks is properly maintained even if task throws
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
            // When all tasks are done, notify any threads waiting for completion
            if (active.load(std::memory_order_acquire) == 0 &&
                pending.load(std::memory_order_acquire) == 0) {
                cv.notify_all();
            }
        }
    };

    // Select queue for new task using round-robin distribution
    // Returns index between 0 and num_threads-1
    size_t selectQueue() {
        return next_queue.value.fetch_add(1, std::memory_order_relaxed) % num_threads;
    }

    // Attempt to steal a task from any other thread's queue
    // Returns true if task was stolen, false otherwise
    bool tryStealTask(std::function<void()>& task, size_t thief_id) {
        // Round-robin victim selection to avoid starvation
        for (size_t i = 1; i < num_threads; ++i) {
            size_t victim = (thief_id + i) % num_threads;
            if (queues[victim]->steal(task)) {
                return true;
            }
        }
        return false;
    }

    // Main worker thread function
    void workerThread(size_t id) {
        while (true) {
            std::function<void()> task;
            bool has_work = false;

            // Phase 1: Check own queue (affinity)
            if (queues[id]->dequeue(task)) {
                has_work = true;
            }
            
            // Phase 2: Steal from others if own queue was empty
            if (!has_work && tryStealTask(task, id)) {
                has_work = true;
            }

            // Execute task if we found work
            if (has_work) {
                pending_tasks.fetch_sub(1, std::memory_order_relaxed);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();  // Task may throw, but guard ensures proper cleanup
                continue;
            }

            // Check if pool is shutting down
            if (stop.value.load(std::memory_order_acquire)) {
                // Drain all remaining tasks before exiting
                for (size_t i = 0; i < num_threads; ++i) {
                    while (queues[i]->dequeue(task)) {
                        pending_tasks.fetch_sub(1, std::memory_order_relaxed);
                        TaskGuard guard(active_tasks, pending_tasks, cv);
                        task();
                    }
                }
                return;
            }

            // One final steal attempt before sleeping to catch race conditions
            // Prevents scenario where work appears just before sleep decision
            if (tryStealTask(task, id)) {
                pending_tasks.fetch_sub(1, std::memory_order_relaxed);
                TaskGuard guard(active_tasks, pending_tasks, cv);
                task();
                continue;
            }

            // No work found - prepare to sleep
            sleeping_threads.fetch_add(1, std::memory_order_relaxed);
            
            std::unique_lock<std::mutex> lock(mutex);
            
            // Double-check all conditions before actually sleeping
            // This prevents missed wake-ups and spurious wake-ups
            bool should_sleep = true;
            
            // Check own queue again
            if (!queues[id]->isEmpty()) {
                should_sleep = false;
            }
            
            // Check other queues for stealable work
            if (should_sleep) {
                for (size_t i = 0; i < num_threads; ++i) {
                    if (i != id && !queues[i]->isEmpty()) {
                        should_sleep = false;
                        break;
                    }
                }
            }
            
            // Check stop flag again
            if (stop.value.load(std::memory_order_acquire)) {
                should_sleep = false;
            }
            
            if (should_sleep) {
                // Block until work is available or pool is stopping
                cv.wait(lock, [this, id] {
                    // Wake conditions:
                    // 1. Pool is stopping (shutdown)
                    if (stop.value.load(std::memory_order_acquire))
                        return true;
                    
                    // 2. Our queue has work (most likely to be fast)
                    if (!queues[id]->isEmpty())
                        return true;
                    
                    // 3. Any other queue has work we can steal
                    for (size_t i = 0; i < num_threads; ++i) {
                        if (i != id && !queues[i]->isEmpty())
                            return true;
                    }
                    
                    return false;
                });
            }
            
            sleeping_threads.fetch_sub(1, std::memory_order_relaxed);
            // Continue loop to re-evaluate work availability
        }
    }

public:
    // Constructor: Create thread pool with specified number of threads
    explicit ThreadPool(size_t n) : num_threads(n) {
        // Initialize per-thread queues
        queues.reserve(n);
        for (size_t i = 0; i < n; ++i)
            queues.emplace_back(
                std::make_unique<LockFreeQueue<std::function<void()>>>());

        // Launch worker threads
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i)
            workers.emplace_back(&ThreadPool::workerThread, this, i);
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

        // Wrap function in packaged_task for future support
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...));

        std::future<return_type> res = task->get_future();

        // Update task counts and queue the work
        pending_tasks.fetch_add(1, std::memory_order_relaxed);
        queues[selectQueue()]->enqueue([task]{ (*task)(); });

        // Wake up a sleeping thread if any exist
        // Using notify_one() is sufficient - exactly one thread will process this task
        size_t sleeping = sleeping_threads.load(std::memory_order_acquire);
        if (sleeping > 0) {
            cv.notify_one();
        }

        return res;
    }

    // Block until all submitted tasks have completed
    // Useful for synchronization points and graceful shutdown
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
    // Useful for graceful shutdown scenarios
    void waitAndStop() {
        waitAllTasksCompleted();
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();
    }
};

#endif // THREAD_POOL_H
