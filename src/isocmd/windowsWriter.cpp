// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Third-Party Library Headers
#include <libmount/libmount.h>
#include <blkid/blkid.h>

// Project Headers
#include "../state.h"
#include "../write2usbUI.h"
#include "../write2usb.h"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Global scope: device readiness (public API, no private dependencies)
// ---------------------------------------------------------------------------

/**
 * @brief Checks if a block device is accessible and ready for I/O
 *        operations.
 *
 * Attempts to open the device node in read-only and non-blocking mode. This
 * effectively tests if the kernel has fully registered the block device and
 * that it is not currently locked or in an invalid state.
 *
 * @param path The filesystem path to the device node (e.g., "/dev/sdb1").
 * @return true if the device can be opened successfully.
 * @return false if the device does not exist, access is denied, or it is
 *         busy.
 */
bool isDeviceReady(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) return false;
    close(fd);
    return true;
}

/**
 * @brief Waits for a device node to appear and become ready within a
 *        specified timeout.
 *
 * Performs an informed poll by checking for the existence of the device node
 * and verifying its readiness via isDeviceReady(). Provides an
 * init-agnostic alternative to `udevadm settle` or `udevadm wait`.
 *
 * @param path            The filesystem path to the device node to wait for.
 * @param timeout_seconds The maximum duration to wait in seconds (default 30).
 * @return true if the device node is found and is ready for use within the
 *         timeout.
 * @return false if the timeout is reached before the device is ready.
 */
bool waitForDevice(const std::string& path, int timeout_seconds = 30) {
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(timeout_seconds)) {
        if (fs::exists(path) && fs::is_block_file(path)) {
            if (isDeviceReady(path)) return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

// ---------------------------------------------------------------------------
// Translation-unit private implementation
// ---------------------------------------------------------------------------

namespace {

    // ===========================================================================
    // retry:: jittered, cancellable backoff
    // ===========================================================================
    namespace retry {

        /**
         * @brief Sleeps for a randomized backoff interval, waking early on cancel.
         *
         * The delay is @p baseMs plus a uniformly random jitter of 0-100% of
         * @p baseMs, so the result lies in [baseMs, 2 * baseMs]. Jitter keeps
         * concurrent jobs that failed on the same shared resource (free loop device,
         * busy udev) from retrying in lockstep and colliding again.
         *
         * The wait is split into 50 ms slices that poll the cancellation flag, so a
         * cancel during the backoff is noticed within ~50 ms. On cancel the function
         * returns early; callers detect the cancel at the top of their next loop
         * iteration.
         *
         * Each thread has its own random generator, seeded from std::random_device,
         * so concurrent jobs draw independent delays.
         *
         * @param baseMs Minimum delay in milliseconds (the jitter is added on top).
         */
        void sleepWithJitter(int baseMs)
        {
            static thread_local std::mt19937 rng{std::random_device{}()};
            const int total = baseMs + std::uniform_int_distribution<int>(0, baseMs)(rng);

            const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(total);
            while (std::chrono::steady_clock::now() < end) {
                if (GlobalState::g_operationCancelled.load()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        /**
         * @brief Calls @p fn until it returns 0, with jittered linear backoff.
         *
         * Cancellation is checked before every attempt (and sleepWithJitter wakes
         * early on cancel). The delay after attempt i (1-based) is ~baseDelayMs * i,
         * jittered.
         *
         * @param fn          Callable returning int; 0 means success.
         * @param attempts    Maximum number of attempts (default 3).
         * @param baseDelayMs Base backoff in milliseconds (default 500).
         * @return 0 on success, -1 if cancelled (or attempts <= 0), otherwise the
         *         return value of the last attempt.
         */
        template <class Fn>
        int withBackoff(Fn&& fn, int attempts = 3, int baseDelayMs = 500)
        {
            int rc = -1;
            for (int i = 0; i < attempts; ++i) {
                if (GlobalState::g_operationCancelled.load()) return -1;

                rc = fn();
                if (rc == 0) return 0;

                if (i + 1 < attempts)
                    sleepWithJitter(baseDelayMs * (i + 1));
            }
            return rc;
        }

    }  // namespace retry

    // ===========================================================================
    // proc:: spawning external commands
    // ===========================================================================
    namespace proc {

        /**
         * @brief Runs an external command with stdout/stderr suppressed.
         *
         * Uses posix_spawn instead of fork()+exec() to avoid duplicating the
         * calling process's threads, held locks, or memory state into the child.
         * fork() in a multithreaded process only carries the calling thread
         * forward — if another thread held e.g. glibc's malloc arena lock or a
         * libmount/libblkid internal lock at the moment of fork(), the child can
         * deadlock on its very first allocation before exec() ever replaces it.
         * posix_spawn sidesteps this entirely since no such fork-without-exec
         * window exists.
         *
         * The executable is located with posix_spawnp, which searches the caller's
         * PATH. The child itself runs with a fixed PATH (see below).
         *
         * @param args Argument vector where args[0] is the executable name.
         * @return Exit code of the child process, or -1 on spawn/wait failure.
         */
        int runCommand(const std::vector<std::string>& args) {
            if (args.empty()) return -1;

            std::vector<char*> argv;
            argv.reserve(args.size() + 1);
            for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);

            posix_spawn_file_actions_t fileActions;
            if (posix_spawn_file_actions_init(&fileActions) != 0) return -1;

            int devNull = open("/dev/null", O_WRONLY);
            if (devNull < 0) {
                posix_spawn_file_actions_destroy(&fileActions);
                return -1;
            }

            posix_spawn_file_actions_adddup2(&fileActions, devNull, STDOUT_FILENO);
            posix_spawn_file_actions_adddup2(&fileActions, devNull, STDERR_FILENO);
            posix_spawn_file_actions_addclose(&fileActions, devNull);

            // The child runs with a fixed PATH so that helper programs launched by
            // parted/mkfs.* resolve predictably. This is the child's environment
            // only; the executable itself is looked up by posix_spawnp using the
            // caller's PATH. Covers the standard sbin/bin layouts of Debian/Ubuntu,
            // Fedora/RHEL and Arch (merged-/usr).
            static char trustedPath[] =
                "PATH=/usr/sbin:/usr/bin:/sbin:/bin";
            char* const spawnEnv[] = { trustedPath, nullptr };

            pid_t pid;
            int rc = posix_spawnp(&pid, argv[0], &fileActions, nullptr,
                                    argv.data(), spawnEnv);

            posix_spawn_file_actions_destroy(&fileActions);
            close(devNull);

            if (rc != 0) return -1;

            int status = 0;
            if (waitpid(pid, &status, 0) < 0) return -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        /**
         * @brief Runs an external command, retrying on failure.
         *
         * Retry policy is implemented by retry::withBackoff().
         * Retries absorb transient failures caused by other system activity (udev
         * re-probing the disk, boot-time services holding the device), which
         * typically clear within a second or two.
         *
         * Between attempts the delay grows linearly with the attempt number and is
         * jittered (see retry::sleepWithJitter): ~500-1000 ms after the first
         * failure, ~1000-2000 ms after the second. Cancellation is checked before
         * every attempt and during every backoff. The time each failed attempt
         * itself takes is not included in these figures.
         *
         * @param args        Argument vector where args[0] is the executable name.
         * @param attempts    Maximum number of attempts (default 3).
         * @param baseDelayMs Base backoff in milliseconds, multiplied by the attempt
         *                    number (default 500).
         * @return 0 on success, -1 if cancelled, otherwise the exit code of the last
         *         attempt.
         */
        int runCommandRetry(const std::vector<std::string>& args,
                            int attempts    = 3,
                            int baseDelayMs = 500)
        {
            return retry::withBackoff([&] { return runCommand(args); },
                                      attempts, baseDelayMs);
        }

    }  // namespace proc

    // ===========================================================================
    // mnt:: libmount wrappers, ScopedMount RAII, mount retries
    // ===========================================================================
    namespace mnt {

        /**
         * @brief Mount @p src at @p target using libmnt_context.
         *
         * @param src     Source path or device (may be nullptr for bind/move mounts).
         * @param target  Mount point (must already exist).
         * @param options Comma-separated mount options string (e.g. "ro,loop").
         *                Pass nullptr or "" for no extra options.
         * @param fstype  Filesystem type override (e.g. "ntfs3").
         *                Pass nullptr to let the kernel auto-detect.
         * @return 0 on success, non-zero on failure.
         */
        int libMount(const char* src,
                     const char* target,
                     const char* options = nullptr,
                     const char* fstype  = nullptr)
        {
            libmnt_context* ctx = mnt_new_context();
            if (!ctx) return -1;

            if (src)     mnt_context_set_source(ctx, src);
            if (target)  mnt_context_set_target(ctx, target);
            if (options && *options)
                         mnt_context_append_options(ctx, options);
            if (fstype && *fstype)
                         mnt_context_set_fstype(ctx, fstype);

            int rc = mnt_context_mount(ctx);
            mnt_free_context(ctx);
            return rc;
        }

        /**
         * @brief Performs a controlled unmount using a persistent libmount context.
         *
         * Handles the full lifecycle of an unmount. By reusing the context, it
         * ensures flags like MNT_DETACH and LOOPDEL are applied consistently, even
         * if an initial standard unmount fails.
         *
         * @param path The mount point or device path to unmount.
         */
        void safeUmount(const char* path) {
            libmnt_context* ctx = mnt_new_context();
            if (!ctx) return;

            mnt_context_set_target(ctx, path);
            mnt_context_enable_loopdel(ctx, true);

            // Cancellation: bypass a standard unmount and go straight to lazy
            // detachment for immediate release.
            if (GlobalState::g_operationCancelled.load()) {
                mnt_context_enable_lazy(ctx, true);
            }

            // If the plain unmount reports the device is busy, force a lazy
            // detachment and retry on the same context.
            if (mnt_context_umount(ctx) != 0) {
                mnt_context_enable_lazy(ctx, true);
                mnt_context_umount(ctx);
            }

            mnt_free_context(ctx);
        }

        /**
         * @struct ScopedMount
         * @brief RAII wrapper to ensure automatic unmounting and directory removal.
         *
         * Manages the lifecycle of a mount point created via mkdtemp. On destruction:
         * - If active (setMounted() was called), safeUmount is called first.
         * - rmdir is always called if path is non-empty, cleaning up the temp dir
         *   regardless of whether a mount succeeded.
         *
         * Non-copyable and non-movable — owns a unique kernel resource.
         */
        struct ScopedMount {
            std::string path;
            bool active = false;
            // Accepts an empty path to represent a "disabled" scope (no-op destructor).
            explicit ScopedMount(std::string p) : path(std::move(p)) {}
            ScopedMount(const ScopedMount&)            = delete;
            ScopedMount& operator=(const ScopedMount&) = delete;
            ScopedMount(ScopedMount&&)                 = delete;
            ScopedMount& operator=(ScopedMount&&)      = delete;
            // Call after a successful mount to arm the safeUmount on destruction.
            void setMounted() { active = true; }
            ~ScopedMount() {
                if (path.empty()) return;
                if (active) safeUmount(path.c_str());
                rmdir(path.c_str());
            }
        };

        /**
         * @brief libMount() with the same retry and jittered backoff as
         *        proc::runCommandRetry().
         *
         * Retry policy is implemented by retry::withBackoff().
         * Mainly guards the ISO loop mount against transient failures such as
         * loop-device contention with other boot-time services or concurrent jobs.
         * Parameters match libMount(), plus the retry settings.
         *
         * @param attempts    Maximum number of attempts (default 3).
         * @param baseDelayMs Base backoff in milliseconds, multiplied by the attempt
         *                    number (default 500).
         * @return 0 on success, -1 if cancelled, otherwise the return code of the
         *         last mount attempt.
         */
        int libMountRetry(const char* src, const char* target,
                          const char* options = nullptr,
                          const char* fstype  = nullptr,
                          int attempts    = 3,
                          int baseDelayMs = 500)
        {
            return retry::withBackoff(
                [&] { return libMount(src, target, options, fstype); },
                attempts, baseDelayMs);
        }

        /**
         * @brief Mounts an NTFS partition through a specific kernel driver, with
         *        retry and jittered backoff.
         *
         * Retry policy is implemented by retry::withBackoff().
         * Builds a fresh libmnt_context on every attempt, so no state from a failed
         * attempt carries over. Mount helpers are disabled so the VFS invokes the
         * named kernel driver directly (see disk::getBestNtfsDriver()).
         *
         * @param src         NTFS partition node (e.g. "/dev/sdb2").
         * @param target      Existing mount point.
         * @param fstype      Kernel driver token ("ntfs" or "ntfs3").
         * @param attempts    Maximum number of attempts (default 3).
         * @param baseDelayMs Base backoff in milliseconds (default 500).
         * @return 0 on success, -1 if cancelled, otherwise the return code of the
         *         last attempt.
         */
        int libMountNtfsRetry(const char* src, const char* target,
                              const char* fstype,
                              int attempts    = 3,
                              int baseDelayMs = 500)
        {
            return retry::withBackoff([&] {
                libmnt_context* ctx = mnt_new_context();
                if (!ctx) return -1;

                mnt_context_set_source(ctx, src);
                mnt_context_set_target(ctx, target);
                mnt_context_set_fstype(ctx, fstype);
                mnt_context_append_options(ctx, "noatime");
                mnt_context_disable_helpers(ctx, true);

                const int rc = mnt_context_mount(ctx);
                mnt_free_context(ctx);
                return rc;
            }, attempts, baseDelayMs);
        }

    }  // namespace mnt

    // ===========================================================================
    // iso:: ISO classification and tree enumeration
    // ===========================================================================
    namespace iso {

        /** Layout variants that drive partitioning and copy strategy. */
        enum class IsoType { WindowsInstall, FatOnly };

        /** @brief A single file or directory discovered during the ISO tree walk. */
        struct IsoCopyEntry {
            fs::path src;      ///< Path inside the read-only ISO mount.
            fs::path dst;      ///< Destination path on the FAT32 or NTFS mount.
            uint64_t size;     ///< Source size in bytes (0 for directories).
            bool     isDir;    ///< True if the entry is a directory.
        };

        /**
         * @brief Classify a mounted ISO as a full Windows install or FAT-only boot
         *        disk.
         *
         * Presence of sources/install.wim or sources/install.esd is the canonical
         * indicator of a Windows install image. WinPE, Hiren's Boot CD, and other
         * rescue disks lack these files and are treated as FatOnly.
         *
         * @param isoMnt Mount point of the loop-mounted ISO (read-only).
         * @return WindowsInstall if an install payload is found, FatOnly otherwise.
         */
        IsoType detectIsoType(const std::string& isoMnt) {
            return (fs::exists(isoMnt + "/sources/install.wim") ||
                    fs::exists(isoMnt + "/sources/install.esd"))
                       ? IsoType::WindowsInstall
                       : IsoType::FatOnly;
        }

        /**
         * @brief Decide whether an ISO entry belongs on the ESP (FAT32) partition.
         *
         * For FatOnly, every entry goes to FAT32. For WindowsInstall, only top-level
         * efi/ and boot/ folders (case-insensitively) and sources/boot.wim do.
         *
         * @param rel     Path relative to the ISO mount root.
         * @param isoType Detected ISO type.
         * @return true if the entry must be copied to the ESP.
         */
        bool isEspEntry(const fs::path& rel, IsoType isoType) {
            if (isoType == IsoType::FatOnly) return true;

            std::string top = rel.begin()->string();
            std::transform(top.begin(), top.end(), top.begin(),
                           [](unsigned char c) { return std::tolower(c); });

            static const std::unordered_set<std::string> topFolders = { "efi", "boot" };
            if (topFolders.count(top) > 0) return true;

            std::string relStr = rel.generic_string();
            std::transform(relStr.begin(), relStr.end(), relStr.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            return relStr == "sources/boot.wim";
        }

        /**
         * @brief Walk the ISO tree and classify each entry into the NTFS and ESP
         *        destination lists, accumulating the total byte count.
         *
         * Symlinks are silently skipped (see "Symlinks" in the public contract).
         * Directories contribute 0 to totalBytes. Throws nothing; returns false on
         * any filesystem error (the caller distinguishes cancellation).
         *
         * @param isoMnt       ISO mount point.
         * @param isoType      Detected ISO type.
         * @param fatMnt       FAT32 mount point (ESP destination).
         * @param ntfsMnt      NTFS mount point (WindowsInstall only).
         * @param[out] ntfsEntries  NTFS-bound entries.
         * @param[out] espEntries   ESP-bound entries.
         * @param[out] totalBytes   Sum of source file sizes.
         * @return true on success; false on enumeration error.
         */
        bool enumerateIsoEntries(const std::string& isoMnt,
                                 IsoType isoType,
                                 const std::string& fatMnt,
                                 const std::string& ntfsMnt,
                                 std::vector<IsoCopyEntry>& ntfsEntries,
                                 std::vector<IsoCopyEntry>& espEntries,
                                 uint64_t& totalBytes) {
            try {
                for (const auto& entry : fs::recursive_directory_iterator(isoMnt)) {
                    // Reject symlinks outright — a malicious ISO (Rock Ridge/UDF)
                    // could embed a symlink pointing at an arbitrary host path.
                    if (fs::is_symlink(entry.symlink_status())) continue;

                    const fs::path rel   = fs::relative(entry.path(), isoMnt);
                    const bool     toESP = isEspEntry(rel, isoType);
                    const fs::path dest  = fs::path(toESP ? fatMnt : ntfsMnt) / rel;
                    const uint64_t sz    = entry.is_regular_file() ? entry.file_size() : 0;

                    totalBytes += sz;

                    IsoCopyEntry e{ entry.path(), dest, sz, entry.is_directory() };
                    if (toESP) espEntries.push_back(std::move(e));
                    else       ntfsEntries.push_back(std::move(e));
                }
            } catch (...) {
                return false;
            }
            return true;
        }

    }  // namespace iso

    // IsoType is used throughout the remaining namespaces and the public entry
    // point; bring it in once instead of qualifying every mention.
    using iso::IsoType;

    // ===========================================================================
    // disk:: wipe, partition, format and mount the target device
    // ===========================================================================
    namespace disk {

        /**
         * @brief Probes /sys/module to determine the best available write-capable
         * NTFS kernel driver currently active on the host platform.
         *
         * Evaluates system capabilities via sysfs, ensuring compatibility with both
         * dynamically loaded kernel modules (.ko) and drivers built directly into the
         * kernel core. Uses a lazy-loading strategy: check for pre-initialized
         * drivers first, and only run userspace modprobe for lower priority/fallback
         * tiers if necessary.
         *
         * Following the modern NTFS driver overhaul in Linux 7.1+, this function
         * prioritizes the heavily optimized "ntfs" driver rewrite, which entirely
         * supersedes the legacy read-only engine. It falls back to Paragon's "ntfs3"
         * driver if running on older kernels (like 5.15 through 6.x) where the
         * modern remake is unavailable.
         *
         * @note Because modern Linux distributions frequently symlink userspace
         * binaries or default to older FUSE wrappers, the returned token is mounted
         * with libmount helpers disabled (see mnt::libMountNtfsRetry()). This
         * bypasses userspace mount helpers and forces the VFS subsystem to invoke
         * the designated kernel module directly.
         *
         * Driver priority tiering:
         * 1. "ntfs"  via Kernel 7.1+ (modernized native read/write engine) — highest
         *    priority.
         * 2. "ntfs3" via Kernel 5.15+ (Paragon engine) — fallback.
         *
         * @return Token name of the detected NTFS kernel driver ("ntfs" or "ntfs3"),
         *         or an empty string if no modern write-capable driver is available.
         */
        std::string getBestNtfsDriver() {
            if (std::filesystem::exists("/sys/module/ntfs")) {
                return "ntfs";
            }

            proc::runCommand({"modprobe", "ntfs"});
            if (std::filesystem::exists("/sys/module/ntfs")) {
                return "ntfs";
            }

            proc::runCommand({"modprobe", "ntfs3"});
            if (std::filesystem::exists("/sys/module/ntfs3")) {
                return "ntfs3";
            }

            return {};
        }

        /**
         * @brief Erase all filesystem/partition signatures on @p device.
         *
         * Equivalent to `wipefs -a <device>`. Uses libblkid's probe-and-wipe loop
         * so no child process is spawned.
         *
         * @param device Block device path (e.g. "/dev/sdb").
         * @return true on success (including the case where no signatures were
         *         found).
         */
        bool wipeDeviceSignatures(const std::string& device)
        {
            blkid_probe pr = blkid_new_probe_from_filename(device.c_str());
            if (!pr) return false;

            blkid_probe_enable_superblocks(pr, true);
            blkid_probe_set_superblocks_flags(pr,
                BLKID_SUBLKS_MAGIC | BLKID_SUBLKS_TYPE);

            blkid_probe_enable_partitions(pr, true);
            blkid_probe_set_partitions_flags(pr, BLKID_PARTS_MAGIC);

            // Loop until no more signatures are found.
            // blkid_do_probe() returns 0 while signatures remain, 1 when done.
            while (blkid_do_probe(pr) == 0)
                blkid_do_wipe(pr, false /* not dry-run */);

            blkid_free_probe(pr);
            return true;
        }

        /**
         * @brief Wipe device signatures, partition via parted (ESP/DATA or WINPE),
         *        and derive partition node paths with waitForDevice.
         *
         * @param[out] fatPart  Derived FAT32 partition node (empty on failure).
         * @param[out] ntfsPart Derived NTFS partition node, or empty for FatOnly.
         * @return true on success; false on any wipe/partition/wait failure.
         */
        bool setupPartitionedDevice(const std::string& device, IsoType isoType,
                                    std::string& fatPart, std::string& ntfsPart) {
            if (!wipeDeviceSignatures(device)) return false;

            int partResult = -1;
            if (isoType == IsoType::WindowsInstall) {
                partResult = proc::runCommandRetry({"parted", "-s", device,
                                                    "mklabel", "gpt",
                                                    "mkpart", "ESP",  "fat32", "1MiB",    "1025MiB",
                                                    "mkpart", "DATA", "ntfs",  "1025MiB", "100%",
                                                    "set", "1", "esp",  "on",
                                                    "set", "1", "boot", "on"});
            } else {
                partResult = proc::runCommandRetry({"parted", "-s", device,
                                                    "mklabel", "gpt",
                                                    "mkpart", "WINPE", "fat32", "1MiB", "100%",
                                                    "set", "1", "esp",  "on",
                                                    "set", "1", "boot", "on"});
            }
            if (partResult != 0) return false;
            if (!waitForDevice(device)) return false;

            auto derive = [&](int n) -> std::string {
                std::string t = device + std::to_string(n);
                return waitForDevice(t, 30) ? t : std::string{};
            };
            fatPart  = derive(1);
            ntfsPart = (isoType == IsoType::WindowsInstall) ? derive(2) : std::string{};

            if (fatPart.empty()) return false;
            if (isoType == IsoType::WindowsInstall && ntfsPart.empty()) return false;
            return true;
        }

        /**
         * @brief mkfs.fat on @p fatPart (always) and mkfs.ntfs on @p ntfsPart
         *        (WindowsInstall only), using the documented labels/options.
         *
         * @return true on success; false on any mkfs failure.
         */
        bool formatTargetPartitions(const std::string& fatPart,
                                    const std::string& ntfsPart,
                                    IsoType isoType) {
            if (proc::runCommandRetry({"mkfs.fat", "-F", "32", "-n",
                                       (isoType == IsoType::WindowsInstall ? "WINBOOT" : "WINPE"),
                                       fatPart}) != 0) {
                return false;
            }
            if (isoType == IsoType::WindowsInstall) {
                if (proc::runCommandRetry({"mkfs.ntfs", "-f", "-c", "4096", "-L", "WINDATA",
                                           ntfsPart}) != 0) {
                    return false;
                }
            }
            return true;
        }

        /**
         * @brief Mount @p fatPart with noatime and (WindowsInstall only) @p ntfsPart
         *        through getBestNtfsDriver, arming each ScopedMount in place.
         *
         * @return true on success; false on any mount / driver-lookup failure.
         */
        bool mountTargetFilesystems(IsoType isoType,
                                    const std::string& fatPart,
                                    const std::string& ntfsPart,
                                    const std::string& fatMnt,
                                    const std::string& ntfsMnt,
                                    mnt::ScopedMount& fatScope,
                                    mnt::ScopedMount& ntfsScope) {
            if (mnt::libMountRetry(fatPart.c_str(), fatMnt.c_str(), "noatime") != 0)
                return false;
            fatScope.setMounted();

            if (isoType == IsoType::WindowsInstall) {
                const std::string ntfsDriver = getBestNtfsDriver();
                if (ntfsDriver.empty()) return false;
                if (mnt::libMountNtfsRetry(ntfsPart.c_str(), ntfsMnt.c_str(),
                                           ntfsDriver.c_str()) != 0)
                    return false;
                ntfsScope.setMounted();
            }
            return true;
        }

        /**
         * @brief syncfs() the filesystem mounted at @p mountPath. Open or syncfs
         *        failure is silently ignored (matching the documented flush policy).
         */
        void syncMount(const std::string& mountPath) {
            const int fd = open(mountPath.c_str(), O_RDONLY);
            if (fd >= 0) { syncfs(fd); close(fd); }
        }

    }  // namespace disk

    // ===========================================================================
    // copy:: file copy engine (buffered I/O with paced writeback, one buffer)
    // ===========================================================================
    namespace copy {

        /** @brief Size of the shared I/O buffer. */
        constexpr size_t kBufferSize  = 4 * 1024 * 1024;

        /** @brief Alignment of the shared I/O buffer (one page). */
        constexpr size_t kBufferAlign = 4096;

        /** @brief Writeback is paced once per this many chunks. */
        constexpr int kPaceEveryChunks = 1;

        /**
         * @brief Per-file writeback pacing state: the chunks written since the last
         *        pacing point, and the range paced last time.
         */
        struct WritebackState {
            uint64_t pendOff    = 0;   ///< Start of the not-yet-paced range.
            uint64_t pendLen    = 0;   ///< Length of the not-yet-paced range.
            int      pendChunks = 0;   ///< Chunks accumulated in that range.
            uint64_t prevOff    = 0;   ///< Start of the range paced last time.
            uint64_t prevLen    = 0;   ///< Length of the range paced last time.
            bool     havePrev   = false;
        };

        /**
         * @brief Bound the dirty-page backlog of an output file to a few chunks,
         *        pacing once every kPaceEveryChunks chunks.
         *
         * Chunks are accumulated into one contiguous range. When it reaches
         * kPaceEveryChunks chunks, writeback of the whole range is started
         * (SYNC_FILE_RANGE_WRITE, does not block on the device), then the range
         * paced last time is waited on (WAIT_BEFORE | WRITE | WAIT_AFTER). The
         * device therefore always has the newest range queued while the previous
         * one drains, and a cancel never has more than about
         * kPaceEveryChunks + 1 chunks left to flush.
         *
         * Best effort: failures (e.g. a filesystem without support) are ignored, as
         * with the final syncfs() flush; the copy then degrades to plain buffered
         * I/O.
         *
         * @param fd_out Output file descriptor.
         * @param off    File offset of the chunk just written.
         * @param len    Length in bytes of the chunk just written (must be > 0).
         * @param st     Pacing state for this output file.
         */
        void paceWriteback(int fd_out, uint64_t off, uint64_t len,
                           WritebackState& st) {
            if (st.pendChunks == 0) {
                st.pendOff = off;
                st.pendLen = 0;
            }
            st.pendLen += len;   // chunks are contiguous within a file
            if (++st.pendChunks < kPaceEveryChunks) return;

            sync_file_range(fd_out, static_cast<off_t>(st.pendOff),
                            static_cast<off_t>(st.pendLen),
                            SYNC_FILE_RANGE_WRITE);

            if (st.havePrev) {
                sync_file_range(fd_out, static_cast<off_t>(st.prevOff),
                                static_cast<off_t>(st.prevLen),
                                SYNC_FILE_RANGE_WAIT_BEFORE |
                                SYNC_FILE_RANGE_WRITE |
                                SYNC_FILE_RANGE_WAIT_AFTER);
            }

            st.prevOff    = st.pendOff;
            st.prevLen    = st.pendLen;
            st.havePrev   = true;
            st.pendChunks = 0;
        }

        /**
         * @brief Fill @p buf from @p fd_in up to @p bufferSize bytes, accumulating
         *        short reads and retrying EINTR; only read()==0 ends a chunk early.
         *
         * @return Bytes filled (0 = EOF), or -1 on error / cancellation.
         */
        ssize_t fillChunk(int fd_in, char* buf, size_t bufferSize) {
            size_t total = 0;
            while (total < bufferSize) {
                if (GlobalState::g_operationCancelled.load()) return -1;
                const ssize_t n = read(fd_in, buf + total, bufferSize - total);
                if (n < 0) {
                    if (errno == EINTR) continue;
                    return -1;
                }
                if (n == 0) break;
                total += static_cast<size_t>(n);
            }
            return static_cast<ssize_t>(total);
        }

        /**
         * @brief Write one chunk at @p fileOffset and advance it by the chunk length.
         *
         * Retries EINTR, finishes short writes, and treats a 0-byte write as EIO.
         * Every byte written is credited to the progress counters as it lands. After
         * the chunk is complete, writeback is paced (see paceWriteback()) so the
         * dirty backlog stays at a few chunks and a cancel drains quickly.
         *
         * @param wb Writeback pacing state for this output file.
         * @return true if the chunk was written completely; false otherwise.
         */
        bool writeOneChunk(int fd_out, const char* buf, size_t len,
                           uint64_t& fileOffset,
                           uint64_t& bytesCopiedTotal,
                           std::atomic<uint64_t>& totalAccumulator,
                           WritebackState& wb) {
            size_t done = 0;
            while (done < len) {
                if (GlobalState::g_operationCancelled.load()) return false;

                const ssize_t written = pwrite(fd_out, buf + done, len - done,
                                               static_cast<off_t>(fileOffset + done));
                if (written < 0) {
                    if (errno == EINTR) continue;
                    return false;
                }
                if (written == 0) { errno = EIO; return false; }

                done += static_cast<size_t>(written);
                totalAccumulator.fetch_add(static_cast<uint64_t>(written));
                bytesCopiedTotal += static_cast<uint64_t>(written);
            }

            paceWriteback(fd_out, fileOffset, len, wb);
            fileOffset += len;
            return true;
        }

        /**
         * @brief Copies file data in chunks while tracking progress and checking for
         *        cancellation and source truncation.
         *
         * @return true only if the complete expected file size was copied without
         *         failure or cancellation.
         */
        bool copyFileChunks(int fd_in, int fd_out,
                            uint64_t fileSize,
                            size_t bufferSize,
                            char* buf,
                            std::atomic<uint64_t>& totalAccumulator,
                            uint64_t& bytesCopiedTotal) {

            uint64_t fileOffset = 0;
            WritebackState wb;
            bool success = true;

            while (!GlobalState::g_operationCancelled.load()) {
                const ssize_t n = fillChunk(fd_in, buf, bufferSize);

                if (n < 0) {
                    success = false;
                    break;
                }

                if (n == 0) break;

                if (!writeOneChunk(fd_out, buf, static_cast<size_t>(n), fileOffset,
                                   bytesCopiedTotal, totalAccumulator, wb)) {
                    success = false;
                    break;
                }
            }

            if (GlobalState::g_operationCancelled.load())
                success = false;

            // Fewer bytes than the ISO walk reported means the source was truncated.
            if (success && bytesCopiedTotal < fileSize)
                success = false;

            return success;
        }

        /**
         * @brief Per-file copy entry point: opens src/dst, optionally pre-allocates
         *        the destination, copies it with paced buffered I/O, and cleans up
         *        after a failure or cancellation.
         *
         * Pre-allocation (posix_fallocate) is done for NTFS only: FAT's fallocate
         * allocates clusters one by one and is slow. On failure or cancellation,
         * partially written FAT32 files are truncated to 0 bytes; NTFS files are
         * left as-is.
         *
         * @param preallocate true for NTFS destinations (pre-allocate non-empty
         *                    files); false for FAT32.
         * @return true only if the whole file was copied and no cancellation is in
         *         effect; false otherwise.
         */
        bool copyFileWithProgress(const fs::path& src, const fs::path& dst,
                                  uint64_t fileSize, bool preallocate,
                                  size_t bufferSize, char* buf,
                                  std::atomic<uint64_t>& totalAccumulator) {

            const int fd_in = open(src.c_str(), O_RDONLY);
            if (fd_in < 0) return false;

            posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

            const int fd_out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd_out < 0) {
                close(fd_in);
                return false;
            }

            if (preallocate && fileSize > 0)
                posix_fallocate(fd_out, 0, static_cast<off_t>(fileSize));

            uint64_t bytesCopiedTotal = 0;

            const bool success = copyFileChunks(
                fd_in, fd_out, fileSize, bufferSize, buf,
                totalAccumulator, bytesCopiedTotal);

            if (!success && !preallocate) {
                // Don't leave partial FAT32 files behind.
                [[maybe_unused]] const int r = ftruncate(fd_out, 0);
            }

            if (GlobalState::g_operationCancelled.load())
                posix_fadvise(fd_out, 0, 0, POSIX_FADV_DONTNEED);

            posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);

            close(fd_in);
            close(fd_out);

            return success && !GlobalState::g_operationCancelled.load();
        }

        /**
         * @brief Iterate one entry list, creating directories and copying files.
         *        NTFS-bound files are pre-allocated; FAT32-bound files are not.
         *
         * @return false on the first copy failure or cancellation; true otherwise.
         */
        bool processEntries(const std::vector<iso::IsoCopyEntry>& entries,
                            IsoType isoType, const std::string& ntfsMnt,
                            size_t bufferSize, char* buf,
                            std::atomic<uint64_t>& totalAccumulator) {
            for (const auto& e : entries) {
                if (GlobalState::g_operationCancelled.load()) return false;

                if (e.isDir) {
                    fs::create_directories(e.dst);
                } else {
                    fs::create_directories(e.dst.parent_path());

                    const bool toNtfs = (isoType == IsoType::WindowsInstall) &&
                                        (e.dst.string().rfind(ntfsMnt, 0) == 0);

                    if (!copyFileWithProgress(e.src, e.dst, e.size, toNtfs,
                                              bufferSize, buf, totalAccumulator)) {
                        return false;
                    }
                }
            }
            return true;
        }

    }  // namespace copy

    // ===========================================================================
    // telemetry:: progress / speed monitor thread
    // ===========================================================================
    namespace telemetry {

        /**
         * @brief Body of the telemetry thread. Polls every 500 ms, publishes
         *        bytesWritten/progress/speed into @p info with the documented EMA
         *        and stall semantics, and exits when @p active is cleared.
         *
         * @param accumulator Shared running total of source bytes written.
         * @param active      Set false to stop; the caller joins afterwards.
         * @param totalBytes  Denominator for the progress percentage.
         * @param info        ProgressInfo slot to publish into.
         */
        void runProgressMonitor(std::atomic<uint64_t>& accumulator,
                                std::atomic<bool>& active,
                                uint64_t totalBytes,
                                ProgressInfo& info) {
            auto     lastUpdate    = std::chrono::high_resolution_clock::now();
            uint64_t lastWritten   = 0;
            double   smoothedSpeed = 0.0;
            bool     haveEstimate  = false;

            auto     stallStart    = std::chrono::high_resolution_clock::now();
            bool     stalled       = false;

            // Higher alpha = more reactive to phase changes (NTFS -> ESP),
            // lower = smoother.
            constexpr double alpha = 0.3;
            constexpr int    MIN_UPDATE_INTERVAL_MS = 1000;
            // Show 0 only after this many seconds of continuous stalling
            constexpr int    STALL_TIMEOUT_SECONDS  = 300;

            while (active.load()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));

                const auto now = std::chrono::high_resolution_clock::now();
                const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                        now - lastUpdate).count();

                const uint64_t currentWritten = accumulator.load();
                info.bytesWritten.store(currentWritten);
                info.progress.store(static_cast<int>(
                    std::min(99.0, (static_cast<double>(currentWritten) / totalBytes) * 100.0)));

                const uint64_t deltaBytes = (currentWritten >= lastWritten)
                                                ? (currentWritten - lastWritten) : 0;

                if (deltaBytes > 0 && ms >= MIN_UPDATE_INTERVAL_MS) {
                    stalled = false;

                    const double instantSpeed = (static_cast<double>(deltaBytes) /
                                                    (1024.0 * 1024.0)) / (ms / 1000.0);

                    double smoothingFactor = (instantSpeed < 10.0) ? 0.2 : alpha;

                    smoothedSpeed = haveEstimate
                        ? (smoothingFactor * instantSpeed + (1.0 - smoothingFactor) * smoothedSpeed)
                        : instantSpeed;
                    haveEstimate = true;
                    info.speed.store(smoothedSpeed);

                    lastWritten = currentWritten;
                    lastUpdate = now;
                } else if (deltaBytes == 0 && ms >= MIN_UPDATE_INTERVAL_MS) {
                    // IMPORTANT: do NOT reset lastUpdate/lastWritten here. A long
                    // in-flight write can span many 500 ms ticks with deltaBytes == 0.
                    // Resetting would floor the reported speed. Leaving them untouched
                    // makes the eventual big delta reflect true elapsed time.
                    if (!stalled) { stalled = true; stallStart = now; }

                    auto stallDuration = std::chrono::duration_cast<std::chrono::seconds>(
                                            now - stallStart).count();
                    if (stallDuration >= STALL_TIMEOUT_SECONDS) {
                        info.speed.store(0.0);
                    }
                }
            }
        }

    }  // namespace telemetry

    // ===========================================================================
    // win_iso_steps:: orchestration steps used by writeWindowsIsoToDevice
    // ===========================================================================
    namespace win_iso_steps {

        using EntryVec = std::vector<iso::IsoCopyEntry>;

        bool isCancelled() { return GlobalState::g_operationCancelled.load(); }

        // Centralises the "genuine error vs. user cancellation" failure policy.
        struct FailureReporter {
            ProgressInfo& slot;

            bool fail() const {
                slot.failed.store(true);
                return false;
            }
            bool failUnlessCancelled() const {
                if (isCancelled()) return false;
                return fail();
            }
        };

        // Mounts the ISO read-only; arms the scope only on success.
        bool mountIso(const std::string& isoPath, const char* isoMntBuf,
                      mnt::ScopedMount& isoScope)
        {
            if (mnt::libMountRetry(isoPath.c_str(), isoMntBuf, "ro,loop") != 0) return false;
            isoScope.setMounted();
            return true;
        }

        // Wipe, partition and format. Returns false on any step failure or if
        // cancellation is observed afterwards.
        bool prepareDevice(const std::string& device, IsoType isoType,
                           std::string& fatPart, std::string& ntfsPart)
        {
            if (!disk::setupPartitionedDevice(device, isoType, fatPart, ntfsPart))
                return false;
            if (!disk::formatTargetPartitions(fatPart, ntfsPart, isoType))
                return false;
            return !isCancelled();
        }

        // The NTFS temp dir only exists for WindowsInstall images.
        bool makeNtfsDirIfNeeded(IsoType isoType, char* ntfsMntBuf)
        {
            return isoType != IsoType::WindowsInstall || mkdtemp(ntfsMntBuf) != nullptr;
        }

        std::string ntfsScopePath(IsoType isoType, const char* ntfsMntBuf)
        {
            return isoType == IsoType::WindowsInstall ? std::string(ntfsMntBuf)
                                                      : std::string{};
        }

        bool mountTargets(IsoType isoType, const std::string& fatPart,
                          const std::string& ntfsPart, const std::string& fatMnt,
                          const std::string& ntfsMnt, mnt::ScopedMount& fatScope,
                          mnt::ScopedMount& ntfsScope)
        {
            if (!disk::mountTargetFilesystems(isoType, fatPart, ntfsPart,
                                              fatMnt, ntfsMnt, fatScope, ntfsScope))
                return false;
            return !isCancelled();
        }

        // Fails if enumeration fails or the ISO contributes no bytes.
        bool enumerateNonEmpty(const std::string& isoMnt, IsoType isoType,
                               const std::string& fatMnt, const std::string& ntfsMnt,
                               EntryVec& ntfsEntries, EntryVec& espEntries,
                               uint64_t& totalBytes)
        {
            if (!iso::enumerateIsoEntries(isoMnt, isoType, fatMnt, ntfsMnt,
                                          ntfsEntries, espEntries, totalBytes))
                return false;
            return totalBytes != 0;
        }

        void finalFlush(IsoType isoType, const std::string& ntfsMnt,
                        const std::string& fatMnt)
        {
            if (isCancelled()) return;
            if (isoType == IsoType::WindowsInstall) disk::syncMount(ntfsMnt);
            disk::syncMount(fatMnt);
        }

        // NTFS payload first, then ESP, so the bootloader only lands after the data
        // it references is already on disk. runPass copies one entry list.
        template <class RunPass>
        bool runCopyPasses(RunPass&& runPass, IsoType isoType,
                           const std::string& ntfsMnt, const std::string& fatMnt,
                           EntryVec& ntfsEntries, EntryVec& espEntries)
        {
            if (!runPass(ntfsEntries)) return false;
            if (!runPass(espEntries)) return false;
            finalFlush(isoType, ntfsMnt, fatMnt);
            return true;
        }

        double averageSpeedMBps(uint64_t totalBytes, double seconds)
        {
            if (seconds <= 0.0) return 0.0;
            return (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / seconds;
        }

        void reportSuccess(ProgressInfo& slot, double avgSpeed)
        {
            slot.speed.store(avgSpeed);
            slot.progress.store(100);
            slot.completed.store(true);
        }

    }  // namespace win_iso_steps

}  // namespace

// ---------------------------------------------------------------------------
// Global scope: public entry points
// ---------------------------------------------------------------------------

/**
 * @brief Detects whether an ISO image is Windows installation media.
 *
 * Loop-mounts the ISO read-only into a temporary directory and checks for
 * the co-presence of sources/boot.wim (Windows setup payload) and
 * bootmgr / bootmgr.efi (Windows boot manager). Uses libmount instead of
 * forking mount/umount processes.
 *
 * @param isoPath Absolute path to the ISO image.
 * @return true if the ISO appears to be Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath)
{
    char tmpDir[] = "/tmp/iso_probe_XXXXXX";
    if (!mkdtemp(tmpDir)) return false;

    auto cleanup = [&]() {
        mnt::safeUmount(tmpDir);
        rmdir(tmpDir);
    };

    if (mnt::libMount(isoPath.c_str(), tmpDir, "ro,loop") != 0) {
        rmdir(tmpDir);
        return false;
    }

    bool hasBootWim = fs::exists(std::string(tmpDir) + "/sources/boot.wim");
    bool hasBootMgr = fs::exists(std::string(tmpDir) + "/bootmgr") ||
                      fs::exists(std::string(tmpDir) + "/bootmgr.efi");
    cleanup();
    return hasBootWim && hasBootMgr;
}

/**
 * @brief Wipe a block device, repartition it, and write a Windows-family ISO
 *        using buffered I/O with paced writeback.
 *
 * The ISO is loop-mounted read-only and copied file-by-file onto freshly
 * formatted filesystems. Symlinks in the ISO are silently skipped to prevent
 * path-traversal leaks.
 *
 * @par Partitioning & Copy Order
 * Chosen automatically via iso::detectIsoType():
 * - **WindowsInstall (GPT):**
 *   1. **ESP** (1–1025 MiB, FAT32, label @c WINBOOT): holds @c efi/, @c boot/,
 *      and @c sources/boot.wim (matched case-insensitively).
 *   2. **DATA** (1025 MiB–end, NTFS, label @c WINDATA): holds everything else.
 *   NTFS payload is copied before ESP so the bootloader lands after its data.
 * - **FatOnly (GPT):** Single FAT32 partition (1 MiB–end, label @c WINPE) for
 *   WinPE/rescue images; receives all files.
 *
 * Partition nodes are expected as @c device + N (e.g., @c /dev/sdb1; @c p
 * separators like @c nvme0n1p1 are unsupported) and awaited via waitForDevice()
 * (30 s timeout).
 *
 * @par I/O & Buffering
 * Every file, on both partitions, is copied through the page cache with a
 * manual @c read()/pwrite() loop and one reusable 4 MiB buffer.
 * - **Writeback pacing:** After each chunk (every kPaceEveryChunks chunks,
 *   currently 1), writeback of that chunk is started with
 *   @c sync_file_range() and the previously paced range is waited on. The dirty
 *   backlog therefore stays at about two chunks, progress and speed track the
 *   device rather than the page cache, and a cancel has almost nothing left to
 *   flush at unmount. Pacing is best effort; if unsupported it is ignored.
 * - **Pre-allocation:** Non-empty NTFS files are pre-allocated with
 *   @c posix_fallocate(). FAT32 files are not (its fallocate is slow).
 * - **Integrity:** Short reads accumulate until full or EOF; short writes are
 *   completed; @c EINTR is retried; zero-byte writes fail immediately. Sources
 *   yielding fewer bytes than reported by the ISO walk fail the operation. On
 *   failure or cancellation, FAT32 destinations are truncated to 0 bytes; NTFS
 *   files are left as-is.
 *
 * @par Lifecycle, Telemetry & Cleanup
 * - **Concurrency & RAII:** Fully re-entrant per job. Mounts
 *   (@c mnt::ScopedMount), buffer (@c AlignedBuffer), and threads
 *   (@c ThreadGuard) clean up automatically on all return paths.
 * - **Cancellation:** Aborts cleanly during setup or I/O loops without setting
 *   the slot's @c failed flag (except @c mkdtemp() failures, which always set
 *   @c failed). Unmounts on cancel are lazy.
 * - **Telemetry:** A background thread polls every 500 ms and updates EMA
 *   speed once per second (holding last speed up to 300 s during USB stalls).
 *   Progress caps at 99% during copy and hits 100% upon completion.
 * - **Flush & Unmount:** Calls @c syncfs() on NTFS then FAT32 before unmount.
 *   @c ScopedMount destructors block on @c safeUmount() as the function returns—
 *   after @c completed is marked true—so callers must wait for the function to
 *   return before advising device removal.
 *
 * @param isoPath       Path to the source ISO file.
 * @param device        Target block device (e.g., @c /dev/sdb). Will be wiped.
 * @param progressIndex Index into @p progressData for live status reporting.
 * @param progressData  Shared progress tracking array.
 * @return @c true on success; @c false on error, truncated source, or user
 *         cancellation (sets the slot's @c failed flag only on genuine errors).
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             size_t             progressIndex,
                             std::vector<ProgressInfo>& progressData)
{
    namespace steps = win_iso_steps;

    ProgressInfo& slot = progressData[progressIndex];
    const steps::FailureReporter report{slot};

    char isoMntBuf[] = "/tmp/win_iso_XXXXXX";
    if (!mkdtemp(isoMntBuf)) return report.fail();

    // ScopedMount always removes the dir; setMounted() arms the safeUmount.
    mnt::ScopedMount isoScope(isoMntBuf);

    if (!steps::mountIso(isoPath, isoMntBuf, isoScope))
        return report.failUnlessCancelled();

    const std::string isoMnt  = isoMntBuf;
    const IsoType     isoType = iso::detectIsoType(isoMnt);

    std::string fatPart, ntfsPart;
    if (!steps::prepareDevice(device, isoType, fatPart, ntfsPart))
        return report.failUnlessCancelled();

    char fatMntBuf[]  = "/tmp/win_fat_XXXXXX";
    char ntfsMntBuf[] = "/tmp/win_ntfs_XXXXXX";

    if (!mkdtemp(fatMntBuf)) return report.fail();
    mnt::ScopedMount fatScope(fatMntBuf);

    if (!steps::makeNtfsDirIfNeeded(isoType, ntfsMntBuf)) return report.fail();
    mnt::ScopedMount ntfsScope(steps::ntfsScopePath(isoType, ntfsMntBuf));

    const std::string fatMnt  = fatMntBuf;
    const std::string ntfsMnt = ntfsMntBuf;

    if (!steps::mountTargets(isoType, fatPart, ntfsPart,
                             fatMnt, ntfsMnt, fatScope, ntfsScope))
        return report.failUnlessCancelled();

    std::vector<iso::IsoCopyEntry> ntfsEntries, espEntries;
    uint64_t totalBytes = 0;

    if (!steps::enumerateNonEmpty(isoMnt, isoType, fatMnt, ntfsMnt,
                                  ntfsEntries, espEntries, totalBytes))
        return report.failUnlessCancelled();

    std::atomic<uint64_t> totalBytesWrittenAccumulator{0};
    std::atomic<bool>     monitoringActive{true};
    const auto startTime = std::chrono::high_resolution_clock::now();

    std::thread progressMonitorThread([&, totalBytes]() {
        telemetry::runProgressMonitor(totalBytesWrittenAccumulator, monitoringActive,
                                      totalBytes, slot);
    });

    // Ensures the thread is always stopped and joined on any return path.
    ThreadGuard threadGuard(progressMonitorThread, monitoringActive);

    constexpr size_t bufferSize = copy::kBufferSize;

    AlignedBuffer ioBuf(bufferSize, copy::kBufferAlign);
    if (!ioBuf)
        return report.failUnlessCancelled();

    auto runPass = [&](std::vector<iso::IsoCopyEntry>& entries) {
        return copy::processEntries(entries, isoType, ntfsMnt,
                                    bufferSize, ioBuf.data,
                                    totalBytesWrittenAccumulator);
    };

    if (!steps::runCopyPasses(runPass, isoType, ntfsMnt, fatMnt,
                              ntfsEntries, espEntries))
        return report.failUnlessCancelled();

    threadGuard.stop();

    // ScopedMount destructors for ntfsScope, fatScope, isoScope fire here
    // in reverse declaration order: ntfs -> fat -> iso.

    const auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - startTime);
    const double seconds = totalElapsed.count() / 1000.0;

    steps::reportSuccess(slot, steps::averageSpeedMBps(totalBytes, seconds));
    return true;
}
