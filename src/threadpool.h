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

public:
    LockFreeQueue() {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }
    
    ~LockFreeQueue() {
        Node* current = head.load(std::memory_order_relaxed);
        while (current != nullptr) {
            Node* next = current->next.load(std::memory_order_relaxed);
            delete current;
            current = next;
        }
    }

    void enqueue(T value) {
        Node* newNode = new Node(std::move(value));
        while (true) {
            Node* oldTail = tail.load(std::memory_order_relaxed);
            Node* next = oldTail->next.load(std::memory_order_relaxed);
            if (next == nullptr) {
                if (oldTail->next.compare_exchange_weak(next, newNode,
                                                       std::memory_order_release,
                                                       std::memory_order_relaxed)) {
                    tail.compare_exchange_strong(oldTail, newNode,
                                                 std::memory_order_release,
                                                 std::memory_order_relaxed);
                    return;
                }
            } else {
                tail.compare_exchange_strong(oldTail, next,
                                             std::memory_order_release,
                                             std::memory_order_relaxed);
            }
        }
    }

    bool dequeue(T& result) {
        while (true) {
            Node* oldHead = head.load(std::memory_order_relaxed);
            Node* next = oldHead->next.load(std::memory_order_relaxed);
            if (next == nullptr) {
                return false;  // Queue is empty
            }
            if (head.compare_exchange_weak(oldHead, next,
                                           std::memory_order_release,
                                           std::memory_order_relaxed)) {
                result = std::move(next->data);
                delete oldHead;
                return true;
            }
        }
    }

private:
    alignas(64) std::atomic<Node*> head;
    alignas(64) std::atomic<Node*> tail;
};

class ThreadPool {
public:
    explicit ThreadPool(size_t numThreads) : stop(false) {
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<decltype(f(args...))> {
        using return_type = decltype(f(args...));
        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );
        std::future<return_type> res = task->get_future();
        
        tasks.enqueue([task]() { (*task)(); });
        condition.notify_one();
        return res;
    }

    ~ThreadPool() {
        {
            std::unique_lock<std::mutex> lock(mutex);
            stop = true;
        }
        condition.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        // Ensure all tasks are handled
        std::function<void()> task;
        while (tasks.dequeue(task)) {
            // Re-enqueue or handle remaining tasks if necessary
        }
    }

private:
    void workerThread() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stop || !tasks_empty(); });
                if (stop && tasks_empty()) {
                    return;
                }
                tasks.dequeue(task);  // Dequeue outside of the lock
            }
            if (task) {
                task();  // Execute the task outside of the lock
            }
        }
    }

    bool tasks_empty() {
        std::function<void()> task;
        if (tasks.dequeue(task)) {
            tasks.enqueue(std::move(task));  // Re-enqueue the task
            return false;
        }
        return true;
    }

    std::vector<std::thread> workers;
    LockFreeQueue<std::function<void()>> tasks;
    std::mutex mutex;
    std::condition_variable condition;
    bool stop;
};

#endif // THREAD_POOL_H
