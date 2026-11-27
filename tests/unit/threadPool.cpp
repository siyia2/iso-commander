// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks concurrent task execution, callable ownership, futures and shutdown.
 */

#include "../common/testHarness.h"

import isocmd.threadpool.threadpool;
import isocmd.threadpool.moveOnlyTask;

namespace isocmd_tests {

	namespace {

		// A move-constructed callable remains a live object until its destructor runs.
		// Exercise both storage paths, independently of sanitizer instrumentation.
		template <std::size_t Padding>
		struct LifetimeCallable {
			int& alive;
			int& calls;
			std::array<std::byte, Padding> padding{};

			LifetimeCallable(int& a, int& c) noexcept : alive(a), calls(c) {

				++alive;

			}

			LifetimeCallable(LifetimeCallable&& other) noexcept
			    : alive(other.alive), calls(other.calls) {

				++alive;

			}

			LifetimeCallable(const LifetimeCallable&) = delete;

			~LifetimeCallable() {

				--alive;

			}

			void operator()() {

				++calls;

			}
		};

		// An rvalue can invoke a noexcept copy constructor instead of a move.
		// The moved-from object must still release its ownership.
		struct CopyOnlyCallable {
			std::shared_ptr<int> owner;

			explicit CopyOnlyCallable(const std::shared_ptr<int>& p) : owner(p) {}

			CopyOnlyCallable(const CopyOnlyCallable&) noexcept = default;

			void operator()() {

				++*owner;

			}
		};

		// This callable fits inline, but relocating it may throw. The wrapper must
		// move its heap pointer instead, preserving its noexcept move contract.
		struct ThrowingMoveCallable {
			int& moves;

			explicit ThrowingMoveCallable(int& n) : moves(n) {}

			ThrowingMoveCallable(ThrowingMoveCallable&& other) : moves(other.moves) {

				if (++moves > 1) throw std::runtime_error("unexpected callable relocation");

			}

			void operator()() {}
		};

		template <std::size_t Padding>
		void checkTaskLifetime() {

			int alive = 0, calls = 0;
			{
				MoveOnlyTask first{LifetimeCallable<Padding>{alive, calls}};
				CHECK(alive == 1);
				MoveOnlyTask second(std::move(first));
				CHECK(!first && second && alive == 1);
				MoveOnlyTask third{LifetimeCallable<Padding>{alive, calls}};
				CHECK(alive == 2);
				third = std::move(second);
				CHECK(!second && third && alive == 1);
				third();
				CHECK(calls == 1);
				third.reset();
				CHECK(!third && alive == 0);
				third.reset();
			}
			CHECK(alive == 0);

		}

	} // namespace

	void taskLifetime() {

		checkTaskLifetime<1>();   // inline storage
		checkTaskLifetime<256>(); // heap storage

		auto owner = std::make_shared<int>(0);
		{
			MoveOnlyTask first{CopyOnlyCallable{owner}};
			CHECK(owner.use_count() == 2);
			MoveOnlyTask second(std::move(first));
			CHECK(!first && owner.use_count() == 2);
			MoveOnlyTask third;
			third = std::move(second);
			CHECK(!second && owner.use_count() == 2);
			third();
			CHECK(*owner == 1);
		}
		CHECK(owner.use_count() == 1);

		int moves = 0;
		MoveOnlyTask first{ThrowingMoveCallable{moves}};
		MoveOnlyTask second(std::move(first));
		CHECK(!first && second && moves == 1);
		first = std::move(second);
		CHECK(first && !second && moves == 1);

		// Reproduce the const-vector capture pattern in executeFileOperations.
		const std::vector<int> indices{1, 2, 3};
		ThreadPool pool(2);
		for (int i = 0; i < 64; ++i) {
			pool.enqueue([indices] { CHECK(indices.size() == 3 && indices[2] == 3); }).get();
		}
		pool.shutdown();

	}

	static void checkConcurrentSubmission(ThreadPool& pool) {

		constexpr int producers = 4, perProducer = 2000;

		std::vector<std::atomic<int>> visits(producers * perProducer);
		std::vector<std::vector<std::future<int>>> futures(producers);
		std::vector<std::jthread> submitters;

		for (int p = 0; p < producers; ++p) {
			submitters.emplace_back([&, p] {

				for (int j = 0; j < perProducer; ++j) {
					int id = p * perProducer + j;
					futures[p].push_back(pool.enqueue([&, id] {

						++visits[id];
						return id;

					}));
				}

			});
		}

		submitters.clear(); // jthread joins
		pool.waitAllTasksCompleted();

		for (int p = 0; p < producers; ++p) {
			for (int j = 0; j < perProducer; ++j) {
				int id = p * perProducer + j;
				CHECK(futures[p][j].get() == id &&
				        visits[id].load() == 1);
			}
		}

		CHECK(pool.isIdle() &&
		        pool.pendingCount() == 0 &&
		        pool.activeCount() == 0);

	}

	static void checkFutureTypes(ThreadPool& pool) {

		// Transfer a move-only argument to a bound callable.
		CHECK(pool.enqueue(
		    [p = std::make_unique<int>(42)]() mutable {

				auto consume = [](std::unique_ptr<int> value) { return *value; };
				return consume(std::move(p));

			}).get() == 42);

		// Move-only callable captures.
		CHECK(pool.enqueue(
		    [p = std::make_unique<int>(17)] {

				return *p;

			}).get() == 17);

		// Reference-returning futures.
		int value = 5;
		CHECK(&pool.enqueue([&]() -> int& {

			return value;

		}).get() == &value);

		// Void-returning futures.
		pool.enqueue([&] { value = 9; }).get();
		CHECK(value == 9);

		// Nested task submission.
		auto nested = pool.enqueue([&] {

			return pool.enqueue([] { return 123; });

		});
		CHECK(nested.get().get() == 123);

	}

	static void checkExceptionShutdown(ThreadPool& pool) {

		// Exception propagation for non-void and void futures.
		auto err = pool.enqueue([]() -> int {

			throw std::runtime_error("test exception");

		});

		auto voidErr = pool.enqueue([] {

			throw std::logic_error("void exception");

		});

		// Ensure both exceptions have been published before shutdown.
		err.wait();
		voidErr.wait();

		pool.waitAllTasksCompleted();

		// Join workers before inspecting exception objects to avoid
		// suspected TSan false positives in libstdc++ exception_ptr
		// reference counting during concurrent promise destruction.
		pool.shutdown();

		// Verify repeated shutdown is safe.
		pool.shutdown();

		bool thrown = false;
		try {
			(void)err.get();
		} catch (const std::runtime_error& e) {
			thrown = std::string_view(e.what()) == "test exception";
		}
		CHECK(thrown);

		thrown = false;
		try {
			voidErr.get();
		} catch (const std::logic_error& e) {
			thrown = std::string_view(e.what()) == "void exception";
		}
		CHECK(thrown);

		CHECK(pool.isIdle());

	}

	static void checkPendingShutdown() {

		// After shutdown all queued futures must be either completed or broken,
		// never left waiting indefinitely. Running work may finish before STOP_BIT.
		for (int round = 0; round < 20; ++round) {
			ThreadPool pool(2);
			std::vector<std::future<int>> pending;

			for (int i = 0; i < 500; ++i) {
				pending.push_back(pool.enqueue([i] { return i; }));
			}

			pool.shutdown();
			CHECK(pool.isIdle());

			for (int i = 0; i < 500; ++i) {
				CHECK(pending[i].wait_for(std::chrono::seconds(0)) ==
				        std::future_status::ready);

				try {
					CHECK(pending[i].get() == i);
				} catch (const std::future_error& e) {
					CHECK(e.code() ==
					        std::make_error_code(std::future_errc::broken_promise));
				}
			}
		}

	}

	void threadPool() {

		bool rejected = false;
		try {
			ThreadPool invalid(0);
		} catch (const std::invalid_argument&) {
			rejected = true;
		}
		CHECK(rejected);

		for (std::size_t workers : {1, 2, 4, 8, 16}) {
			ThreadPool pool(workers);
			CHECK(pool.threadCount() == workers);

			checkConcurrentSubmission(pool);
			checkFutureTypes(pool);
			checkExceptionShutdown(pool);
		}

		checkPendingShutdown();

	}

} // namespace isocmd_tests
