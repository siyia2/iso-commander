#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"


// A global thread pool for async tasks
template <typename T>
class LockFreeQueue {
private:
    struct Node {
        T data;
        std::atomic<Node*> next;
        Node(T value = T()) : data(std::move(value)), next(nullptr) {}
    };

    alignas(64) std::atomic<Node*> head; // Ensures head is on a separate cache line
    alignas(64) std::atomic<Node*> tail; // Ensures tail is on a separate cache line

public:
    LockFreeQueue() {
        Node* dummy = new Node(); // Create a dummy node
        head.store(dummy, std::memory_order_relaxed); // Initialize head to dummy
        tail.store(dummy, std::memory_order_relaxed); // Initialize tail to dummy
    }
    
    ~LockFreeQueue() {
        Node* current = head.load(std::memory_order_relaxed);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current; // Clean up all nodes
            current = next;
        }
    }

    void enqueue(T value) {
        Node* newNode = nullptr;
        try {
            newNode = new Node(std::move(value)); // Allocate new node
        } catch (...) {
            // Handle allocation failure (if needed, rethrow or log the error)
            throw;
        }
        while (true) {
            Node* oldTail = tail.load(std::memory_order_relaxed); // Get current tail
            Node* next = oldTail->next.load(std::memory_order_relaxed); // Get next of the tail
            if (next == nullptr) {
                // If next is null, attempt to insert new node
                if (oldTail->next.compare_exchange_weak(next, newNode,
                                                        std::memory_order_release,
                                                        std::memory_order_relaxed)) {
                    // Move tail to the new node
                    tail.compare_exchange_strong(oldTail, newNode,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
                    return;
                }
            } else {
                // If tail was not pointing to the last node, update it
                tail.compare_exchange_strong(oldTail, next,
                                             std::memory_order_release,
                                             std::memory_order_relaxed);
            }
        }
    }

    bool dequeue(T& result) {
        while (true) {
            Node* oldHead = head.load(std::memory_order_relaxed); // Get current head
            Node* next = oldHead->next.load(std::memory_order_relaxed); // Get next of the head
            if (next == nullptr) {
                return false;  // Queue is empty
            }
            if (head.compare_exchange_weak(oldHead, next,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                // Successfully moved head to next, extract the data
                result = std::move(next->data);
                delete oldHead; // Clean up old head node
                return true;
            }
        }
    }

    bool isEmpty() const {
        return head.load(std::memory_order_acquire)->next.load(std::memory_order_acquire) == nullptr;
    }
};

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this); // Start worker threads
        }
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        
        {
            std::unique_lock<std::mutex> lock(mutex); // Lock the task queue
            if (stop) {
                throw std::runtime_error("Enqueue on stopped ThreadPool");
            }
            tasks.enqueue([task]() { (*task)(); }); // Enqueue the task
        }
        condition.notify_one(); // Notify a worker thread
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex); // Lock the task queue
            stop = true; // Set stop flag to true
        }
        condition.notify_all(); // Notify all worker threads to stop
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join(); // Join all worker threads
            }
        }
    }

private:
    void workerThread() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex); // Lock the task queue
                condition.wait(lock, [this] { 
                    return stop || !tasks.isEmpty(); 
                }); // Wait for a task or stop signal
                if (stop && tasks.isEmpty()) {
                    return; // Exit if stop is true and no tasks are left
                }
                if (!tasks.isEmpty()) {
                    tasks.dequeue(task); // Dequeue a task
                }
            }
            if (task) {
                task(); // Execute the task
            }
        }
    }

    std::vector<std::thread> workers; // Worker threads
    LockFreeQueue<std::function<void()>> tasks; // Task queue
    std::mutex mutex; // Mutex for task queue
    std::condition_variable condition; // Condition variable for task queue
    bool stop; // Stop flag
};

#endif // THREAD_POOL_H
