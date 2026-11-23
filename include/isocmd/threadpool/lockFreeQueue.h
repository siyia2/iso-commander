// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LOCKFREEQUEUE_H
#define LOCKFREEQUEUE_H

// C++ Standard Library Headers
#include <atomic>
#include <thread>
#include <utility>

// Project Headers
#include "isocmd/threadpool/hazardPointer.h"

// ─────────────────────────────────────────────────────────────────────────────
//  LockFreeQueue
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class LockFreeQueue
 * @brief Thread-safe, non-blocking Michael–Scott lock-free FIFO queue,
 *        reclaiming nodes via @ref HazardPointerDomain.
 *
 * Implements the classic Michael & Scott (1996) two-pointer CAS-based queue,
 * linked with plain `std::atomic<Node*>`. Both @ref enqueue and @ref dequeue
 * are linearisable and lock-free: `head`, `tail`, and every node's `next` are
 * native machine-word atomics with hardware CAS on every mainstream 64-bit
 * target — no internal spinlock table, no per-node control-block allocation,
 * unlike the `atomic<shared_ptr<Node>>` this replaces (see file-level
 * comment for the measured problem with that approach).
 *
 * The queue uses a sentinel (dummy) head node so that head and tail never
 * alias the same node while the queue is non-empty, simplifying the CAS logic.
 *
 * @tparam T Element type.  Must be movable.
 *
 * ### Safety model
 * A node is only ever freed via @ref HazardPointerDomain::retire, and only
 * once no thread's hazard-pointer slot still references it — see the
 * `HazardPointerDomain` class doc for the full protocol. Every place this
 * class dereferences a `Node*` it doesn't already own is preceded by
 * publishing that pointer into the calling thread's hazard slot and
 * re-validating it against the structure, per the standard protocol.
 *
 * ### Typical usage
 * @code
 *   LockFreeQueue<int> q;
 *   q.enqueue(42);
 *
 *   int val;
 *   if (q.dequeue(val)) {
 *       // val == 42
 *   }
 * @endcode
 */
template <typename T>
class LockFreeQueue {
private:
    /**
     * @brief Internal singly-linked list node.
     *
     * @c data holds the element value (only meaningful in non-sentinel nodes).
     * @c next is an atomic raw pointer to the successor node.
     */
    struct Node {
        T data;
        std::atomic<Node*> next;

        Node() : data(), next(nullptr) {}
        Node(T&& val) : data(std::move(val)), next(nullptr) {}
    };

    /// @brief Points to the sentinel node; elements are dequeued from @c head->next.
    alignas(64) std::atomic<Node*> head;
    /// @brief Points to the last enqueued node (may lag one step behind the true tail).
    alignas(64) std::atomic<Node*> tail;

public:
    /**
     * @brief Constructs an empty queue with a single sentinel node.
     */
    LockFreeQueue() {
        Node* dummy = new Node();
        head.store(dummy, std::memory_order_relaxed);
        tail.store(dummy, std::memory_order_relaxed);
    }

    /**
     * @brief Destroys the queue, freeing nodes iteratively.
     *
     * @note Not thread-safe — same precondition as any container's
     *       destructor: no concurrent @ref enqueue / @ref dequeue may be in
     *       flight while this runs. Because nothing else can be touching the
     *       queue at this point, no hazard-pointer protocol is needed here —
     *       nodes are freed directly, iteratively (never recursively, so
     *       this remains O(1) stack depth regardless of queue length, same
     *       guarantee the previous revision's explicit destructor provided).
     */
    ~LockFreeQueue() {
        Node* node = head.load(std::memory_order_relaxed);
        while (node) {
            Node* next = node->next.load(std::memory_order_relaxed);
            delete node;
            node = next;
        }
    }

    LockFreeQueue(const LockFreeQueue&) = delete;
    LockFreeQueue& operator=(const LockFreeQueue&) = delete;

    /**
     * @brief Appends @p value to the back of the queue.
     *
     * Allocates a new node, then uses a CAS loop to link it after the current
     * tail.  If the tail pointer has fallen behind (another thread enqueued but
     * did not yet swing the tail), this thread helps advance it first.
     *
     * The hazard slot on `last` is what makes it safe to dereference
     * `last->next` a moment after reading `last` from `tail`: without it,
     * another thread could have already dequeued and retired `last` in that
     * window, and `last->next.load()` would be a use-after-free.
     *
     * @param value The value to enqueue.  Moved into the new node.
     */
    void enqueue(T value) {
        Node* new_node = new Node(std::move(value));
        auto* hp = localHPRecord();

        while (true) {
            Node* last = tail.load(std::memory_order_acquire);

            // Publish + re-validate (hazard pointer protocol step 1).
            hp->hazards[0].store(last, std::memory_order_seq_cst);
            if (last != tail.load(std::memory_order_seq_cst)) {
                continue; // tail moved before we finished publishing; retry.
            }

            Node* next = last->next.load(std::memory_order_acquire);

            if (last == tail.load(std::memory_order_relaxed)) {
                if (next == nullptr) {
                    // Tail truly points to the last node; try to link new_node.
                    if (last->next.compare_exchange_weak(next, new_node,
                        std::memory_order_release, std::memory_order_relaxed)) {
                        // Link succeeded; try to swing tail (failure is benign).
                        tail.compare_exchange_weak(last, new_node, std::memory_order_release);
                        hp->hazards[0].store(nullptr, std::memory_order_release);
                        return;
                    }
                } else {
                    // Tail is lagging; help advance it.
                    tail.compare_exchange_weak(last, next, std::memory_order_release);
                }
            }
            pause_processor();
        }
    }

    /**
     * @brief Removes and returns the element at the front of the queue.
     *
     * Uses a CAS on @c head to atomically claim ownership of the first real
     * node.  The thread that wins the CAS is the sole owner of that node's
     * @c data and moves it into @p result — deliberately performed *after*
     * the winning CAS (not before, as a naive port might do), since another
     * thread's concurrently-failed attempt must never have touched (and thus
     * never move-corrupted) `next->data`.
     *
     * Both `first` and `next` are hazard-protected for the duration of this
     * call, so it is always safe to dereference either even though a
     * concurrent thread could otherwise have unlinked and retired them.
     *
     * @param[out] result  Receives the dequeued value on success.
     * @return `true` if an element was dequeued, `false` if the queue was empty.
     */
    bool dequeue(T& result) {
        auto* hp = localHPRecord();

        while (true) {
            Node* first = head.load(std::memory_order_acquire);
            hp->hazards[0].store(first, std::memory_order_seq_cst);
            if (first != head.load(std::memory_order_seq_cst)) {
                continue; // head moved before we finished publishing; retry.
            }

            Node* last = tail.load(std::memory_order_acquire);
            Node* next = first->next.load(std::memory_order_acquire);

            hp->hazards[1].store(next, std::memory_order_seq_cst);
            if (first != head.load(std::memory_order_seq_cst)) {
                continue; // first was retired mid-publish of `next`; retry.
            }

            if (first == head.load(std::memory_order_relaxed)) {
                if (first == last) {
                    if (next == nullptr) {
                        // Truly empty.
                        hp->hazards[0].store(nullptr, std::memory_order_release);
                        hp->hazards[1].store(nullptr, std::memory_order_release);
                        return false;
                    }
                    // Tail is lagging; help advance it.
                    tail.compare_exchange_weak(last, next, std::memory_order_release);
                } else if (next != nullptr) {
                    // Attempt to swing head to the next node.
                    Node* expected_first = first;
                    if (head.compare_exchange_weak(expected_first, next, std::memory_order_acq_rel)) {
                        // Sole owner of next->data; extract it now that we've won.
                        result = std::move(next->data);
                        hp->hazards[0].store(nullptr, std::memory_order_release);
                        hp->hazards[1].store(nullptr, std::memory_order_release);
                        HazardPointerDomain::instance().retire(first);
                        return true;
                    }
                }
            }
            pause_processor();
        }
    }

private:
    /**
     * @brief Emits a CPU spin-loop hint or yields the thread.
     */
    static inline void pause_processor() noexcept {
        #if defined(__x86_64__) || defined(_M_X64)
            __builtin_ia32_pause();
        #else
            std::this_thread::yield();
        #endif
    }
};

#endif // LOCKFREEQUEUE_H
