// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdint.h>

import std;
import isocmd.core.threadCaps;
import isocmd.threadpool.moveOnlyTask;
import isocmd.threadpool.threadpool;

namespace GlobalThreadCaps {
	std::size_t MAX_USEFUL_THREADS = 16;
	std::size_t CPMV_THREAD_CAP   = 4;
	std::size_t CONV_THREAD_CAP   = 4;
	std::size_t MOUNT_THREAD_CAP  = 8;
	std::size_t CLEAN_THREAD_CAP  = 4;
	std::size_t UMOUNT_THREAD_CAP = 8;
	std::size_t RM_THREAD_CAP     = 8;
	std::size_t SORT_THREAD_CAP   = 2;
	std::size_t FILTER_THREAD_CAP = 2;
}

namespace {

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
		static void deleteAs(void* q) {

			delete static_cast<T*>(q);

		}

		template <typename T>
		void retire(T* p) {

			auto& list = retireList();
			list.ptrs.push_back(p);
			list.deleters.push_back(&deleteAs<T>);
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

		/**
		 * @brief Per-thread retire list.
		 *
		 * @par Thread-exit leak fix
		 * A thread that retires only a handful of nodes (fewer than
		 * @ref RETIRE_THRESHOLD) and then exits would, under the original
		 * design, simply drop that list — the pointers it held were never
		 * freed, because nothing but @ref retire's threshold check ever
		 * triggered a scan. Confirmed by AddressSanitizer's LeakSanitizer:
		 * short-lived-thread-churn workloads (many threads each touching the
		 * queue once or twice, well below threshold, then exiting) leaked
		 * hundreds of KB of retired nodes in testing.
		 *
		 * The destructor below closes that gap: at thread-exit time it forces
		 * one last reclamation attempt on whatever remains in this thread's
		 * list, using the exact same guarded-address scan as
		 * @ref scanAndReclaim. Anything that scan still finds hazarded by some
		 * other, still-running thread (a legitimate, if rare, possibility even
		 * at the exact moment this thread is exiting) is handed off to a
		 * process-wide orphan list rather than dropped — @ref scanAndReclaim
		 * always folds that orphan list into its own next scan, so an orphaned
		 * node is guaranteed to be freed the next time *any* thread in the
		 * process crosses its own threshold, rather than being lost.
		 */
		struct RetireList {
			std::vector<void*> ptrs;
			std::vector<void (*)(void*)> deleters;

			~RetireList() {

				if (!ptrs.empty()) {
					HazardPointerDomain::instance().reclaimOnThreadExit(ptrs, deleters);
				}

			}
		};

		/// @brief Intrusive singly-linked list of every HPRecord ever allocated
		/// by this domain (active or recycled-but-inactive).
		std::atomic<HPRecord*> head_{nullptr};

		/**
		 * @brief Singly-linked-list node used only for the orphan stack below.
		 * Distinct from the queue's own `Node` — this one belongs entirely to
		 * the reclamation machinery and is never hazard-protected itself: once
		 * a thread's @ref drainOrphans call atomically swaps @ref orphan_head_
		 * to `nullptr`, that thread holds the *only* reference to the whole
		 * chain, so no other thread can ever be mid-dereference of an
		 * OrphanNode. That's what makes exchange-based draining safe without
		 * hazard pointers of its own, and immune to the classic Treiber-stack
		 * pop ABA hazard (which arises from popping node-by-node while another
		 * thread might free-and-reuse the node in between reading it and
		 * CASing on it — draining the entire chain in one exchange has no such
		 * window).
		 */
		struct OrphanNode {
			void* ptr;
			void (*deleter)(void*);
			OrphanNode* next;
		};

		/// @brief Lock-free Treiber stack of nodes left behind by a thread that
		/// force-reclaimed on exit (see @ref RetireList's destructor) but found
		/// some still hazarded by another live thread at that moment.
		std::atomic<OrphanNode*> orphan_head_{nullptr};

		static RetireList& retireList() {

			static thread_local RetireList list;
			return list;

		}

		/**
		 * @brief Collects every address currently published in any active
		 *        hazard-pointer slot, process-wide.
		 * @return Sorted vector suitable for `std::binary_search`.
		 */
		std::vector<void*> collectGuardedAddresses() {

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
			return guarded;

		}

		/**
		 * @brief Publishes a batch of leftover (ptr, deleter) pairs onto the
		 * orphan stack in a single CAS, regardless of batch size.
		 *
		 * Builds the whole chain locally first (not yet visible to any other
		 * thread), then attaches it in front of whatever @ref orphan_head_
		 * currently points to with one `compare_exchange_weak` loop — a
		 * standard batched Treiber push.
		 */
		void pushOrphans(const std::vector<void*>& ptrs, const std::vector<void (*)(void*)>& deleters) {

			OrphanNode* first = nullptr;
			OrphanNode* last = nullptr;
			for (std::size_t i = 0; i < ptrs.size(); ++i) {
				OrphanNode* node = new OrphanNode{ptrs[i], deleters[i], nullptr};
				if (!first) { first = last = node; }
				else { last->next = node; last = node; }
			}
			if (!first) return;

			OrphanNode* old_head = orphan_head_.load(std::memory_order_relaxed);
			do {
				last->next = old_head;
			} while (!orphan_head_.compare_exchange_weak(old_head, first,
			             std::memory_order_release, std::memory_order_relaxed));

		}

		/**
		 * @brief Atomically takes ownership of the entire orphan stack and
		 * appends its contents to @p target_ptrs / @p target_deleters, freeing
		 * the (now exclusively-owned) OrphanNode wrappers as it walks the chain.
		 */
		void adoptOrphans(std::vector<void*>& target_ptrs, std::vector<void (*)(void*)>& target_deleters) {

			OrphanNode* node = orphan_head_.exchange(nullptr, std::memory_order_acquire);
			while (node) {
				target_ptrs.push_back(node->ptr);
				target_deleters.push_back(node->deleter);
				OrphanNode* next = node->next;
				delete node;
				node = next;
			}

		}

		/**
		 * @brief Forces a final reclamation pass for a thread's leftover retired
		 *        list at thread-exit time, called from @ref RetireList's
		 *        destructor. Anything still hazarded goes to the orphan stack.
		 */
		void reclaimOnThreadExit(std::vector<void*>& ptrs, std::vector<void (*)(void*)>& deleters) {

			std::vector<void*> guarded = collectGuardedAddresses();

			std::vector<void*> leftover_ptrs;
			std::vector<void (*)(void*)> leftover_deleters;
			for (std::size_t i = 0; i < ptrs.size(); ++i) {
				if (std::binary_search(guarded.begin(), guarded.end(), ptrs[i])) {
					leftover_ptrs.push_back(ptrs[i]);
					leftover_deleters.push_back(deleters[i]);
				} else {
					deleters[i](ptrs[i]);
				}
			}
			pushOrphans(leftover_ptrs, leftover_deleters);

		}

		/**
		 * @brief Frees every retired node in the calling thread's retire list
		 *        that is not currently published by any active hazard record.
		 *
		 * Also adopts and attempts to reclaim anything sitting in the global
		 * orphan stack (see @ref reclaimOnThreadExit), so nodes left behind by
		 * an exited thread are guaranteed to eventually be freed by whichever
		 * thread next crosses its own @ref RETIRE_THRESHOLD.
		 */
		void scanAndReclaim() {

			auto& list = retireList();

			adoptOrphans(list.ptrs, list.deleters);

			std::vector<void*> guarded = collectGuardedAddresses();

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

		~HazardPointerHolder() {

			HazardPointerDomain::instance().releaseRecord(rec_);

		}

		HazardPointerDomain::HPRecord* record() const noexcept {

			return rec_;

		}

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

} // namespace

// Private queue implementation: unchanged Michael–Scott CAS and hazard protocol.
// Each dereference of a shared node is protected by a published hazard and
// revalidation of the queue pointer. The sentinel is reclaimed only after
// all hazard slots cease referencing it. No extra allocation or indirection
// is introduced into the embedded queue.
struct ThreadPool::TaskQueue::Node {
	MoveOnlyTask data;
	std::atomic<Node*> next;

	Node() : data(), next(nullptr) {}

	Node(MoveOnlyTask&& val) : data(std::move(val)), next(nullptr) {}
};

inline void ThreadPool::TaskQueue::pause_processor() noexcept {

    #if defined(__x86_64__) || defined(_M_X64)
		__builtin_ia32_pause();
    #else
		std::this_thread::yield();
    #endif

}

ThreadPool::TaskQueue::TaskQueue() {

	Node* dummy = new Node();
	head.store(dummy, std::memory_order_relaxed);
	tail.store(dummy, std::memory_order_relaxed);

}

ThreadPool::TaskQueue::~TaskQueue() {

	Node* node = head.load(std::memory_order_relaxed);
	while (node) {
		Node* next = node->next.load(std::memory_order_relaxed);
		delete node;
		node = next;
	}

}

void ThreadPool::TaskQueue::enqueuePrepared(MoveOnlyTask&& value) {

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

bool ThreadPool::TaskQueue::dequeue(MoveOnlyTask& result) {

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

/**
 * @brief Publishes an already type-erased task and wakes a worker.
 *
 * Kept out of enqueue() so its callable-specific instantiations do not each
 * compile the lock-free queue insertion and atomic notification machinery.
 */
void ThreadPool::submitTask(MoveOnlyTask task) {

	task_queue.enqueuePrepared(std::move(task));
	task_state.fetch_add(PENDING_ONE, std::memory_order_release);
	task_state.notify_one();

}

/**
 * @brief Executes one dequeued task and releases its "active" slot.
 *
 * Runs @p task, swallowing any exception (tasks submitted through
 * enqueue() already route exceptions into their std::promise, so nothing
 * should escape; this keeps a stray throw from killing a worker thread).
 * Afterwards the active counter in @c task_state is decremented, and if
 * that left the pool with no active and no pending work, all waiters
 * (e.g. in waitAllTasksCompleted()) are woken.
 *
 * @param task The task to run; may be empty, in which case only the
 *             counter bookkeeping is performed.
 * @pre The caller has already moved one unit from "pending" to "active"
 *      in @c task_state for this task.
 */
void ThreadPool::runTask(MoveOnlyTask &task) {

	if (task) {
		try {
			task();
		} catch (...) {
		}
	}

	uint64_t prev = task_state.fetch_sub(ACTIVE_ONE, std::memory_order_acq_rel);

	if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
		task_state.notify_all();
	}

}

/**
 * @brief Main loop of every worker thread.
 *
 * Each iteration:
 *  1. Returns if the stop bit is set in @c task_state.
 *  2. Parks on @c task_state.wait() while no task is pending, re-checking
 *     the stop bit after every wakeup.
 *  3. Claims a task by CAS-ing @c task_state from "pending" to "active"
 *     (pause/yield and retry if another worker won the race).
 *  4. Dequeues and runs the task via runTask(). If the queue is
 *     unexpectedly empty (the enqueuer bumped the counter before the node
 *     became visible), the claim is rolled back, waiters are notified if
 *     the pool became idle, and the thread yields before retrying.
 *
 * Lock-free: all coordination goes through the single atomic @c task_state.
 */
void ThreadPool::workerThread() {

	while (true) {
		uint64_t current_state = task_state.load(std::memory_order_acquire);

		if (stopFromState(current_state)) {
			return;
		}

		while (pendingFromState(current_state) == 0) {
			task_state.wait(current_state, std::memory_order_relaxed);
			current_state = task_state.load(std::memory_order_acquire);
			if (stopFromState(current_state))
			    return;
		}

		uint64_t expected = current_state;
		uint64_t desired = current_state - PENDING_ONE + ACTIVE_ONE;

		if (!task_state.compare_exchange_weak(expected, desired, std::memory_order_acq_rel,
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
			uint64_t prev =
			    task_state.fetch_add(PENDING_ONE - ACTIVE_ONE, std::memory_order_acq_rel);
			if (activeFromState(prev) == 1 && pendingFromState(prev) == 0) {
				task_state.notify_all();
			}
			std::this_thread::yield();
		}
	}

}

/**
 * @brief Constructs the pool and starts @p n worker threads.
 *
 * @param n Number of worker threads to spawn.
 * @throws std::invalid_argument if @p n is zero.
 */
ThreadPool::ThreadPool(std::size_t n) : num_threads(n) {

	if (n == 0)
	    throw std::invalid_argument("ThreadPool: n > 0 required");
	workers.reserve(n);
	for (std::size_t i = 0; i < n; ++i) {
		workers.emplace_back(&ThreadPool::workerThread, this);
	}

}

/**
 * @brief Stops the pool: calls shutdown(), joining all workers.
 */
ThreadPool::~ThreadPool() {

	shutdown();

}

/**
 * @brief Blocks until no task is pending or running.
 *
 * Sleeps on @c task_state.wait() and re-evaluates after each notification.
 * Does not stop the pool; new tasks may still be enqueued afterwards.
 *
 * @note Returns once the pool is idle at the moment of the check; tasks
 *       enqueued concurrently by other threads may still arrive later.
 */
void ThreadPool::waitAllTasksCompleted() {

	uint64_t current_state = task_state.load(std::memory_order_acquire);
	while (pendingFromState(current_state) != 0 || activeFromState(current_state) != 0) {
		task_state.wait(current_state, std::memory_order_relaxed);
		current_state = task_state.load(std::memory_order_acquire);
	}

}

/**
 * @brief Stops all workers and discards tasks that never started.
 *
 * Sets the stop bit in @c task_state and wakes every parked worker, then
 * joins them. Tasks already running finish; tasks still queued are
 * dequeued and dropped (their pending count is subtracted), so their
 * futures never become ready. Safe to call more than once.
 */
void ThreadPool::shutdown() {

	task_state.fetch_or(STOP_BIT, std::memory_order_release);
	task_state.notify_all();

	for (std::thread &worker : workers) {
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

/**
 * @brief Returns the process-wide ThreadPool singleton.
 *
 * Created on first use (thread-safe function-local static) with
 * `max(2, hardware_concurrency())` workers, capped at
 * GlobalThreadCaps::MAX_USEFUL_THREADS.
 *
 * @return Reference to the shared pool; valid until program exit.
 */
ThreadPool &getStaticThreadPool() {

	unsigned int maxThreads = std::max(2u, std::thread::hardware_concurrency());
	static ThreadPool instance(
	    std::min({static_cast<std::size_t>(maxThreads), GlobalThreadCaps::MAX_USEFUL_THREADS}));
	return instance;

}
