// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

// All POSIX headers must precede the shared standard-library import.
#include "testHarness.h"

// Test-only GNU linker wrappers. A rule matches one explicitly registered inode
// (and device identity), never an arbitrary descriptor or a path prefix.
namespace isocmd_tests::io_faults {

    enum class Call { Read, Write, Pwrite, Fsync, SyncRange, Fallocate, Truncate, Close, Count };
    enum class Kind { Pass, Error, Zero, Short, CancelAfterShort, UnalignedShort, CorruptRead };
    struct Step {
        Kind kind = Kind::Pass;
        int error = 0;
        std::size_t bytes = 0;
        void (*cancel)() = nullptr;
    };
    inline Step error(int code) { return {Kind::Error, code}; }
    inline Step zero() { return {Kind::Zero}; }
    inline Step corruptRead() { return {Kind::CorruptRead}; }
    inline Step shortTransfer(std::size_t n) { return {Kind::Short, 0, n}; }
    // Emulate a kernel reporting an unaligned count from an aligned O_DIRECT
    // request. Commit one complete sector, then rewind its final byte so the
    // reported count and descriptor offset agree. The last byte may already have
    // reached storage; a successful restart must overwrite it correctly.
    inline Step unalignedDirectShort(std::size_t sector) { return {Kind::UnalignedShort, 0, sector}; }
    inline Step cancelAfterShort(std::size_t n, void (*cancel)()) {
        return {Kind::CancelAfterShort, 0, n, cancel};
    }

    struct Identity {
        struct stat value{};
        explicit Identity(int fd) {
            if (fstat(fd, &value) != 0) throw std::runtime_error("fault target fstat failed");
            if (!S_ISREG(value.st_mode) && !S_ISBLK(value.st_mode))
                throw std::runtime_error("fault target must be a regular file or verified block device");
        }
        bool matches(int fd) const noexcept;
    };

    struct Rule {
        std::optional<Identity> target;
        std::vector<Step> steps;
        std::atomic<std::size_t> calls{0};
        // Count simulated results that require an actual successful syscall first.
        std::atomic<std::size_t> effects{0};
        void set(const Identity& identity, std::initializer_list<Step> sequence) {
            target = identity;
            steps = sequence;
            calls = 0;
            effects = 0;
        }
        bool consumed() const { return calls.load() >= steps.size(); }
    };

    struct Plan {
        std::array<Rule, static_cast<std::size_t>(Call::Count)> rules;
        Rule& operator[](Call op) { return rules[static_cast<std::size_t>(op)]; }
    };

    // Configure the plan before installing it. All writer threads must have joined
    // before its scope ends. Counters are atomic; rules are immutable while active.
    class Scope {
    public:
        explicit Scope(Plan& plan);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

} // namespace isocmd_tests::io_faults
