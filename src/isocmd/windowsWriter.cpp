// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

    // Fixed, trusted search path for privileged helper binaries — deliberately
    // NOT the inherited `environ`, whose PATH could be attacker-influenced
    // (e.g. via sudo/pkexec with preserved env, or a less-trusted parent).
    // Covers the standard sbin/bin split across major distros (Debian/Ubuntu,
    // Fedora/RHEL, Arch's merged-/usr layout) without hardcoding a single
    // absolute binary path.
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
 * binaries or default to older FUSE wrappers, downstream mounting logic
 * should pass the internal flag ("mount", "-i", "-t", driver, ...) when using
 * the returned token. This bypasses userspace mount helpers and forces the
 * VFS subsystem to invoke the designated kernel module directly.
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

    runCommand({"modprobe", "ntfs"});
    if (std::filesystem::exists("/sys/module/ntfs")) {
        return "ntfs";
    }

    runCommand({"modprobe", "ntfs3"});
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
static bool wipeDeviceSignatures(const std::string& device)
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
 * @brief Mount @p src at @p target using libmnt_context.
 *
 * @param src     Source path or device (may be nullptr for bind/move mounts).
 * @param target  Mount point (must already exist).
 * @param options Comma-separated mount options string (e.g. "ro,loop").
 *                Pass nullptr or "" for no extra options.
 * @param fstype  Filesystem type override (e.g. "ntfs3").
 *                Pass nullptr to let the kernel auto-detect.
 * @param flags   Extra libmount context flags (e.g. MNT_MS_PROPAGATION).
 *                Pass 0 for the common case.
 * @return 0 on success, non-zero on failure.
 */
static int libMount(const char* src,
                    const char* target,
                    const char* options = nullptr,
                    const char* fstype  = nullptr,
                    int         flags   = 0)
{
    libmnt_context* ctx = mnt_new_context();
    if (!ctx) return -1;

    if (src)     mnt_context_set_source(ctx, src);
    if (target)  mnt_context_set_target(ctx, target);
    if (options && *options)
                 mnt_context_append_options(ctx, options);
    if (fstype && *fstype)
                 mnt_context_set_fstype(ctx, fstype);
    (void)flags; // reserved for caller convenience

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
        safeUmount(tmpDir);
        rmdir(tmpDir);
    };

    if (libMount(isoPath.c_str(), tmpDir, "ro,loop") != 0) {
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
static void sleepWithJitter(int baseMs)
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
 * @brief Runs an external command, retrying on failure.
 *
 * Retries absorb transient failures caused by other system activity (udev
 * re-probing the disk, boot-time services holding the device), which
 * typically clear within a second or two.
 *
 * Between attempts the delay grows linearly with the attempt number and is
 * jittered (see sleepWithJitter): ~500-1000 ms after the first failure,
 * ~1000-2000 ms after the second. Cancellation is checked before every
 * attempt and during every backoff. The time each failed attempt itself
 * takes is not included in these figures.
 *
 * @param args        Argument vector where args[0] is the executable name.
 * @param attempts    Maximum number of attempts (default 3).
 * @param baseDelayMs Base backoff in milliseconds, multiplied by the attempt
 *                    number (default 500).
 * @return 0 on success, -1 if cancelled, otherwise the exit code of the last
 *         attempt.
 */
static int runCommandRetry(const std::vector<std::string>& args,
                           int attempts    = 3,
                           int baseDelayMs = 500)
{
    int rc = -1;
    for (int i = 0; i < attempts; ++i) {
        if (GlobalState::g_operationCancelled.load()) return -1;

        rc = runCommand(args);
        if (rc == 0) return 0;

        if (i + 1 < attempts)
            sleepWithJitter(baseDelayMs * (i + 1));
    }
    return rc;
}

/**
 * @brief libMount() with the same retry and jittered backoff as
 *        runCommandRetry().
 *
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
static int libMountRetry(const char* src, const char* target,
                         const char* options = nullptr,
                         const char* fstype  = nullptr,
                         int attempts    = 3,
                         int baseDelayMs = 500)
{
    int rc = -1;
    for (int i = 0; i < attempts; ++i) {
        if (GlobalState::g_operationCancelled.load()) return -1;

        rc = libMount(src, target, options, fstype);
        if (rc == 0) return 0;

        if (i + 1 < attempts)
            sleepWithJitter(baseDelayMs * (i + 1));
    }
    return rc;
}

/**
 * @brief Mounts an NTFS partition through a specific kernel driver, with
 *        retry and jittered backoff.
 *
 * Builds a fresh libmnt_context on every attempt, so no state from a failed
 * attempt carries over. Mount helpers are disabled so the VFS invokes the
 * named kernel driver directly (see getBestNtfsDriver()).
 *
 * @param src         NTFS partition node (e.g. "/dev/sdb2").
 * @param target      Existing mount point.
 * @param fstype      Kernel driver token ("ntfs" or "ntfs3").
 * @param attempts    Maximum number of attempts (default 3).
 * @param baseDelayMs Base backoff in milliseconds (default 500).
 * @return 0 on success, -1 if cancelled, otherwise the return code of the
 *         last attempt.
 */
static int libMountNtfsRetry(const char* src, const char* target,
                             const char* fstype,
                             int attempts    = 3,
                             int baseDelayMs = 500)
{
    int rc = -1;
    for (int i = 0; i < attempts; ++i) {
        if (GlobalState::g_operationCancelled.load()) return -1;

        libmnt_context* ctx = mnt_new_context();
        if (ctx) {
            mnt_context_set_source(ctx, src);
            mnt_context_set_target(ctx, target);
            mnt_context_set_fstype(ctx, fstype);
            mnt_context_append_options(ctx, "noatime");
            mnt_context_disable_helpers(ctx, true);
            rc = mnt_context_mount(ctx);
            mnt_free_context(ctx);
        } else {
            rc = -1;
        }
        if (rc == 0) return 0;

        if (i + 1 < attempts)
            sleepWithJitter(baseDelayMs * (i + 1));
    }
    return rc;
}

/** Layout variants that drive partitioning and copy strategy. */
enum class IsoType { WindowsInstall, FatOnly };

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
static IsoType detectIsoType(const std::string& isoMnt) {
    return (fs::exists(isoMnt + "/sources/install.wim") ||
            fs::exists(isoMnt + "/sources/install.esd"))
               ? IsoType::WindowsInstall
               : IsoType::FatOnly;
}

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

namespace detail {

/** @brief Minimum size for NTFS pre-allocation via posix_fallocate. */
constexpr uint64_t kFallocateMinSize    = 1 * 1024 * 1024;

/** @brief Maximum restarts allowed for an unaligned O_DIRECT short write. */
constexpr int      kMaxUnalignedRetries = 3;

/** @brief Target size of each ping-pong I/O buffer (rounded down to sector). */
constexpr size_t   kDesiredBuffer       = 4 * 1024 * 1024;

/** @brief A single file or directory discovered during the ISO tree walk. */
struct IsoCopyEntry {
    fs::path src;      ///< Path inside the read-only ISO mount.
    fs::path dst;      ///< Destination path on the FAT32 or NTFS mount.
    uint64_t size;     ///< Source size in bytes (0 for directories).
    bool     isDir;    ///< True if the entry is a directory.
};

/**
 * @brief Return the larger of a block device's logical and physical sector
 *        sizes, falling back to 512 on any open/ioctl failure or non-positive
 *        result.
 * @param devNode Block device node (e.g. /dev/sdb1).
 * @return max(BLKSSZGET, BLKPBSZGET), or 512 on failure.
 */
int queryBestSectorSize(const std::string& devNode) {
    int fd = open(devNode.c_str(), O_RDONLY);
    if (fd < 0) return 512;
    int logicalSize  = 512;
    int physicalSize = 512;
    if (ioctl(fd, BLKSSZGET,  &logicalSize)  < 0 || logicalSize  <= 0) logicalSize  = 512;
    if (ioctl(fd, BLKPBSZGET, &physicalSize) < 0 || physicalSize <= 0) physicalSize = 512;
    close(fd);
    return std::max(logicalSize, physicalSize);
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

            double smoothingFactor = (instantSpeed < 10.0) ? 0.6 : alpha;

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

/**
 * @brief Fill @p bufV from @p fd_in up to @p bufferSize bytes, accumulating
 *        short reads and retrying EINTR; only read()==0 ends a chunk early.
 *
 * @return Bytes filled (0 = EOF), or -1 on error / cancellation.
 */
ssize_t fillChunk(int fd_in, void* bufV, size_t bufferSize) {
    unsigned char* buf = static_cast<unsigned char*>(bufV);
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
 * @brief Write one chunk at @p fileOffset, advancing it by the source-byte
 *        count (not the aligned padded length) on success.
 *
 * Applies O_DIRECT padding to @p sectorSize alignment, retries EINTR,
 * treats a 0-byte write as EIO, and on an unaligned O_DIRECT short write
 * restarts the chunk up to kMaxUnalignedRetries times while preserving the
 * per-chunk credit high-water mark so already-counted source bytes are not
 * double-credited.
 *
 * @return true if the chunk was written completely; false otherwise.
 */
bool writeOneChunk(int fd_out, void* bufV, ssize_t bytesRead,
                   uint64_t& fileOffset, bool useBufferedIO,
                   size_t sectorSize, size_t mask,
                   uint64_t& bytesCopiedTotal,
                   std::atomic<uint64_t>& totalAccumulator) {
    unsigned char* buf = static_cast<unsigned char*>(bufV);

    ssize_t writeLen = bytesRead;
    if (!useBufferedIO) {
        const ssize_t aligned = (bytesRead + static_cast<ssize_t>(mask)) &
                                ~static_cast<ssize_t>(mask);
        if (aligned > bytesRead) {
            memset(buf + bytesRead, 0, static_cast<size_t>(aligned - bytesRead));
        }
        writeLen = aligned;
    }

    ssize_t bytes_written = 0;
    uint64_t creditedHighWater = 0;
    int unalignedRetries = 0;

    while (bytes_written < writeLen) {
        if (GlobalState::g_operationCancelled.load()) return false;

        const off_t writeOffset =
            static_cast<off_t>(fileOffset) + static_cast<off_t>(bytes_written);

        const ssize_t written = pwrite(fd_out, buf + bytes_written,
                                       static_cast<size_t>(writeLen - bytes_written),
                                       writeOffset);

        if (written < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (written == 0) { errno = EIO; return false; }

        // A positive unaligned O_DIRECT short write may leave part of the
        // destination inconsistent. Restart the whole chunk from its original
        // aligned offset, with the original buffer.
        if (!useBufferedIO &&
            (static_cast<size_t>(written) % static_cast<size_t>(sectorSize)) != 0) {
            if (++unalignedRetries > kMaxUnalignedRetries) {
                errno = EIO;
                return false;
            }
            // Do not reset creditedHighWater: already credited source bytes
            // must not be counted a second time.
            bytes_written = 0;
            continue;
        }

        bytes_written += written;

        const uint64_t realBytesSoFar = static_cast<uint64_t>(
            std::min<ssize_t>(bytes_written, bytesRead));
        if (realBytesSoFar > creditedHighWater) {
            const uint64_t delta = realBytesSoFar - creditedHighWater;
            totalAccumulator.fetch_add(delta);
            bytesCopiedTotal += delta;
            creditedHighWater = realBytesSoFar;
        }
    }

    // The next chunk starts after actual source bytes, not any padding added
    // to the final O_DIRECT write.
    fileOffset += static_cast<uint64_t>(bytesRead);
    return true;
}

/**
 * @brief Copy a file that fits in one buffer on the calling thread
 *        (no reader thread is spawned).
 * @return true on success; false on read/write error or cancellation.
 */
bool copySmallFile(int fd_in, int fd_out, void* buf,
                   size_t bufferSize, bool useBufferedIO,
                   size_t sectorSize, size_t mask,
                   uint64_t& bytesCopiedTotal,
                   std::atomic<uint64_t>& totalAccumulator) {
    uint64_t fileOffset = 0;
    for (;;) {
        const ssize_t n = fillChunk(fd_in, buf, bufferSize);
        if (n < 0) return false;
        if (n == 0) break;
        if (!writeOneChunk(fd_out, buf, n, fileOffset, useBufferedIO,
                           sectorSize, mask, bytesCopiedTotal, totalAccumulator))
            return false;
    }
    return true;
}

/**
 * @brief Copy a file larger than one buffer using a short-lived reader
 *        thread and two ping-pong slots.
 *
 * The reader is always stopped and joined before returning. Writer failures
 * and cancellation are propagated to the reader via a shared flag, and any
 * already-published slot is written first (per "Double buffering").
 *
 * @return true on success; false on read/write error or cancellation.
 */
bool copyLargeFile(int fd_in, int fd_out, void* bufA, void* bufB,
                   size_t bufferSize, bool useBufferedIO,
                   size_t sectorSize, size_t mask,
                   uint64_t& bytesCopiedTotal,
                   std::atomic<uint64_t>& totalAccumulator) {
    void* slotBuf[2] = { bufA, bufB };
    struct Slot { ssize_t bytes = 0; bool full = false; };
    Slot slots[2];

    std::mutex              mtx;
    std::condition_variable cv;
    bool readerDone = false;
    bool readFailed = false;
    bool stopReader = false;
    bool success    = true;

    std::thread reader;
    try {
        reader = std::thread([&]() {
            for (size_t i = 0; ; ++i) {
                const size_t s = i & 1;
                {
                    std::unique_lock<std::mutex> lk(mtx);
                    cv.wait(lk, [&] { return !slots[s].full || stopReader; });
                    if (stopReader) break;
                }

                const ssize_t n = fillChunk(fd_in, slotBuf[s], bufferSize);

                bool finished = false;
                {
                    std::lock_guard<std::mutex> lk(mtx);
                    if (n < 0)      { readFailed = true; finished = true; }
                    else if (n == 0){ finished = true; }
                    else            { slots[s].bytes = n; slots[s].full = true; }
                }
                cv.notify_all();
                if (finished) break;
            }
            {
                std::lock_guard<std::mutex> lk(mtx);
                readerDone = true;
            }
            cv.notify_all();
        });
    } catch (...) {
        success = false;
    }

    if (reader.joinable()) {
        uint64_t fileOffset = 0;
        for (size_t i = 0; ; ++i) {
            const size_t s = i & 1;
            ssize_t n = 0;

            {
                std::unique_lock<std::mutex> lk(mtx);
                cv.wait(lk, [&] { return slots[s].full || readerDone; });
                // Slots are published in order, so if this one is not full
                // and the reader is done, there is no more data.
                if (!slots[s].full) break;
                n = slots[s].bytes;
            }

            if (!writeOneChunk(fd_out, slotBuf[s], n, fileOffset,
                               useBufferedIO, sectorSize, mask,
                               bytesCopiedTotal, totalAccumulator)) {
                success = false;
                break;
            }

            {
                std::lock_guard<std::mutex> lk(mtx);
                slots[s].full = false;
            }
            cv.notify_all();
        }

        // Always stop and join the reader before returning (and thus before
        // the caller closes the fds).
        {
            std::lock_guard<std::mutex> lk(mtx);
            stopReader = true;
        }
        cv.notify_all();
        reader.join();

        if (readFailed) success = false;
    }

    return success;
}

/**
 * @brief Per-file copy entry point: opens src/dst, chooses buffered (NTFS)
 *        vs. O_DIRECT (FAT32) strategy, applies posix_fallocate above the
 *        threshold, checks for a truncated source before the FAT32 final
 *        ftruncate, and performs the conditional ftruncate/fadvise cleanup.
 *
 * @return true only if the whole file was copied and no cancellation is in
 *         effect; false otherwise.
 */
bool copyFileWithProgress(const fs::path& src, const fs::path& dst,
                          uint64_t fileSize, bool useBufferedIO,
                          int sectorSize, size_t bufferSize,
                          void* bufA, void* bufB,
                          std::atomic<uint64_t>& totalAccumulator) {
    const int fd_in = open(src.c_str(), O_RDONLY);
    if (fd_in < 0) return false;
    posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

    int outFlags = O_WRONLY | O_CREAT | O_TRUNC;
    if (!useBufferedIO) outFlags |= O_DIRECT;

    const int fd_out = open(dst.c_str(), outFlags, 0644);
    if (fd_out < 0) { close(fd_in); return false; }

    if (useBufferedIO && fileSize >= kFallocateMinSize)
        posix_fallocate(fd_out, 0, static_cast<off_t>(fileSize));

    const size_t mask = static_cast<size_t>(sectorSize) - 1;
    uint64_t bytesCopiedTotal = 0;
    bool success;

    if (fileSize <= bufferSize) {
        success = copySmallFile(fd_in, fd_out, bufA, bufferSize, useBufferedIO,
                                sectorSize, mask, bytesCopiedTotal, totalAccumulator);
    } else {
        success = copyLargeFile(fd_in, fd_out, bufA, bufB, bufferSize,
                                useBufferedIO, sectorSize, mask,
                                bytesCopiedTotal, totalAccumulator);
    }

    // Detect a source shorter than the walk reported. Run before the FAT32
    // ftruncate that would otherwise zero-extend and mask truncation.
    if (success && !GlobalState::g_operationCancelled.load() &&
        bytesCopiedTotal < fileSize) {
        success = false;
    }

    if (!useBufferedIO) {
        if (GlobalState::g_operationCancelled.load()) {
            [[maybe_unused]] const int r = ftruncate(fd_out, 0);
        } else if (success) {
            if (ftruncate(fd_out, static_cast<off_t>(fileSize)) != 0)
                success = false;
        }
        if (!success) {
            [[maybe_unused]] const int r = ftruncate(fd_out, 0);
        }
    }

    if (GlobalState::g_operationCancelled.load())
        posix_fadvise(fd_out, 0, 0, POSIX_FADV_DONTNEED);

    posix_fadvise(fd_in, 0, 0, POSIX_FADV_DONTNEED);

    close(fd_in);
    close(fd_out);

    return success && !GlobalState::g_operationCancelled.load();
}

/**
 * @brief Iterate one entry list, creating directories and copying files
 *        with the correct sector size per destination filesystem.
 *
 * @return false on the first copy failure or cancellation; true otherwise.
 */
bool processEntries(const std::vector<IsoCopyEntry>& entries,
                    IsoType isoType, const std::string& ntfsMnt,
                    int fatSectorSize, int ntfsSectorSize,
                    size_t bufferSize,
                    void* bufA, void* bufB,
                    std::atomic<uint64_t>& totalAccumulator) {
    for (const auto& e : entries) {
        if (GlobalState::g_operationCancelled.load()) return false;

        if (e.isDir) {
            fs::create_directories(e.dst);
        } else {
            fs::create_directories(e.dst.parent_path());

            const bool toNtfs = (isoType == IsoType::WindowsInstall) &&
                                (e.dst.string().rfind(ntfsMnt, 0) == 0);
            const int sectorSize = toNtfs ? ntfsSectorSize : fatSectorSize;

            if (!copyFileWithProgress(e.src, e.dst, e.size, toNtfs, sectorSize,
                                      bufferSize, bufA, bufB, totalAccumulator)) {
                return false;
            }
        }
    }
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
        partResult = runCommandRetry({"parted", "-s", device,
                                      "mklabel", "gpt",
                                      "mkpart", "ESP",  "fat32", "1MiB",    "1025MiB",
                                      "mkpart", "DATA", "ntfs",  "1025MiB", "100%",
                                      "set", "1", "esp",  "on",
                                      "set", "1", "boot", "on"});
    } else {
        partResult = runCommandRetry({"parted", "-s", device,
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
    if (runCommandRetry({"mkfs.fat", "-F", "32", "-n",
                         (isoType == IsoType::WindowsInstall ? "WINBOOT" : "WINPE"),
                         fatPart}) != 0) {
        return false;
    }
    if (isoType == IsoType::WindowsInstall) {
        if (runCommandRetry({"mkfs.ntfs", "-f", "-c", "4096", "-L", "WINDATA",
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
                            ScopedMount& fatScope,
                            ScopedMount& ntfsScope) {
    if (libMountRetry(fatPart.c_str(), fatMnt.c_str(), "noatime") != 0)
        return false;
    fatScope.setMounted();

    if (isoType == IsoType::WindowsInstall) {
        const std::string ntfsDriver = getBestNtfsDriver();
        if (ntfsDriver.empty()) return false;
        if (libMountNtfsRetry(ntfsPart.c_str(), ntfsMnt.c_str(),
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

} // namespace detail

namespace {
namespace win_iso_steps {

using EntryVec    = std::vector<detail::IsoCopyEntry>;

inline bool isCancelled() { return GlobalState::g_operationCancelled.load(); }

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
inline bool mountIso(const std::string& isoPath, const char* isoMntBuf,
                     ScopedMount& isoScope)
{
    if (libMountRetry(isoPath.c_str(), isoMntBuf, "ro,loop") != 0) return false;
    isoScope.setMounted();
    return true;
}

// Wipe, partition, format and query sector sizes. Returns false on any step
// failure or if cancellation is observed after the sector-size queries.
inline bool prepareDevice(const std::string& device, IsoType isoType,
                          std::string& fatPart, std::string& ntfsPart,
                          int& fatSectorSize, int& ntfsSectorSize)
{
    if (!detail::setupPartitionedDevice(device, isoType, fatPart, ntfsPart))
        return false;
    if (!detail::formatTargetPartitions(fatPart, ntfsPart, isoType))
        return false;

    fatSectorSize  = detail::queryBestSectorSize(fatPart);
    ntfsSectorSize = (isoType == IsoType::WindowsInstall)
                         ? detail::queryBestSectorSize(ntfsPart) : 512;
    return !isCancelled();
}

// The NTFS temp dir only exists for WindowsInstall images.
inline bool makeNtfsDirIfNeeded(IsoType isoType, char* ntfsMntBuf)
{
    return isoType != IsoType::WindowsInstall || mkdtemp(ntfsMntBuf) != nullptr;
}

inline std::string ntfsScopePath(IsoType isoType, const char* ntfsMntBuf)
{
    return isoType == IsoType::WindowsInstall ? std::string(ntfsMntBuf)
                                              : std::string{};
}

inline bool mountTargets(IsoType isoType, const std::string& fatPart,
                         const std::string& ntfsPart, const std::string& fatMnt,
                         const std::string& ntfsMnt, ScopedMount& fatScope,
                         ScopedMount& ntfsScope)
{
    if (!detail::mountTargetFilesystems(isoType, fatPart, ntfsPart,
                                        fatMnt, ntfsMnt, fatScope, ntfsScope))
        return false;
    return !isCancelled();
}

// Fails if enumeration fails or the ISO contributes no bytes.
inline bool enumerateNonEmpty(const std::string& isoMnt, IsoType isoType,
                              const std::string& fatMnt, const std::string& ntfsMnt,
                              EntryVec& ntfsEntries, EntryVec& espEntries,
                              uint64_t& totalBytes)
{
    if (!detail::enumerateIsoEntries(isoMnt, isoType, fatMnt, ntfsMnt,
                                     ntfsEntries, espEntries, totalBytes))
        return false;
    return totalBytes != 0;
}

inline bool buffersValid(AlignedBuffer& a, AlignedBuffer& b) { return a && b; }

// Drain the NTFS write-back backlog before the O_DIRECT ESP pass starts.
// Buffered NTFS writes only land in the page cache, so without this the
// kernel keeps flushing the NTFS data in the background while the ESP
// pass writes directly to a different region of the same device. Two
// competing write streams make cheap flash crawl near the end of the
// copy. The return value is not checked: a flush error here is not
// reported (same as the final syncfs), and the unmount still blocks until
// all I/O completes.
inline void flushNtfsBacklog(IsoType isoType, const std::string& ntfsMnt)
{
    if (isoType == IsoType::WindowsInstall && !isCancelled())
        detail::syncMount(ntfsMnt);
}

inline void finalFlush(IsoType isoType, const std::string& ntfsMnt,
                       const std::string& fatMnt)
{
    if (isCancelled()) return;
    if (isoType == IsoType::WindowsInstall) detail::syncMount(ntfsMnt);
    detail::syncMount(fatMnt);
}

// NTFS payload first, then ESP, so the bootloader only lands after the data
// it references is already on disk. runPass copies one entry list.
template <class RunPass>
bool runCopyPasses(RunPass&& runPass, IsoType isoType,
                   const std::string& ntfsMnt, const std::string& fatMnt,
                   EntryVec& ntfsEntries, EntryVec& espEntries)
{
    if (!runPass(ntfsEntries)) return false;
    flushNtfsBacklog(isoType, ntfsMnt);
    if (!runPass(espEntries)) return false;
    finalFlush(isoType, ntfsMnt, fatMnt);
    return true;
}

inline double averageSpeedMBps(uint64_t totalBytes, double seconds)
{
    if (seconds <= 0.0) return 0.0;
    return (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / seconds;
}

inline void reportSuccess(ProgressInfo& slot, double avgSpeed)
{
    slot.speed.store(avgSpeed);
    slot.progress.store(100);
    slot.completed.store(true);
}

} // namespace win_iso_steps
} // namespace

/**
 * @brief Wipe a block device, repartition it, and write a Windows-family ISO
 *        using a hybrid I/O model.
 *
 * The ISO is loop-mounted read-only and copied file-by-file onto freshly
 * formatted filesystems. Symlinks in the ISO are silently skipped to prevent
 * path-traversal leaks.
 *
 * @par Partitioning & Copy Order
 * Chosen automatically via detectIsoType():
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
 * - **FAT32 (O_DIRECT):** Bypasses page cache using two ping-pong buffers
 *   (4 MiB rounded down to the max logical/physical sector size, fallback
 *   512 B). Final chunks are zero-padded to sector alignment and trimmed via
 *   @c ftruncate(). Unaligned positive writes trigger up to 3 chunk restarts
 *   from the original offset (never falling back to buffered I/O).
 * - **NTFS (Buffered):** Manual @c read()/pwrite() loop. Files >= 1 MiB are
 *   pre-allocated with @c posix_fallocate().
 * - **Double Buffering:** Files larger than one buffer spawn a reader thread
 *   (at most one slot ahead of the calling writer thread); smaller files copy
 *   synchronously on the calling thread.
 * - **Integrity:** Short reads accumulate until full or EOF; @c EINTR is
 *   retried; zero-byte writes fail immediately. Sources yielding fewer bytes
 *   than reported by the ISO walk fail the operation. On failure or
 *   cancellation, FAT32 destinations are truncated to 0 bytes; NTFS files are
 *   left as-is.
 *
 * @par Lifecycle, Telemetry & Cleanup
 * - **Concurrency & RAII:** Fully re-entrant per job. Mounts (@c ScopedMount),
 *   buffers (@c AlignedBuffer), and threads (@c ThreadGuard) clean up
 *   automatically on all return paths.
 * - **Cancellation:** Aborts cleanly during setup or I/O loops without setting
 *   the slot's @c failed flag (except @c mkdtemp() failures, which always set
 *   @c failed).
 * - **Telemetry:** A background thread polls every 500 ms and updates EMA
 *   speed once per second (holding last speed up to 300 s during USB stalls).
 *   Progress caps at 99% during copy and hits 100% upon completion.
 * - **Flush & Unmount:** Calls @c syncfs() between NTFS and ESP passes and
 *   before unmount. @c ScopedMount destructors block on @c safeUmount() as the
 *   function returns—after @c completed is marked true—so callers must wait
 *   for the function to return before advising device removal.
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
    ScopedMount isoScope(isoMntBuf);

    if (!steps::mountIso(isoPath, isoMntBuf, isoScope))
        return report.failUnlessCancelled();

    const std::string isoMnt  = isoMntBuf;
    const IsoType     isoType = detectIsoType(isoMnt);

    std::string fatPart, ntfsPart;
    int fatSectorSize = 0, ntfsSectorSize = 512;
    if (!steps::prepareDevice(device, isoType, fatPart, ntfsPart,
                              fatSectorSize, ntfsSectorSize))
        return report.failUnlessCancelled();

    char fatMntBuf[]  = "/tmp/win_fat_XXXXXX";
    char ntfsMntBuf[] = "/tmp/win_ntfs_XXXXXX";

    if (!mkdtemp(fatMntBuf)) return report.fail();
    ScopedMount fatScope(fatMntBuf);

    if (!steps::makeNtfsDirIfNeeded(isoType, ntfsMntBuf)) return report.fail();
    ScopedMount ntfsScope(steps::ntfsScopePath(isoType, ntfsMntBuf));

    const std::string fatMnt  = fatMntBuf;
    const std::string ntfsMnt = ntfsMntBuf;

    if (!steps::mountTargets(isoType, fatPart, ntfsPart,
                             fatMnt, ntfsMnt, fatScope, ntfsScope))
        return report.failUnlessCancelled();

    std::vector<detail::IsoCopyEntry> ntfsEntries, espEntries;
    uint64_t totalBytes = 0;

    if (!steps::enumerateNonEmpty(isoMnt, isoType, fatMnt, ntfsMnt,
                                  ntfsEntries, espEntries, totalBytes))
        return report.failUnlessCancelled();

    std::atomic<uint64_t> totalBytesWrittenAccumulator{0};
    std::atomic<bool>     monitoringActive{true};
    const auto startTime = std::chrono::high_resolution_clock::now();

    std::thread progressMonitorThread([&, totalBytes]() {
        detail::runProgressMonitor(totalBytesWrittenAccumulator, monitoringActive,
                                   totalBytes, slot);
    });

    // Ensures the thread is always stopped and joined on any return path.
    ThreadGuard threadGuard(progressMonitorThread, monitoringActive);

    const int    maxSectorSize = std::max(fatSectorSize, ntfsSectorSize);
    const size_t bufferSize    = (detail::kDesiredBuffer / maxSectorSize) * maxSectorSize;

    AlignedBuffer ioBufA(bufferSize, maxSectorSize);
    AlignedBuffer ioBufB(bufferSize, maxSectorSize);
    if (!steps::buffersValid(ioBufA, ioBufB))
        return report.failUnlessCancelled();

    auto runPass = [&](std::vector<detail::IsoCopyEntry>& entries) {
        return detail::processEntries(entries, isoType, ntfsMnt,
                                      fatSectorSize, ntfsSectorSize,
                                      bufferSize, ioBufA.data, ioBufB.data,
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

#include <array>
#include <vector>
#include <string_view>
#include <queue>
#include <algorithm>
#include <cstdint>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * @brief Represents a node in the Aho-Corasick automaton trie.
 */
struct AcNode {
    std::array<int, 256> next{};
    int fail{0};
    uint32_t match{0};
    AcNode() { next.fill(-1); }
};

/**
 * @brief Builds and returns the static Aho-Corasick automaton for Windows ISO signatures.
 *
 * Searches simultaneously for "boot.wim", "bootmgr.efi", and "bootmgr" case-insensitively
 * across ASCII, UTF-16LE, and UTF-16BE encodings in a single O(n) pass.
 *
 * @return const std::vector<AcNode>& Reference to the immutable automaton node collection.
 */
static const std::vector<AcNode>& getAhoCorasickAutomaton() {
    static const auto nodes = [] {
        std::vector<AcNode> trie;
        trie.emplace_back();

        auto addPattern = [&](std::string_view text, int logicalIndex, int variant) {
            int cur = 0;
            for (char raw : text) {
                unsigned char c = static_cast<unsigned char>(std::tolower(raw));
                uint8_t bytes[2];
                int byteCount;
                if (variant == 0) {
                    bytes[0] = c;
                    byteCount = 1;
                } else if (variant == 1) {
                    bytes[0] = c; bytes[1] = 0x00;
                    byteCount = 2;
                } else {
                    bytes[0] = 0x00; bytes[1] = c;
                    byteCount = 2;
                }
                for (int b = 0; b < byteCount; ++b) {
                    int byte = bytes[b];
                    if (trie[cur].next[byte] == -1) {
                        trie[cur].next[byte] = static_cast<int>(trie.size());
                        trie.emplace_back();
                    }
                    cur = trie[cur].next[byte];
                }
            }
            trie[cur].match |= (1u << (logicalIndex * 3 + variant));
        };

        for (int v = 0; v < 3; ++v) {
            addPattern("boot.wim",    0, v);
            addPattern("bootmgr.efi", 1, v);
            addPattern("bootmgr",     2, v);
        }

        // BFS to build failure links
        std::queue<int> q;
        for (int c = 0; c < 256; ++c) {
            if (trie[0].next[c] == -1) {
                trie[0].next[c] = 0;
            } else {
                trie[trie[0].next[c]].fail = 0;
                q.push(trie[0].next[c]);
            }
        }
        while (!q.empty()) {
            int u = q.front(); q.pop();
            trie[u].match |= trie[trie[u].fail].match;
            for (int c = 0; c < 256; ++c) {
                int v = trie[u].next[c];
                if (v == -1) {
                    trie[u].next[c] = trie[trie[u].fail].next[c];
                } else {
                    trie[v].fail = trie[trie[u].fail].next[c];
                    q.push(v);
                }
            }
        }

        return trie;
    }();
    return nodes;
}

/**
 * @brief Returns a static 256-entry case-folding lookup table.
 *
 * Replaces std::tolower in the hot scan loop to avoid locale machinery
 * and branch overhead across the full mapping window.
 *
 * @return const std::array<uint8_t, 256>& Reference to the lookup table.
 */
static const std::array<uint8_t, 256>& getToLowerTable() {
    static const auto table = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[i] = static_cast<uint8_t>(std::tolower(i));
        return t;
    }();
    return table;
}

/**
 * @brief Validates the volume headers of the mapped ISO image.
 *
 * Checks for a Primary Volume Descriptor (type 0x01, identifier "CD001") at sector 16,
 * or pure-UDF identifiers ("NSR02" / "NSR03").
 *
 * @param addr    Pointer to the mapped file memory window.
 * @param mapSize Size of the mapped window in bytes.
 * @return true if the volume header is valid; false otherwise.
 */
static bool isValidVolumeHeader(const char* addr, size_t mapSize) {
    if (mapSize < 0x8006) return false;
    const uint8_t descType = static_cast<uint8_t>(addr[0x8000]);
    std::string_view id(addr + 0x8001, 5);
    const bool validIso = (descType == 0x01 && id == "CD001");
    const bool validUdf = (id == "NSR02" || id == "NSR03");
    return validIso || validUdf;
}

/**
 * @brief Scans the mapped memory window using the Aho-Corasick automaton with early exit.
 *
 * @param addr    Pointer to the mapped file memory window.
 * @param mapSize Size of the mapped window in bytes.
 * @return uint32_t Bitmask representing the matched signature groups.
 */
static uint32_t scanForSignatures(const char* addr, size_t mapSize) {
    const auto& ac = getAhoCorasickAutomaton();
    const auto& toLower = getToLowerTable();

    constexpr uint32_t MASK_BOOTWIM = 0b000000111u;
    constexpr uint32_t MASK_BOOTMGR = 0b111111000u;
    constexpr uint32_t MASK_ALL     = MASK_BOOTWIM | MASK_BOOTMGR;

    uint32_t found = 0;
    int cur = 0;
    for (size_t i = 0; i < mapSize; ++i) {
        cur    = ac[cur].next[toLower[static_cast<uint8_t>(addr[i])]];
        found |= ac[cur].match;

        if ((found & MASK_ALL) == MASK_ALL) break;
    }

    return found;
}

/**
 * @brief Probes an ISO image to detect Windows 10/11 installation media.
 *
 * Maps up to the first 32 MB of the file into the process address space and
 * performs a single-pass Aho-Corasick scan for Windows Boot Manager and WinPE
 * payload signatures. The 32 MB window is sufficient because Windows ISO
 * directory entries for bootmgr and boot.wim always appear within the first
 * few MB of the image.
 *
 * @par Volume validation
 * Requires a Primary Volume Descriptor (type 0x01, identifier "CD001") at
 * sector 16. Pure-UDF images with "NSR02"/"NSR03" are also accepted; "BEA01"
 * alone is not sufficient.
 *
 * @par Signature scan
 * Searches simultaneously for "bootmgr[.efi]" and "boot.wim"
 * case-insensitively across ASCII, UTF-16LE, and UTF-16BE in a single O(n)
 * pass. UTF-16LE is the native encoding used by Windows for Joliet/UDF
 * filenames. The automaton is built once on first call and reused across
 * subsequent calls.
 *
 * @par Case folding
 * A static 256-entry lookup table replaces std::tolower in the hot scan loop,
 * avoiding locale machinery and branch overhead across the full mapping
 * window.
 *
 * @par Early exit
 * The scan terminates as soon as both signature groups are matched, without
 * scanning the remainder of the window. On typical Windows ISOs both
 * signatures appear within the first 2-3 MB, so up to ~29 MB of iteration is
 * skipped in the common case.
 *
 * @param fd       Open, readable file descriptor positioned anywhere — the
 *                 mapping is always made from offset 0. The caller retains
 *                 ownership; this function does not close the descriptor.
 * @param fileSize Size of the image in bytes, as returned by fstat. Used for
 *                 the minimum-size guard and to clamp the mapping window.
 * @return true if the volume header is valid and both "bootmgr[.efi]" and
 *         "boot.wim" signatures are present within the mapped window.
 *
 * @note Does not modify the file; read-only mapping.
 * @note The static Aho-Corasick automaton and case-folding table are both
 *       thread-safe under C++11 and later (guaranteed by static-local
 *       initialisation rules).
 * @note MADV_WILLNEED is applied to the first 4 MB of the mapping to begin
 *       faulting in pages immediately; MADV_SEQUENTIAL covers the remainder so
 *       the prefetcher stays primed if a full scan is needed.
 */
bool isWindowsIsoInitialCheck(int fd, off_t fileSize) {
    if (fileSize < 0x9000) return false;

    const size_t mapSize = std::min(static_cast<size_t>(fileSize),
                                    static_cast<size_t>(32 * 1024 * 1024));
    char* addr = static_cast<char*>(
        mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (addr == MAP_FAILED) return false;

    // Hint the kernel to prefetch the first 4 MiB immediately
    madvise(addr,                  4 * 1024 * 1024, MADV_WILLNEED);
    madvise(addr, mapSize,                          MADV_SEQUENTIAL);

    if (!isValidVolumeHeader(addr, mapSize)) {
        munmap(addr, mapSize);
        return false;
    }

    const uint32_t found = scanForSignatures(addr, mapSize);

    munmap(addr, mapSize);

    constexpr uint32_t MASK_BOOTWIM = 0b000000111u;
    constexpr uint32_t MASK_BOOTMGR = 0b111111000u;

    return (found & MASK_BOOTWIM) != 0 &&
           (found & MASK_BOOTMGR) != 0;
}
