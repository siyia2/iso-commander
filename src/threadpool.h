#ifndef THREAD_POOL_H
#define THREAD_POOL_H
#include "headers.h"


// A global threadpool for async tasks with work-stealing scalable from 1 to 192 threads
template <typename T>
class LockFreeQueue {
private:
    static constexpr size_t CACHE_LINE_SIZE = 64;

    // Node structure for the queue
	struct Node {
		T data;
		std::atomic<Node*> next;
		std::atomic<uint64_t> timestamp; // Timestamp for ABA prevention
		Node(T value = T()) : data(std::move(value)), next(nullptr), timestamp(0) {}
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
		return std::min(num_threads * 64, static_cast<size_t>(12288));
    }

    // Allocate a node from the pool or dynamically
    Node* allocate_node(T value) {
		size_t index = pool_index.fetch_add(1, std::memory_order_relaxed);
		if (index < pool_size) {
			Node* node = new (&node_pool[index]) Node(std::move(value));
			node->timestamp.store(1, std::memory_order_relaxed); // Initial timestamp
			return node;
		} else {
			Node* node = new Node(std::move(value));
			node->timestamp.store(1, std::memory_order_relaxed); // Initial timestamp
			return node;
		}
	}


    // Deallocate a node
    void deallocate_node(Node* node) {
    // Check if the node belongs to the node_pool
    auto pool_begin = &node_pool[0];
    auto pool_end = pool_begin + pool_size;
    if (node >= pool_begin && node < pool_end) {
        node->~Node(); // Call the destructor explicitly, but don't free memory
        node->timestamp.fetch_add(1, std::memory_order_relaxed); // Increment timestamp to prevent ABA
		} else {
			delete node; // If not in pool, delete normally
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
    // Explicitly destroy all nodes in the pool
    for (size_t i = 0; i < pool_size; ++i) {
        node_pool[i].~Node();
		}
	}

    // Enqueue an item into the queue
    void enqueue(T value) {
    Node* newNode = allocate_node(std::move(value));
    while (true) {
        Node* oldTail = tail.ptr.load(std::memory_order_acquire);
        Node* next = oldTail->next.load(std::memory_order_acquire);
        uint64_t timestamp = oldTail->timestamp.load(std::memory_order_relaxed);
        if (next == nullptr) {
            if (oldTail->next.compare_exchange_weak(next, newNode,
                                                    std::memory_order_release,
                                                    std::memory_order_relaxed) &&
                oldTail->timestamp.compare_exchange_weak(timestamp, timestamp + 1,
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
    // Struct for aligning atomic bool values to avoid false sharing
    struct alignas(64) AlignedAtomic {
        std::atomic<bool> value;
        AlignedAtomic(bool initial = false) : value(initial) {}
    };

    // Struct for aligning atomic size_t values to avoid false sharing
    struct alignas(64) AlignedAtomicSize {
        std::atomic<size_t> value;
        AlignedAtomicSize(size_t initial = 0) : value(initial) {}
    };

    // Vector of worker threads
    std::vector<std::thread> workers;

    // Vector of queues (one for each worker thread)
    std::vector<std::unique_ptr<LockFreeQueue<std::function<void()>>>> queues;

    // Mutex and condition variable for synchronization
    std::mutex mutex;
    std::condition_variable cv;

    // Atomic flag to signal threads to stop
    AlignedAtomic stop;

    // Atomic variable to select the next queue for task enqueueing
    AlignedAtomicSize next_queue;

    // Number of threads in the pool
    const size_t num_threads;

    // Atomic counter for tracking active tasks
    std::atomic<size_t> active_tasks;

    // Private method to select a queue index based on a random strategy
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

            // Attempt to steal tasks if the current queue is empty
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

            // Execute the task if obtained, otherwise wait
            if (gotTask) {
                ++active_tasks;
                task();
                --active_tasks;
            } else {
                // Wait for a signal to wake up or terminate
                std::unique_lock<std::mutex> lock(mutex);
                cv.wait_for(lock, std::chrono::milliseconds(1), [this] { 
                    return stop.value.load(std::memory_order_acquire) || 
                           std::any_of(queues.begin(), queues.end(), 
                               [](const auto& q) { return !q->isEmpty(); });
                });

                // Exit the thread if signaled to stop and all queues are empty
                if (stop.value.load(std::memory_order_acquire) && 
                    std::all_of(queues.begin(), queues.end(), 
                        [](const auto& q) { return q->isEmpty(); })) {
                    return;
                }
            }
        }
    }

    // Method to determine the number of steal attempts based on the number of threads
    size_t adaptiveStealAttempts() {
        if (num_threads <= 2) return 1;
        return std::min(num_threads / 2, static_cast<size_t>(64));
    }

public:
    // Constructor to initialize the thread pool with a specified number of threads
    explicit ThreadPool(size_t numThreads)
        : stop(false), next_queue(0), num_threads(numThreads), active_tasks(0) {
        queues.reserve(numThreads);
        for (size_t i = 0; i < numThreads; ++i) {
            queues.emplace_back(std::make_unique<LockFreeQueue<std::function<void()>>>(numThreads));
        }
        for (size_t i = 0; i < numThreads; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this, i);
        }
    }

    // Enqueue method to submit a task to the thread pool
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

    // Destructor to stop all threads and clean up resources
    ~ThreadPool() {
        stop.value.store(true, std::memory_order_release);
        cv.notify_all();

        for (std::thread& worker : workers) {
            worker.join();
        }
    }

    // Method to wait until all tasks are completed
    void waitAllTasksCompleted() {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] {
            return active_tasks.load(std::memory_order_acquire) == 0 &&
                   std::all_of(queues.begin(), queues.end(),
                               [](const auto& q) { return q->isEmpty(); });
        });
    }
};

#endif // THREAD_POOL_H
