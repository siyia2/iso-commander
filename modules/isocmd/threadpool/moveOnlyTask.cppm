// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.threadpool.moveOnlyTask;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- threadpool/moveOnlyTask ----

// ─────────────────────────────────────────────────────────────────────────────
//  MoveOnlyTask
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @class MoveOnlyTask
 * @brief A move-only, type-erased callable wrapper with small-buffer optimisation.
 *
 * Stores any callable (function pointer, lambda, `std::bind` expression, …) that
 * accepts zero arguments and returns `void`.  The callable is held inside an
 * inline storage buffer of @ref StorageSize bytes when size, alignment and
 * non-throwing move construction permit; otherwise it
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
    alignas(std::max_align_t) std::byte storage[StorageSize];

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
        if constexpr (sizeof(DecayedF) <= StorageSize &&
                      alignof(DecayedF) <= alignof(std::max_align_t) &&
                      std::is_nothrow_move_constructible_v<DecayedF>) {
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
            // Moving constructs a new callable but does not end the source
            // callable's lifetime. Destroy it before clearing its VTable.
            other.reset();
        }
    }
};

} // extern "C++"
