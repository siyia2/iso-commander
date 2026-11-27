// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.threadpool.threadpool;

import std;
import isocmd.threadpool.moveOnlyTask;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @file threadpool.cppm
 * @brief Worker threads, task submission and the shared task queue.
 *
 * MoveOnlyTask stores jobs that cannot be copied. TaskQueue is a Michael–Scott
 * first-in, first-out queue that uses atomic pointers instead of a queue mutex.
 * Hazard pointers (see threadpool.cpp) prevent a removed node from being freed
 * while another thread may still use it. Workers sleep with atomic wait/notify
 * when there is no work.
 */

// Common future-state operations for void tasks, implemented once in taskPromise.cpp.
namespace threadpool_detail {
	std::shared_ptr<std::promise<void>> preparePromise(std::future<void>& result);
	void completePromise(std::promise<void>& promise);
	void failPromise(std::promise<void>& promise);

	template <typename R>
	std::shared_ptr<std::promise<R>> preparePromise(std::future<R>& result) {

		auto promise = std::make_shared<std::promise<R>>();
		result = promise->get_future();
		return promise;

	}
}

/**
 * @brief Runs submitted jobs on a fixed number of worker threads.
 *
 * enqueue() returns a future: the caller can wait for the result or receive
 * an exception thrown by the job. The destructor shuts down the pool.
 *
 * A single atomic 64-bit state tracks pending jobs (bits 63–32), the shutdown
 * flag (bit 31) and running jobs (bits 30–0). Updating the shutdown flag in
 * this same value wakes waiting workers without losing either count.
 * Workers wait when no work is pending and are notified on submission or shutdown.
 *
 * @code
 * ThreadPool pool(4);
 * auto result = pool.enqueue([x = 7] { return x * x; });
 * // result.get() returns 49.
 * pool.waitAllTasksCompleted();
 * @endcode
 */
class ThreadPool {
private:
	const std::size_t num_threads;

	alignas(64) std::atomic<std::uint64_t> task_state{0};

	static constexpr std::uint64_t PENDING_ONE = std::uint64_t(1) << 32;
	static constexpr std::uint64_t ACTIVE_ONE  = std::uint64_t(1);
	static constexpr std::uint64_t STOP_BIT    = std::uint64_t(1) << 31;
	static constexpr std::uint64_t ACTIVE_MASK = STOP_BIT - 1;

	std::vector<std::thread> workers;
	/**
	 * @brief Embedded Michael–Scott FIFO for MoveOnlyTask, reclaimed via hazard pointers.
	 *
	 * Only the layout and declarations are needed by pool importers. Node
	 * construction, CAS loops and reclamation live in threadpool.cpp.
	 */
	class TaskQueue {
		struct Node;
		alignas(64) std::atomic<Node*> head;
		alignas(64) std::atomic<Node*> tail;
		static void pause_processor() noexcept;
	public:
		TaskQueue();
		~TaskQueue();
		TaskQueue(const TaskQueue&) = delete;
		TaskQueue& operator=(const TaskQueue&) = delete;
		void enqueuePrepared(MoveOnlyTask&& value);
		bool dequeue(MoveOnlyTask& result);
	};

	TaskQueue task_queue;

	static std::uint32_t pendingFromState(std::uint64_t s) noexcept {

		return static_cast<std::uint32_t>(s >> 32);

	}

	static std::uint32_t activeFromState(std::uint64_t s) noexcept {

		return static_cast<std::uint32_t>(s & ACTIVE_MASK);

	}

	static bool stopFromState(std::uint64_t s) noexcept {

		return (s & STOP_BIT) != 0;

	}

	void submitTask(MoveOnlyTask task);

	void runTask(MoveOnlyTask& task);

	void workerThread();

public:
	explicit ThreadPool(std::size_t n);

	~ThreadPool();

	// Submit a bound, zero-argument callable; capture any required arguments.
	template <class F>
	auto enqueue(F&& f) -> std::future<std::invoke_result_t<F>> {

		using return_type = std::invoke_result_t<F>;

		std::future<return_type> result;
		auto promise = threadpool_detail::preparePromise(result);

		submitTask(MoveOnlyTask([
		    func = std::forward<F>(f),
		    p    = std::move(promise)]() mutable
		{

			try {
				if constexpr (std::is_void_v<return_type>) {
					std::move(func)();
					threadpool_detail::completePromise(*p);
				} else {
					p->set_value(std::move(func)());
				}
			} catch (...) {
				if constexpr (std::is_void_v<return_type>) {
					threadpool_detail::failPromise(*p);
				} else {
					p->set_exception(std::current_exception());
				}
			}

		}));

		return result;

	}

	void waitAllTasksCompleted();

	void shutdown();

	bool isIdle() const {

		std::uint64_t s = task_state.load(std::memory_order_acquire);
		return pendingFromState(s) == 0 && activeFromState(s) == 0;

	}

	std::size_t threadCount() const {

		return num_threads;

	}

	std::uint64_t pendingCount() const {

		return pendingFromState(task_state.load(std::memory_order_acquire));

	}

	std::uint64_t activeCount() const {

		return activeFromState(task_state.load(std::memory_order_acquire));

	}
};

/**
 * @brief Thread-safe retrieval of the process-wide ThreadPool singleton.
 *        Defined in src/threadpool/threadpool.cpp.
 */
ThreadPool& getStaticThreadPool();

} // extern "C++"
