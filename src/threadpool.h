// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

// Project Headers
#include "./concurrency.h"

/**
 * @file thread_pool.hpp
 * @brief Lock-free thread pool with type-erased move-only tasks.
 *
 * Provides four cooperative components:
 *  - @ref MoveOnlyTask       – a small-buffer-optimised, move-only type-erased callable.
 *  - @ref HazardPointerDomain – a process-wide hazard-pointer registry providing safe
 *                                deferred reclamation for lock-free node-based structures.
 *  - @ref LockFreeQueue      – a Michael–Scott lock-free FIFO queue built on raw
 *                                `atomic<Node*>`, reclaiming retired nodes via hazard pointers.
 *  - @ref ThreadPool         – a fully lock-free worker-thread pool built on the two above.
 *
 * @note Requires C++20 (`std::atomic::wait` / `notify_*`).
 *
 * @par Revision notes (this revision)
 * `LockFreeQueue` previously stored its nodes behind `std::atomic<std::shared_ptr<Node>>`.
 * That compiles and is correct, but on every mainstream libstdc++/libc++ build as of
 * this writing, `atomic<shared_ptr<T>>` is **not** lock-free — `is_lock_free()` and
 * the compile-time `is_always_lock_free` both report `false`, because the
 * implementation protects the pointer+control-block pair with an internal spinlock
 * table keyed by address. That meant every `load`/`store`/`compare_exchange` on
 * `head`, `tail`, or any node's `next` silently took an internal lock — worse, a
 * lock *shared across every unrelated `atomic<shared_ptr<T>>` in the process* that
 * happens to hash to the same table slot — while still paying the cost of
 * heap-allocated control blocks and atomic refcount churn per node.
 *
 * This revision replaces that with the standard fix for the underlying problem
 * (safe reclamation of a node another thread might still be dereferencing) without
 * giving up lock-freedom to get it: **hazard pointers** (Maged Michael, 2004).
 * Nodes are now linked via plain `std::atomic<Node*>`, which *is* natively
 * lock-free CAS on every mainstream 64-bit target. Before a thread dereferences a
 * node it doesn't yet own, it publishes that node's address into a hazard-pointer
 * slot; any other thread wanting to actually `delete` a retired node first scans
 * every live hazard-pointer slot in the process and defers deletion of any node
 * still published there. This removes both the false-sharing/global-lock-table
 * problem and the per-node control-block allocation entirely.
 *
 * The `ThreadPool`'s own `shutdown()` fix from the previous revision (folding the
 * stop flag into `task_state` instead of clobbering it with placeholder wakeup
 * values) is unchanged and still applies — see the `ThreadPool` class doc below.
 */

// ─────────────────────────────────────────────────────────────────────────────
//  MoveOnlyTask
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class MoveOnlyTask
 * @brief A move-only, type-erased callable wrapper with small-buffer optimisation.
 *
 * Stores any callable (function pointer, lambda, `std::bind` expression, …) that
 * accepts zero arguments and returns `void`.  The callable is held inside an
 * inline storage buffer of @ref StorageSize bytes whenever possible; otherwise it
 * is heap-allocated via `std::unique_ptr` and the pointer itself lives in the
 * buffer.  Either way the external interface – move, call, destroy – is uniform
 * through a compile-time-generated VTable.
 *
 * Copy construction and copy assignment are explicitly deleted because the stored
 * callable may itself be non-copyable (e.g. a lambda that captures a
 * `std::unique_ptr`).
 *
 * ### Typical usage
 * @code
 *   MoveOnlyTask task = []{ std::puts("hello"); };
 *   task();   // prints "hello"
 *
 *   MoveOnlyTask moved = std::move(task);
 *   moved();  // still prints "hello"
 *   // task is now empty (operator bool() == false)
 * @endcode
 */
class MoveOnlyTask {
private:
    /// @brief Maximum number of bytes that may be stored inline (without heap allocation).
    static constexpr std::size_t StorageSize = 120;

    /**
     * @brief Per-type virtual dispatch table.
     *
     * A single instance of this struct is generated at compile time for each
     * concrete callable type @c F via the `vtable_for<F>` / `vtable_for_heap<F>`
     * static members.  All three function pointers receive the raw storage pointer
     * and must cast it to the correct type internally.
     */
    struct VTable {
        void (*call)(void*);           ///< Invoke the stored callable.
        void (*destroy)(void*) noexcept; ///< Destroy the stored callable in-place.
        void (*move)(void*, void*) noexcept; ///< Move-construct from @p src into @p dst.
    };

    /// @brief Aligned raw storage for the callable (or a `unique_ptr` to it).
    alignas(std::max_align_t) char storage[StorageSize];

    /// @brief Active dispatch table, or `nullptr` when the task is empty.
    const VTable* vtable{nullptr};

    // ── Inline-storage VTable implementations ────────────────────────────────

    template <typename F>
    static void call_impl(void* ptr) {
        (*std::launder(static_cast<F*>(ptr)))();
    }

    template <typename F>
    static void destroy_impl(void* ptr) noexcept {
        std::launder(static_cast<F*>(ptr))->~F();
    }

    template <typename F>
    static void move_impl(void* dst, void* src) noexcept {
        ::new (dst) F(std::move(*std::launder(static_cast<F*>(src))));
    }

    template <typename F>
    static constexpr VTable vtable_for = { &call_impl<F>, &destroy_impl<F>, &move_impl<F> };

    // ── Heap-storage VTable implementations ──────────────────────────────────

    template <typename F>
    static void call_heap_impl(void* ptr) {
        auto* up = std::launder(static_cast<std::unique_ptr<F>*>(ptr));
        (**up)();
    }

    template <typename F>
    static void destroy_heap_impl(void* ptr) noexcept {
        using Box = std::unique_ptr<F>;
        std::launder(static_cast<Box*>(ptr))->~Box();
    }

    template <typename F>
    static void move_heap_impl(void* dst, void* src) noexcept {
        using Box = std::unique_ptr<F>;
        ::new (dst) Box(std::move(*std::launder(static_cast<Box*>(src))));
    }

    template <typename F>
    static constexpr VTable vtable_for_heap = { &call_heap_impl<F>, &destroy_heap_impl<F>, &move_heap_impl<F> };

public:
    // ── Constructors / destructor ─────────────────────────────────────────────

    /// @brief Constructs an empty (null) task.
    MoveOnlyTask() noexcept = default;

    template <typename F, typename = std::enable_if_t<!std::is_same_v<std::decay_t<F>, MoveOnlyTask>>>
    MoveOnlyTask(F&& f) {
        using DecayedF = std::decay_t<F>;
        if constexpr (sizeof(DecayedF) <= StorageSize && alignof(DecayedF) <= alignof(std::max_align_t)) {
            ::new (static_cast<void*>(storage)) DecayedF(std::forward<F>(f));
            vtable = &vtable_for<DecayedF>;
        } else {
            using Box = std::unique_ptr<DecayedF>;
            ::new (static_cast<void*>(storage)) Box(std::make_unique<DecayedF>(std::forward<F>(f)));
            vtable = &vtable_for_heap<DecayedF>;
        }
    }

    ~MoveOnlyTask() noexcept { reset(); }

    MoveOnlyTask(const MoveOnlyTask&) = delete;
    MoveOnlyTask& operator=(const MoveOnlyTask&) = delete;

    MoveOnlyTask(MoveOnlyTask&& other) noexcept { move_from(std::move(other)); }

    MoveOnlyTask& operator=(MoveOnlyTask&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(std::move(other));
        }
        return *this;
    }

    // ── Observers / mutators ─────────────────────────────────────────────────

    void operator()() {
        if (vtable) {
            vtable->call(storage);
        }
    }

    explicit operator bool() const noexcept { return vtable != nullptr; }

    void reset() noexcept {
        if (vtable) {
            vtable->destroy(storage);
            vtable = nullptr;
        }
    }

private:
    void move_from(MoveOnlyTask&& other) noexcept {
        vtable = other.vtable;
        if (vtable) {
            vtable->move(storage, other.storage);
            other.vtable = nullptr;
        }
    }
};

// ─────────────────────────────────────────────────────────────────────────────
//  HazardPointerDomain
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class HazardPointerDomain
 * @brief Process-wide hazard-pointer registry (Maged Michael, "Safe Memory
 *        Reclamation for Dynamic Lock-Free Objects Using Atomic Reads and
 *        Writes", PODC 2002 / expanded 2004).
 *
 * @details
 * The problem hazard pointers solve: in a lock-free structure built on raw
 * `atomic<Node*>`, a thread that has just read a `Node*` (e.g. via
 * `head.load()`) needs to dereference it a moment later. Between the read and
 * the dereference, another thread could unlink *and free* that same node,
 * leaving the first thread holding a dangling pointer (use-after-free), or —
 * worse — a different node could be allocated at the same address in the
 * meantime, producing the classic ABA problem where a CAS silently "succeeds"
 * against a node that isn't logically the one it was compared against.
 * `shared_ptr` sidesteps this via atomic refcounting, at the cost of the
 * spinlock-table implementation described in the file-level comment above.
 * Hazard pointers solve the same problem without ever taking a lock:
 *
 *  1. Before dereferencing a node it doesn't yet own, a thread **publishes**
 *     that node's address into one of its own hazard-pointer slots
 *     (`hazards[i].store(node, seq_cst)`), then **re-validates** that the
 *     node is still reachable from the structure (re-reads `head`/`tail`/etc
 *     and retries if it changed). This close-together store+reload pair
 *     (both `seq_cst`, which forbids the store/load reordering that a plain
 *     acquire/release pair would still permit) is what closes the race: any
 *     thread that might free the node is guaranteed to see the published
 *     hazard pointer before it can safely reclaim.
 *  2. A thread that logically removes a node never `delete`s it directly.
 *     It calls @ref retire, which appends the node to that thread's private
 *     retire list.
 *  3. Once a thread's retire list grows past a threshold, it scans every
 *     *active* hazard-pointer record in the whole process, unions all
 *     currently-published addresses into one set, and only `delete`s the
 *     retired nodes that are **not** in that set. Anything still hazarded is
 *     kept for the next scan.
 *
 * This bounds the number of not-yet-reclaimed nodes (unlike leaking forever)
 * while guaranteeing a node is never freed while any thread might still
 * dereference it — and every operation involved (`load`, `store`, CAS on the
 * hazard slots and on the registry's intrusive list) is itself lock-free.
 *
 * @note This is a compact, self-contained implementation intended for use by
 *       @ref LockFreeQueue. It is not a general-purpose replacement for
 *       `<experimental/hazard_pointer>` — in particular its retire-list scan
 *       is O(retired × active-threads) per scan, which is standard for a
 *       from-scratch hazard-pointer implementation and entirely adequate for
 *       a thread-pool-sized number of threads, but a production system with
 *       very large thread counts may prefer a more elaborate scheme (e.g.
 *       per-domain sharding).
 */
class HazardPointerDomain {
public:
    /// @brief Hazard-pointer slots reserved per thread. Two suffices for
    /// Michael–Scott: `dequeue` must simultaneously protect `first` (head)
    /// and `next` (the node whose data is being extracted).
    static constexpr std::size_t HAZARDS_PER_THREAD = 2;

    /// @brief One record per thread that has ever touched the queue, reused
    /// across a thread's lifetime and recyclable once a thread exits.
    struct HPRecord {
        std::atomic<void*> hazards[HAZARDS_PER_THREAD];
        std::atomic<HPRecord*> next{nullptr};
        std::atomic<bool> active{false};
    };

    /// @brief Returns the single process-wide domain instance.
    static HazardPointerDomain& instance() {
        static HazardPointerDomain domain;
        return domain;
    }

    /**
     * @brief Acquires an @ref HPRecord for the calling thread.
     *
     * Scans the intrusive registry list for a record some other, now-exited
     * thread released (`active == false`) and claims it via CAS; if none is
     * free, allocates a new one and lock-free-pushes it onto the list head.
     * Records are never freed once allocated — they are recycled — so the
     * registry list only ever grows to the high-water mark of concurrent
     * threads that have used the domain, and scans stay O(that mark).
     *
     * @return Pointer to a record owned by the calling thread until
     *         @ref releaseRecord is called on it.
     */
    HPRecord* acquireRecord() {
        HPRecord* rec = head_.load(std::memory_order_acquire);
        while (rec) {
            bool expected = false;
            if (!rec->active.load(std::memory_order_relaxed) &&
                rec->active.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                for (auto& h : rec->hazards) h.store(nullptr, std::memory_order_relaxed);
                return rec;
            }
            rec = rec->next.load(std::memory_order_acquire);
        }

        HPRecord* new_rec = new HPRecord();
        new_rec->active.store(true, std::memory_order_relaxed);
        for (auto& h : new_rec->hazards) h.store(nullptr, std::memory_order_relaxed);

        HPRecord* old_head = head_.load(std::memory_order_relaxed);
        do {
            new_rec->next.store(old_head, std::memory_order_relaxed);
        } while (!head_.compare_exchange_weak(old_head, new_rec,
                     std::memory_order_release, std::memory_order_relaxed));
        return new_rec;
    }

    /**
     * @brief Releases @p rec back to the pool for reuse by a future thread.
     *
     * Clears its hazard slots first so a concurrent @ref scanAndReclaim never
     * sees stale, no-longer-meaningful published addresses from this thread.
     */
    void releaseRecord(HPRecord* rec) noexcept {
        for (auto& h : rec->hazards) h.store(nullptr, std::memory_order_release);
        rec->active.store(false, std::memory_order_release);
    }

    /**
     * @brief Defers reclamation of @p p until no hazard pointer protects it.
     *
     * Adds @p p to the calling thread's private retire list; once that list
     * grows past @ref RETIRE_THRESHOLD entries, triggers a scan that frees
     * every retired node not currently published in any active hazard slot.
     *
     * @tparam T   Node type (deduced).
     * @param  p   Node to retire. Ownership transfers to the domain; the
     *             caller must not touch @p p again after this call.
     */
    template <typename T>
    void retire(T* p) {
        auto& list = retireList();
        list.ptrs.push_back(p);
        list.deleters.push_back([](void* q) { delete static_cast<T*>(q); });
        if (list.ptrs.size() >= RETIRE_THRESHOLD) {
            scanAndReclaim();
        }
    }

private:
    /// @brief Batch size that triggers a reclamation scan. Sized relative to
    /// a generous upper bound on concurrent threads so that, even at that
    /// bound, no more than a small multiple of (threads × HAZARDS_PER_THREAD)
    /// retired nodes are ever outstanding at once.
    static constexpr std::size_t MAX_EXPECTED_THREADS = 128;
    static constexpr std::size_t RETIRE_THRESHOLD = 2 * MAX_EXPECTED_THREADS * HAZARDS_PER_THREAD;

    struct RetireList {
        std::vector<void*> ptrs;
        std::vector<void (*)(void*)> deleters;
    };

    /// @brief Intrusive singly-linked list of every HPRecord ever allocated
    /// by this domain (active or recycled-but-inactive).
    std::atomic<HPRecord*> head_{nullptr};

    static RetireList& retireList() {
        static thread_local RetireList list;
        return list;
    }

    /**
     * @brief Frees every retired node in the calling thread's retire list
     *        that is not currently published by any active hazard record.
     */
    void scanAndReclaim() {
        auto& list = retireList();

        std::vector<void*> guarded;
        guarded.reserve(MAX_EXPECTED_THREADS * HAZARDS_PER_THREAD);
        HPRecord* rec = head_.load(std::memory_order_acquire);
        while (rec) {
            if (rec->active.load(std::memory_order_acquire)) {
                for (auto& h : rec->hazards) {
                    void* p = h.load(std::memory_order_acquire);
                    if (p) guarded.push_back(p);
                }
            }
            rec = rec->next.load(std::memory_order_acquire);
        }
        std::sort(guarded.begin(), guarded.end());

        std::vector<void*> stillRetired;
        std::vector<void (*)(void*)> stillDeleters;
        stillRetired.reserve(list.ptrs.size());
        stillDeleters.reserve(list.deleters.size());

        for (std::size_t i = 0; i < list.ptrs.size(); ++i) {
            if (std::binary_search(guarded.begin(), guarded.end(), list.ptrs[i])) {
                stillRetired.push_back(list.ptrs[i]);
                stillDeleters.push_back(list.deleters[i]);
            } else {
                list.deleters[i](list.ptrs[i]);
            }
        }
        list.ptrs.swap(stillRetired);
        list.deleters.swap(stillDeleters);
    }
};

/**
 * @brief RAII holder that acquires an @ref HazardPointerDomain::HPRecord on
 * first use by the calling thread and releases it back to the pool when the
 * thread exits.
 *
 * Meant to back a `thread_local` instance (see @ref localHPRecord) so every
 * thread — whether a pool worker or an external caller submitting/dequeuing
 * directly — gets exactly one record, lazily, without any explicit
 * registration step.
 */
class HazardPointerHolder {
public:
    HazardPointerHolder() : rec_(HazardPointerDomain::instance().acquireRecord()) {}
    ~HazardPointerHolder() { HazardPointerDomain::instance().releaseRecord(rec_); }
    HazardPointerDomain::HPRecord* record() const noexcept { return rec_; }

    HazardPointerHolder(const HazardPointerHolder&) = delete;
    HazardPointerHolder& operator=(const HazardPointerHolder&) = delete;

private:
    HazardPointerDomain::HPRecord* rec_;
};

/**
 * @brief Returns the calling thread's hazard-pointer record, creating it on
 *        first call.
 *
 * Implemented as a function-local `thread_local` so each thread's record is
 * initialised exactly once, lazily, and destroyed (releasing the record for
 * reuse) when that thread exits — mirroring the Meyers-singleton pattern
 * used for @ref getStaticThreadPool below, but per-thread instead of once
 * per process.
 */
inline HazardPointerDomain::HPRecord* localHPRecord() {
    static thread_local HazardPointerHolder holder;
    return holder.record();
}

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

// ─────────────────────────────────────────────────────────────────────────────
//  ThreadPool
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class ThreadPool
 * @brief Fully lock-free thread pool backed by C++20 atomic wait/notify.
 *
 * Manages a fixed set of worker threads that drain tasks from a @ref LockFreeQueue.
 * Task submission is done via @ref enqueue, which returns a `std::future` allowing
 * the caller to obtain the result (or re-throw exceptions) asynchronously.
 *
 * ### State encoding
 * A single 64-bit atomic `task_state` encodes two counters and a shutdown flag:
 * | Bits  | Meaning                                      |
 * |-------|----------------------------------------------|
 * | 63–32 | Pending tasks (enqueued, not yet picked up)   |
 * | 31    | Shutdown-requested flag, set by `shutdown()`  |
 * | 30–0  | Active tasks (currently executing)            |
 *
 * Workers park on `task_state.wait()` when no pending work is available and
 * are woken by `task_state.notify_one()` after each submission, or by
 * `task_state.notify_all()` once `shutdown()` sets the flag above.
 *
 * @par Fix: shutdown signalling
 * The shutdown flag used to be a separate `std::atomic<bool> stop`, with
 * `shutdown()` additionally overwriting `task_state` with placeholder values
 * purely to force any parked worker to wake up. That "destructive wakeup"
 * clobbered the real pending/active counts. Folding the flag into
 * `task_state` itself removes the need to touch the counters at all:
 * `task_state.wait(old)` is specified to return immediately if the atomic's
 * current value no longer equals `old` — so setting @ref STOP_BIT is
 * guaranteed to either be observed by a worker before it parks, or to make
 * its very next `wait()` call return immediately.
 *
 * @note `LockFreeQueue<MoveOnlyTask>` now reclaims nodes via hazard pointers
 * instead of `atomic<shared_ptr<Node>>` — see that class's doc comment. This
 * is transparent to `ThreadPool`; its interface (`enqueue`/`dequeue`) is
 * unchanged.
 *
 * ### Typical usage
 * @code
 *   ThreadPool pool(std::thread::hardware_concurrency());
 *
 *   auto f = pool.enqueue([](int x){ return x * x; }, 7);
 *   std::cout << f.get() << '\n'; // 49
 *
 *   pool.waitAllTasksCompleted();
 *   pool.shutdown();
 * @endcode
 *
 * @note The destructor calls @ref shutdown automatically, so explicit shutdown
 *       is only necessary when you need to drain the pool before destruction.
 */
class ThreadPool {
private:
    const size_t num_threads;

    alignas(64) std::atomic<uint64_t> task_state{0};

    static constexpr uint64_t PENDING_ONE = uint64_t(1) << 32;
    static constexpr uint64_t ACTIVE_ONE  = uint64_t(1);
    static constexpr uint64_t STOP_BIT    = uint64_t(1) << 31;
    static constexpr uint64_t ACTIVE_MASK = STOP_BIT - 1;

    std::vector<std::thread> workers;
    LockFreeQueue<MoveOnlyTask> task_queue;
    alignas(64) std::atomic<uint64_t> global_waiters{0};

    static uint32_t pendingFromState(uint64_t s) noexcept { return static_cast<uint32_t>(s >> 32); }
    static uint32_t activeFromState(uint64_t s) noexcept  { return static_cast<uint32_t>(s & ACTIVE_MASK); }
    static bool stopFromState(uint64_t s) noexcept { return (s & STOP_BIT) != 0; }

    void runTask(MoveOnlyTask& task) {
        if (task) {
            try { task(); } catch (...) {}
        }

        uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);

        if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
            task_state.notify_all();
        }
    }

    void workerThread() {
        while (true) {
            uint64_t current_state = task_state.load(std::memory_order_acquire);

            if (stopFromState(current_state)) {
                return;
            }

            while (pendingFromState(current_state) == 0) {
                task_state.wait(current_state, std::memory_order_relaxed);
                current_state = task_state.load(std::memory_order_acquire);
                if (stopFromState(current_state)) return;
            }

            uint64_t expected = current_state;
            uint64_t desired = current_state - PENDING_ONE + ACTIVE_ONE;

            if (!task_state.compare_exchange_weak(expected, desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                #if defined(__x86_64__) || defined(_M_X64)
                    __builtin_ia32_pause();
                #else
                    std::this_thread::yield();
                #endif
                continue;
            }

            MoveOnlyTask task;
            if (task_queue.dequeue(task)) {
                runTask(task);
            } else {
                uint64_t prev = task_state.fetch_add(PENDING_ONE - ACTIVE_ONE, std::memory_order_acq_rel);
                if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
                    task_state.notify_all();
                }
                std::this_thread::yield();
            }
        }
    }

public:
    explicit ThreadPool(size_t n) : num_threads(n) {
        if (n == 0) throw std::invalid_argument("ThreadPool: n > 0 required");
        workers.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            workers.emplace_back(&ThreadPool::workerThread, this);
        }
    }

    ~ThreadPool() {
        shutdown();
    }

    template <class F, class... Args>
    auto enqueue(F&& f, Args&&... args) -> std::future<std::invoke_result_t<F, Args...>> {
        using return_type = std::invoke_result_t<F, Args...>;

        auto promise = std::make_shared<std::promise<return_type>>();
        std::future<return_type> result = promise->get_future();

        task_queue.enqueue(MoveOnlyTask([
            func = std::forward<F>(f),
            tup  = std::make_tuple(std::forward<Args>(args)...),
            p    = std::move(promise)]() mutable
        {
            try {
                if constexpr (std::is_void_v<return_type>) {
                    std::apply(std::move(func), std::move(tup));
                    p->set_value();
                } else {
                    p->set_value(std::apply(std::move(func), std::move(tup)));
                }
            } catch (...) {
                p->set_exception(std::current_exception());
            }
        }));

        task_state.fetch_add(PENDING_ONE, std::memory_order_release);
        task_state.notify_one();

        return result;
    }

    void waitAllTasksCompleted() {
        uint64_t current_state = task_state.load(std::memory_order_acquire);
        while (pendingFromState(current_state) != 0 || activeFromState(current_state) != 0) {
            task_state.wait(current_state, std::memory_order_relaxed);
            current_state = task_state.load(std::memory_order_acquire);
        }
    }

    void shutdown() {
        task_state.fetch_or(STOP_BIT, std::memory_order_release);
        task_state.notify_all();

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        MoveOnlyTask abandoned_task;
        while (task_queue.dequeue(abandoned_task)) {
            task_state.fetch_sub(PENDING_ONE, std::memory_order_acq_rel);
        }
        task_state.notify_all();
    }

    bool isIdle() const {
        uint64_t s = task_state.load(std::memory_order_acquire);
        return pendingFromState(s) == 0 && activeFromState(s) == 0;
    }

    size_t threadCount() const { return num_threads; }
    uint64_t pendingCount() const { return pendingFromState(task_state.load(std::memory_order_acquire)); }
    uint64_t activeCount() const { return activeFromState(task_state.load(std::memory_order_acquire)); }
};

/**
 * @brief Thread-safe retrieval of the process-wide ThreadPool singleton.
 */
inline ThreadPool& getStaticThreadPool() {
    unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());
    static ThreadPool instance(
        std::min({ static_cast<size_t>(maxThreads),
                   GlobalConcurrency::MAX_USEFUL_THREADS }));
    return instance;
}

#endif // THREAD_POOL_H
