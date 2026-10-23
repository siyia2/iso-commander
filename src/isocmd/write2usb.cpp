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
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <queue>
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

    // Redirect child's stdout/stderr to /dev/null via posix_spawn's
    // file_actions — this replaces the dup2() calls that used to run
    // post-fork in the child.
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
 * This function evaluates system capabilities via sysfs, ensuring seamless
 * compatibility with both dynamically loaded kernel modules (.ko) and drivers
 * built directly into the kernel core. It utilizes a lazy-loading optimization
 * strategy, checking for pre-initialized drivers first and only executing
 * userspace `modprobe` commands for lower priority/fallback tiers if necessary.
 *
 * Following the modern NTFS driver overhaul in Linux 7.1+, this function
 * prioritizes the heavily optimized, modernized "ntfs" driver rewrite, which
 * entirely supersedes the legacy read-only engine. It falls back to Paragon's
 * "ntfs3" driver if running on older kernels (like 5.15 through 6.x) where the
 * modern remake is unavailable.
 *
 * @note Because modern Linux distributions frequently symlink userspace binaries
 * or default to older FUSE wrappers, downstream mounting logic should pass the
 * internal flag ("mount", "-i", "-t", driver, ...) when using the returned token.
 * This bypasses userspace mount helpers and forces the VFS subsystem to invoke
 * the designated kernel module directly.
 *
 * Driver Priority Tiering:
 * 1. "ntfs"  via Kernel 7.1+ (Modernized Native Read/Write Engine) — Highest Priority
 * 2. "ntfs3" via Kernel 5.15+ (Paragon Engine) — Fallback
 *
 * @return std::string The token name of the detected NTFS kernel driver ("ntfs" or "ntfs3"),
 * or an empty string if no modern write-capable driver is available.
 */
std::string getBestNtfsDriver() {
    // First Pass: Check if the modern 7.1+ NTFSPLUS driver is already active/built-in
    if (std::filesystem::exists("/sys/module/ntfs")) {
        return "ntfs";
    }

    // Demand-load the modern 7.1+ NTFSPLUS driver if it was dormant as a module
    runCommand({"modprobe", "ntfs"});
    if (std::filesystem::exists("/sys/module/ntfs")) {
        return "ntfs";
    }

    // Modern driver failed/unavailable. Lazy-load the Paragon ntfs3 fallback (Kernel 5.15+)
    runCommand({"modprobe", "ntfs3"});
    if (std::filesystem::exists("/sys/module/ntfs3")) {
        return "ntfs3";
    }

    // Return empty if no modern native write-capable drivers could be verified
    return {};
}

// ---------------------------------------------------------------------------
// Wipefs replacement (libblkid)
// ---------------------------------------------------------------------------

/**
 * @brief Erase all filesystem/partition signatures on @p device.
 *
 * Equivalent to `wipefs -a <device>`.  Uses libblkid's probe-and-wipe loop
 * so no child process is spawned.
 *
 * @param device Block device path (e.g. "/dev/sdb").
 * @return true on success (including the case where no signatures were found).
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

// ---------------------------------------------------------------------------
// Internal mount / umount / helpers (libmount)
// ---------------------------------------------------------------------------

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
 * * This function handles the full lifecycle of an unmount operation. By reusing
 * the context, it ensures that flags like MNT_DETACH and LOOPDEL are applied
 * consistently, even if an initial standard unmount fails.
 * * @param path The mount point or device path to unmount.
 */
void safeUmount(const char* path) {
    libmnt_context* ctx = mnt_new_context();
    if (!ctx) return;

    // Set the target path and ensure the loop device is deleted
    // automatically by the kernel once the filesystem reference count hits 0.
    mnt_context_set_target(ctx, path);
    mnt_context_enable_loopdel(ctx, true);

    // PRIORITY 1: Cancellation Path
    // If the operation is cancelled, bypass standard unmounts and
    // go straight to lazy detachment for immediate release.
    if (GlobalState::g_operationCancelled.load()) {
        mnt_context_enable_lazy(ctx, true);
    }

    // Attempt the unmount.
    // If mnt_context_umount returns non-zero, it means the device is 'busy'.
    if (mnt_context_umount(ctx) != 0) {

        // PRIORITY 2: Fallback Path
        // If not already lazy, enable lazy detachment and retry the
        // operation on the same context to force cleanup.
        mnt_context_enable_lazy(ctx, true);
        mnt_context_umount(ctx);
    }

    // Clean up context memory
    mnt_free_context(ctx);
}

/**
 * @brief Detects whether an ISO image is Windows installation media.
 *
 * Loop-mounts the ISO read-only into a temporary directory and checks
 * for the co-presence of sources/boot.wim (Windows setup payload)
 * and bootmgr / bootmgr.efi (Windows boot manager).
 *
 * Uses libmount instead of forking mount/umount processes.
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

// ---------------------------------------------------------------------------
// Windows Family ISO type detection
// ---------------------------------------------------------------------------

/** Layout variants that drive partitioning and copy strategy. */
enum class IsoType { WindowsInstall, FatOnly };

/**
 * @brief Classify a mounted ISO as a full Windows install or FAT-only boot disk.
 *
 * Presence of @c sources/install.wim or @c sources/install.esd is the
 * canonical indicator of a Windows install image.  WinPE, Hiren's Boot CD,
 * and other rescue disks lack these files and are treated as @c FatOnly.
 *
 * @param isoMnt Mount point of the loop-mounted ISO (read-only).
 * @return @c WindowsInstall if an install payload is found, @c FatOnly otherwise.
 */
static IsoType detectIsoType(const std::string& isoMnt) {
    // A genuine Windows install ISO always ships sources/install.wim or
    // sources/install.esd.  Everything else (Hiren's, rescue disks, WinPE
    // variants without an install image, …) goes through the FAT-only path.
    return (fs::exists(isoMnt + "/sources/install.wim") ||
            fs::exists(isoMnt + "/sources/install.esd"))
               ? IsoType::WindowsInstall
               : IsoType::FatOnly;
}

/**
 * @brief Checks if a block device is accessible and ready for I/O operations.
 * * Attempts to open the device node in read-only and non-blocking mode.
 * This effectively tests if the kernel has fully registered the block device
 * and that it is not currently locked or in an invalid state.
 * * @param path The filesystem path to the device node (e.g., "/dev/sdb1").
 * @return true If the device can be opened successfully.
 * @return false If the device does not exist, access is denied, or it is busy.
 */
bool isDeviceReady(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (fd == -1) return false;
    close(fd);
    return true;
}

/**
 * @brief Waits for a device node to appear and become ready within a specified timeout.
 * * This function performs an informed poll by checking for the existence of the
 * device node and verifying its readiness via isDeviceReady(). It provides an
 * init-agnostic alternative to `udevadm settle` or `udevadm wait`.
 * * @param path The filesystem path to the device node to wait for.
 * @param timeout_seconds The maximum duration to wait in seconds (default is 30).
 * @return true If the device node is found and is ready for use within the timeout.
 * @return false If the timeout is reached before the device is ready.
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
// Windows writer
// ---------------------------------------------------------------------------


/**
 * @brief Write a Windows-family ISO image to a block device.
 *
 * Mounts the source ISO read-only, wipes and repartitions the target device,
 * formats the resulting partition(s), and copies the ISO contents file by
 * file to the appropriate filesystem(s).
 *
 * @section winiso_layout Partition layout
 *
 * Partitioning is selected automatically according to @c detectIsoType:
 *
 * - **WindowsInstall** - GPT with two partitions:
 *   1. ESP: 1 MiB to 1025 MiB (1 GiB), FAT32, label @c WINBOOT. Contains
 *      the top-level @c efi/ and @c boot/ directories and
 *      @c sources/boot.wim, matched case-insensitively.
 *   2. DATA: 1025 MiB to the end of the device, NTFS, label @c WINDATA.
 *      Contains all remaining entries, including files in the ISO root.
 *
 * - **FatOnly** (WinPE, Hiren's, rescue disks) - GPT with one FAT32
 *   partition, label @c WINPE, spanning 1 MiB to the end of the device.
 *   All ISO entries are copied to this partition.
 *
 * Partition node names are derived by appending the partition number to
 * @p device (for example, @c /dev/sdb1). Devices requiring a @c p separator
 * before the partition number, such as @c /dev/nvme0n1 and @c /dev/mmcblk0,
 * are not supported by this naming scheme.
 *
 * The device and partition nodes are awaited using @c waitForDevice.
 * Partition nodes are given a 30-second timeout to accommodate udev delays.
 *
 * @section winiso_mounting Mounting and entry classification
 *
 * - The ISO is mounted read-only using @c ro,loop.
 * - FAT32 is mounted with @c noatime.
 * - NTFS is mounted using the driver selected by @c getBestNtfsDriver,
 *   with @c noatime and mount helpers disabled.
 *
 * ISO entries are classified for the destination filesystem before copying.
 * For Windows installation ISOs, @c efi/, @c boot/, and
 * @c sources/boot.wim are assigned to the ESP; all other entries are
 * assigned to NTFS. For @c FatOnly images, every entry is assigned to FAT32.
 *
 * Symlinks encountered during the ISO directory walk are silently skipped.
 * This prevents ISO symlinks from directing the copy operation to arbitrary
 * host paths. Entries available only through symlinks are absent from the
 * destination.
 *
 * @section winiso_io Copy strategy and I/O error handling
 *
 * Both FAT32 and NTFS destination files use buffered file I/O. Each file is
 * copied using @c read() and @c pwrite() with a reusable 4 MiB buffer.
 * The implementation does not use @c O_DIRECT and does not require
 * sector-aligned user buffers, offsets, or write lengths.
 *
 * - Reads accumulate data until the buffer is full or @c read() returns
 *   zero to indicate EOF. A short positive read is not treated as EOF.
 * - @c EINTR is retried on reads and writes.
 * - Writes use @c pwrite() with explicit file offsets and continue until
 *   the entire chunk has been written.
 * - A zero-byte write is treated as an I/O failure to prevent an infinite
 *   loop when the destination stops making progress.
 * - Other read or write errors fail the current file copy.
 * - Copy order is NTFS payload first, followed by the ESP. @c FatOnly
 *   images have no NTFS pass.
 *
 * The expected size of each regular file is captured during the ISO walk.
 * After copying, the implementation checks that the number of source bytes
 * copied is not smaller than the expected size. A source that yields fewer
 * bytes than expected causes the operation to fail, unless cancellation
 * has already been requested.
 *
 * This size check does not independently verify destination contents against
 * the source and does not detect every possible source modification. A source
 * that grows after the directory walk may cause the copied byte count to
 * exceed the precomputed total.
 *
 * On failure or cancellation, FAT32 destination files are truncated to zero
 * length on a best-effort basis. NTFS destination files are left as-is.
 * Errors from these cleanup truncation attempts are ignored.
 *
 * The source and destination file descriptors are closed after copying,
 * but errors returned by @c close() are not checked. Errors from
 * @c posix_fadvise() are also ignored.
 *
 * Buffered I/O avoids the alignment and final-chunk padding requirements
 * associated with @c O_DIRECT. It does not eliminate partial writes,
 * delayed writeback errors, or device and filesystem failures.
 *
 * @section winiso_progress Progress reporting and concurrency
 *
 * Each invocation uses its own progress slot, temporary mount directories,
 * I/O buffer, byte counter, and progress-monitor thread. Independent jobs can
 * therefore run concurrently, provided the caller supplies distinct progress
 * slots and target devices.
 *
 * A dedicated background thread polls progress every 500 ms. The copy loop
 * increments the shared byte counter after each successful @c pwrite().
 * Reported progress is capped at 99% while copying.
 *
 * Speed is recalculated no more frequently than once per second using an
 * exponential moving average. The smoothing factor is 0.3 normally and 0.6
 * when instantaneous speed is below 10 MiB/s.
 *
 * During a stall, the last measured speed is retained for up to 300 seconds
 * before the displayed speed is set to zero. The measurement baseline is
 * not reset during a stall, so the next speed sample accounts for the full
 * elapsed interval.
 *
 * At finalization, the displayed average speed is calculated using the
 * precomputed total source bytes and the elapsed time since the progress
 * monitor's start time. This elapsed time excludes setup and partitioning.
 * The calculation uses the expected total size, not the actual accumulated
 * number of bytes written.
 *
 * @section winiso_cancel Cancellation and failure handling
 *
 * Cancellation is checked at several setup checkpoints, during ISO entry
 * collection, between files, and inside the per-file read and write loops.
 * A cancellation detected during copying aborts the current operation.
 *
 * Setup steps that fail while @c GlobalState::g_operationCancelled is already
 * set return @c false without setting the failed flag through
 * @c failUnlessCancelled. Failures of @c mkdtemp use @c fail() directly
 * and therefore set the failed flag even if cancellation is already active.
 *
 * Genuine copy failures set the corresponding @c progressData slot's
 * @c failed flag through @c fail(). Cancellation paths normally return
 * @c false without setting that flag through the failure helper.
 *
 * The I/O buffer allocation failure sets the failed flag only if cancellation
 * is not active. An exception during ISO directory traversal is handled as a
 * failure unless cancellation is active. Exceptions from directory creation
 * during copying are not caught by @c processEntries.
 *
 * Directory creation uses @c fs::create_directories, but its return value
 * is not checked. A directory creation error that does not throw may
 * therefore go undetected until a subsequent file operation fails.
 *
 * Mount points are managed by @c ScopedMount. The progress-monitor thread
 * is managed by @c ThreadGuard, which stops and joins the thread when its
 * guard is destroyed, including on normal returns and early exits.
 *
 * @section winiso_flush Flush, unmount, and completion
 *
 * After all files have been copied, and only if cancellation has not been
 * requested at that checkpoint, @c syncfs is called on the NTFS mount for
 * Windows installation ISOs and then on the FAT32 mount.
 *
 * The return values of @c syncfs and the corresponding @c close() calls are
 * ignored. Failure to open a mount point for synchronization is also ignored.
 * Consequently, synchronization errors are not reported by this function.
 *
 * The progress-monitor thread is stopped and joined before final progress
 * accounting. The @c ScopedMount objects are local variables and are
 * destroyed only when the function returns, in reverse declaration order:
 * NTFS, FAT32, then the ISO. Thus, final progress is published before these
 * unmount attempts finish. Unmount failures cannot change the return value
 * or the already-published completion state.
 *
 * Finalization sets the displayed speed to the calculated average, sets
 * progress to 100%, sets @c completed to true, and returns @c true.
 * The current implementation does not perform a final cancellation check
 * immediately before publishing these values.
 *
 * Callers must not interpret @c completed alone as proof that synchronization
 * or unmounting succeeded, or that cancellation was not requested during
 * finalization.
 *
 * @param isoPath       Path to the source ISO file.
 * @param device        Target block device (for example, @c /dev/sdb).
 *                      The device will be wiped and repartitioned.
 * @param progressIndex Index into the shared @c progressData array for
 *                      progress, speed, failure, and completion reporting.
 *
 * @return @c true when the function reaches its successful finalization path;
 *         @c false on a reported failure or cancellation. Genuine errors
 *         generally set the slot's @c failed flag, while cancellation paths
 *         normally return @c false without setting it through the failure
 *         helper. Some errors, including synchronization and unmount errors,
 *         are not propagated by the current implementation.
 */
bool writeWindowsIsoToDevice(const std::string& isoPath,
                             const std::string& device,
                             size_t             progressIndex)
{
    auto fail = [&]() -> bool {
        progressData[progressIndex].failed.store(true);
        return false;
    };

    auto failUnlessCancelled = [&]() -> bool {
        if (GlobalState::g_operationCancelled.load()) return false;
        return fail();
    };

    // ------------------------------------------------------------------ //
    // 0. Mount the ISO to inspect its layout                              //
    // ------------------------------------------------------------------ //
    char isoMntBuf[] = "/tmp/win_iso_XXXXXX";
    if (!mkdtemp(isoMntBuf)) return fail();

    // ScopedMount always removes the dir; setMounted() arms the safeUmount.
    ScopedMount isoScope(isoMntBuf);

    if (libMount(isoPath.c_str(), isoMntBuf, "ro,loop") != 0) return failUnlessCancelled();
    isoScope.setMounted();

    const std::string isoMnt  = isoMntBuf;
    const IsoType     isoType = detectIsoType(isoMnt);

    // ------------------------------------------------------------------ //
    // 1. Wipe + repartition                                               //
    // ------------------------------------------------------------------ //
    if (!wipeDeviceSignatures(device)) return failUnlessCancelled();

    int partResult = -1;
    if (isoType == IsoType::WindowsInstall) {
        partResult = runCommand({"parted", "-s", device,
                                 "mklabel", "gpt",
                                 "mkpart", "ESP",  "fat32", "1MiB",    "1025MiB",
                                 "mkpart", "DATA", "ntfs",  "1025MiB", "100%",
                                 "set", "1", "esp",  "on",
                                 "set", "1", "boot", "on"});
    } else {
        partResult = runCommand({"parted", "-s", device,
                                 "mklabel", "gpt",
                                 "mkpart", "WINPE", "fat32", "1MiB", "100%",
                                 "set", "1", "esp",  "on",
                                 "set", "1", "boot", "on"});
    }
    if (partResult != 0) return failUnlessCancelled();

    if (!waitForDevice(device)) return failUnlessCancelled();

    auto derivePartition = [&](int n) -> std::string {
        std::string target = device + std::to_string(n);
        return waitForDevice(target, 30) ? target : std::string{};
    };

    const std::string fatPart  = derivePartition(1);
    const std::string ntfsPart = (isoType == IsoType::WindowsInstall)
                                     ? derivePartition(2)
                                     : std::string{};

    if (fatPart.empty()) return failUnlessCancelled();
    if (isoType == IsoType::WindowsInstall && ntfsPart.empty()) return failUnlessCancelled();

    if (runCommand({"mkfs.fat", "-F", "32", "-n",
                    (isoType == IsoType::WindowsInstall ? "WINBOOT" : "WINPE"),
                    fatPart}) != 0) {
        return failUnlessCancelled();
    }

    if (isoType == IsoType::WindowsInstall) {
        if (runCommand({"mkfs.ntfs", "-f", "-c", "4096", "-L", "WINDATA",
                        ntfsPart}) != 0) {
            return failUnlessCancelled();
        }
    }

    if (GlobalState::g_operationCancelled.load()) return false;

    // ------------------------------------------------------------------ //
    // 2. Mount FAT32 partition (and NTFS for Windows installs)            //
    // ------------------------------------------------------------------ //
    char fatMntBuf[]  = "/tmp/win_fat_XXXXXX";
    char ntfsMntBuf[] = "/tmp/win_ntfs_XXXXXX";

    if (!mkdtemp(fatMntBuf)) return fail();
    ScopedMount fatScope(fatMntBuf);

    // ntfsScope is armed only for WindowsInstall; an empty path is a no-op.
    if (isoType == IsoType::WindowsInstall) {
        if (!mkdtemp(ntfsMntBuf)) return fail();
    }
    ScopedMount ntfsScope(isoType == IsoType::WindowsInstall
                              ? std::string(ntfsMntBuf)
                              : std::string{});

    const std::string fatMnt  = fatMntBuf;
    const std::string ntfsMnt = ntfsMntBuf;

    if (libMount(fatPart.c_str(), fatMntBuf, "noatime") != 0) return failUnlessCancelled();
    fatScope.setMounted();

    if (isoType == IsoType::WindowsInstall) {
        const std::string ntfsDriver = getBestNtfsDriver();
        if (ntfsDriver.empty()) return failUnlessCancelled();

        libmnt_context* ctx = mnt_new_context();
        mnt_context_set_source(ctx, ntfsPart.c_str());
        mnt_context_set_target(ctx, ntfsMntBuf);
        mnt_context_set_fstype(ctx, ntfsDriver.c_str());
        mnt_context_append_options(ctx, "noatime");
        mnt_context_disable_helpers(ctx, true);
        const int rc = mnt_context_mount(ctx);
        mnt_free_context(ctx);

        if (rc != 0) return failUnlessCancelled();
        ntfsScope.setMounted();
    }

    if (GlobalState::g_operationCancelled.load()) return false;

    // ------------------------------------------------------------------ //
    // 3. Collect and classify ISO entries                                 //
    // ------------------------------------------------------------------ //
    struct IsoEntry {
        fs::path src;
        fs::path dst;
        uint64_t size;
        bool     isDir;
    };

    std::vector<IsoEntry> ntfsEntries;
    std::vector<IsoEntry> espEntries;
    uint64_t totalBytes = 0;

    const std::unordered_set<std::string> espTopFolders = { "efi", "boot" };
    const std::string espBootWim = "sources/boot.wim";

    auto belongsOnESP = [&](const fs::path& rel) -> bool {
        if (isoType == IsoType::FatOnly) return true;

        std::string top = rel.begin()->string();
        std::transform(top.begin(), top.end(), top.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (espTopFolders.count(top) > 0) return true;

        std::string relStr = rel.generic_string();
        std::transform(relStr.begin(), relStr.end(), relStr.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        return relStr == espBootWim;
    };

    try {
        for (const auto& entry : fs::recursive_directory_iterator(isoMnt)) {

            // Reject symlinks outright — a malicious ISO (Rock Ridge/UDF) could
            // embed a symlink pointing at an arbitrary host path (e.g. /etc/shadow).
            // Following it here would read host files with this process's
            // privileges and copy their contents onto the target device.
            if (fs::is_symlink(entry.symlink_status())) {
                continue;
            }

            const fs::path rel   = fs::relative(entry.path(), isoMnt);
            const bool     toESP = belongsOnESP(rel);
            const fs::path dest  = fs::path(toESP ? fatMnt : ntfsMnt) / rel;
            const uint64_t sz    = entry.is_regular_file() ? entry.file_size() : 0;

            totalBytes += sz;

            IsoEntry e{ entry.path(), dest, sz, entry.is_directory() };
            if (toESP) espEntries.push_back(std::move(e));
            else       ntfsEntries.push_back(std::move(e));
        }
    } catch (...) {
        if (!GlobalState::g_operationCancelled.load()) return fail();
        return false;
    }

    if (totalBytes == 0 ) {
        if (!GlobalState::g_operationCancelled.load()) {
            return fail();
        } else {
            return false;
        }
    }
    // ------------------------------------------------------------------ //
    // 4. Async progress monitoring thread                                 //
    // ------------------------------------------------------------------ //
    std::atomic<uint64_t> totalBytesWrittenAccumulator{0};
    std::atomic<bool>     monitoringActive{true};
    const auto startTime = std::chrono::high_resolution_clock::now();

    std::thread progressMonitorThread([&, totalBytes, progressIndex]() {
        auto     lastUpdate    = std::chrono::high_resolution_clock::now();
        uint64_t lastWritten   = 0;
        double   smoothedSpeed = 0.0;
        bool     haveEstimate  = false;

        // Stall tracking
        auto     stallStart    = std::chrono::high_resolution_clock::now();
        bool     stalled       = false;

        // Higher alpha = more reactive to phase changes (NTFS -> ESP), lower = smoother.
        constexpr double alpha = 0.3;
        // Minimum time between speed updates to avoid distortion
        constexpr int MIN_UPDATE_INTERVAL_MS = 1000;
        // Show 0 only after this many seconds of continuous stalling
        constexpr int STALL_TIMEOUT_SECONDS = 300;

        while (monitoringActive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            const auto now = std::chrono::high_resolution_clock::now();
            const auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    now - lastUpdate).count();

            const uint64_t currentWritten = totalBytesWrittenAccumulator.load();
            progressData[progressIndex].bytesWritten.store(currentWritten);
            progressData[progressIndex].progress.store(static_cast<int>(
                std::min(99.0, (static_cast<double>(currentWritten) / totalBytes) * 100.0)));

            const uint64_t deltaBytes = (currentWritten >= lastWritten)
                                            ? (currentWritten - lastWritten)
                                            : 0;

            // Only calculate speed if enough time has passed for meaningful measurement
            if (deltaBytes > 0 && ms >= MIN_UPDATE_INTERVAL_MS) {
                // Progress detected - reset stall tracking
                stalled = false;

                const double instantSpeed = (static_cast<double>(deltaBytes) /
                                                (1024.0 * 1024.0)) / (ms / 1000.0);

                // Use adaptive smoothing: more reactive for slow drives
                double smoothingFactor = (instantSpeed < 10.0) ? 0.6 : alpha;

                smoothedSpeed = haveEstimate
                    ? (smoothingFactor * instantSpeed + (1.0 - smoothingFactor) * smoothedSpeed)
                    : instantSpeed;
                haveEstimate = true;
                progressData[progressIndex].speed.store(smoothedSpeed);

                // Only update timestamps when we've actually measured something
                lastWritten = currentWritten;
                lastUpdate = now;
            } else if (deltaBytes == 0 && ms >= MIN_UPDATE_INTERVAL_MS) {
                // Stall detected - track how long it's been.
                // IMPORTANT: do NOT reset lastUpdate/lastWritten here. A long
                // in-flight write (e.g. a throttled 4MiB chunk on a slow USB
                // NTFS target) can span many 500ms polling ticks with
                // deltaBytes == 0 the whole time. Resetting the baseline on
                // every tick would make the eventual big delta land right
                // after the last reset, understating elapsed time and
                // artificially flooring the reported speed at roughly
                // bufferSize / MIN_UPDATE_INTERVAL_MS (~4 MB/s). By leaving
                // lastUpdate/lastWritten untouched, once real progress does
                // land, ms reflects the true elapsed time since data last
                // moved and the speed calculation above is accurate.
                if (!stalled) {
                    stalled = true;
                    stallStart = now;
                }

                auto stallDuration = std::chrono::duration_cast<std::chrono::seconds>(
                                        now - stallStart).count();

                // Keep last known speed for STALL_TIMEOUT_SECONDS, then show 0
                if (stallDuration >= STALL_TIMEOUT_SECONDS) {
                    progressData[progressIndex].speed.store(0.0);
                }
                // Else: keep the existing smoothedSpeed (don't change it)
            }
            // If ms < MIN_UPDATE_INTERVAL_MS, we do nothing - keep old speed value
            // and don't update timestamps to accumulate more measurement time
        }
    });

    // ThreadGuard ensures the thread is always stopped and joined on any
    // return path — success, failure, or cancellation.
    ThreadGuard threadGuard(progressMonitorThread, monitoringActive);

    // ------------------------------------------------------------------ //
    // 5. Buffered I/O buffer                                             //
    // ------------------------------------------------------------------ //
    constexpr size_t bufferSize = 4 * 1024 * 1024;
    std::vector<char> ioBuf;
    try {
        ioBuf.resize(bufferSize);
    } catch (...) {
        if (!GlobalState::g_operationCancelled.load()) return fail();
        return false;
    }

    // ------------------------------------------------------------------ //
    // 6. Per-file copy using buffered I/O                                //
    // ------------------------------------------------------------------ //
    auto copyWithProgress = [&](const fs::path& src, const fs::path& dst,
                                uint64_t fileSize) -> bool {
        const bool isNtfsDest = (isoType == IsoType::WindowsInstall) &&
                                (dst.string().rfind(ntfsMnt, 0) == 0);

        const int fd_in = open(src.c_str(), O_RDONLY);
        if (fd_in < 0) return false;
        posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

        // Buffered I/O is used for both FAT32 and NTFS destinations.
        const int fd_out = open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_out < 0) {
            close(fd_in);
            return false;
        }

        bool success = true;
        uint64_t bytesCopiedTotal = 0;
        uint64_t fileOffset = 0;

        while (!GlobalState::g_operationCancelled.load()) {
            size_t bytesReadThisChunk = 0;

            // Accumulate reads until the buffer is full or EOF is reached.
            while (bytesReadThisChunk < bufferSize) {
                if (GlobalState::g_operationCancelled.load()) {
                    success = false;
                    goto done;
                }

                const ssize_t bytesRead = read(
                    fd_in,
                    ioBuf.data() + bytesReadThisChunk,
                    bufferSize - bytesReadThisChunk);

                if (bytesRead < 0) {
                    if (errno == EINTR) continue;
                    success = false;
                    goto done;
                }
                if (bytesRead == 0) break;

                bytesReadThisChunk += static_cast<size_t>(bytesRead);
            }

            if (bytesReadThisChunk == 0) break;

            size_t bytesWrittenThisChunk = 0;
            while (bytesWrittenThisChunk < bytesReadThisChunk) {
                if (GlobalState::g_operationCancelled.load()) {
                    success = false;
                    goto done;
                }

                const ssize_t bytesWritten = pwrite(
                    fd_out,
                    ioBuf.data() + bytesWrittenThisChunk,
                    bytesReadThisChunk - bytesWrittenThisChunk,
                    static_cast<off_t>(fileOffset + bytesWrittenThisChunk));

                if (bytesWritten < 0) {
                    if (errno == EINTR) continue;
                    success = false;
                    goto done;
                }
                if (bytesWritten == 0) {
                    errno = EIO;
                    success = false;
                    goto done;
                }

                bytesWrittenThisChunk += static_cast<size_t>(bytesWritten);
                bytesCopiedTotal += static_cast<uint64_t>(bytesWritten);
                totalBytesWrittenAccumulator.fetch_add(static_cast<uint64_t>(bytesWritten));
            }

            fileOffset += static_cast<uint64_t>(bytesReadThisChunk);
        }

    done:
        // Keep the original short-source check and destination cleanup rules.
        if (success && !GlobalState::g_operationCancelled.load() &&
            bytesCopiedTotal < fileSize) {
            success = false;
        }

        if (!isNtfsDest) {
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
    };

    // ------------------------------------------------------------------ //
    // 7. Copy passes (NTFS payload first, ESP second)                    //
    // ------------------------------------------------------------------ //

    // Returns false on failure or cancellation; RAII handles all cleanup.
    auto processEntries = [&](const std::vector<IsoEntry>& entries) -> bool {
        for (const auto& e : entries) {
            if (GlobalState::g_operationCancelled.load()) return false;

            if (e.isDir) {
                fs::create_directories(e.dst);
            } else {
                fs::create_directories(e.dst.parent_path());

                if (!copyWithProgress(e.src, e.dst, e.size)) {
                    if (!GlobalState::g_operationCancelled.load()) {
                        return fail();
                    } else {
                        return false;
                    }
                }
            }
        }
        return true;
    };

    if (!processEntries(ntfsEntries)) return false;
    if (!processEntries(espEntries))  return false;

    // ThreadGuard destructor stops + joins the monitor thread here on any
    // early return. On the success path it fires at end of scope below.

    // ------------------------------------------------------------------ //
    // 8. Flush filesystems before unmounting                             //
    // ------------------------------------------------------------------ //
    // syncfs pushes all dirty pages at the filesystem level. safeUmount
    // (called by ScopedMount destructors) then blocks until the kernel
    // confirms all I/O has reached the device.
    if (!GlobalState::g_operationCancelled.load()) {
        if (isoType == IsoType::WindowsInstall) {
            const int ntfsFd = open(ntfsMnt.c_str(), O_RDONLY);
            if (ntfsFd >= 0) { syncfs(ntfsFd); close(ntfsFd); }
        }
        const int fatFd = open(fatMnt.c_str(), O_RDONLY);
        if (fatFd >= 0) { syncfs(fatFd); close(fatFd); }
    }

    threadGuard.stop();  // Sets monitoringActive=false and joins

    // ScopedMount destructors for ntfsScope, fatScope, isoScope fire here
    // in reverse declaration order: ntfs → fat → iso.

    // ------------------------------------------------------------------ //
    // 9. Final progress accounting                                        //
    // ------------------------------------------------------------------ //
    if (GlobalState::g_operationCancelled.load()) {
        return false;
    }
    const auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - startTime);
    const double seconds  = totalElapsed.count() / 1000.0;
    const double avgSpeed = (seconds > 0.0)
        ? (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / seconds
        : 0.0;

    progressData[progressIndex].speed.store(avgSpeed);
    progressData[progressIndex].progress.store(100);
    progressData[progressIndex].completed.store(true);
    return true;
}

/**
 * @brief Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
 *
 * Walks the Volume Recognition Sequence (sectors 16-23, 2048-byte spacing) with
 * @c pread, so the descriptor's offset is untouched. Accepts an ISO 9660 PVD
 * (type @c 0x01, "CD001"), which covers UDF-bridge images, or a UDF "NSR02"/"NSR03"
 * descriptor, which covers pure-UDF images. "CD001" non-PVD entries, "BEA01" and
 * "TEA01" are skipped; any other identifier rejects the image immediately.
 *
 * @param fd Open, readable descriptor for the image. Not closed by this function.
 * @return @c true if an accepted descriptor is found; @c false on a short read,
 *         an unrecognized identifier, or no match within 8 sectors.
 *
 * @note Acceptance does not imply the image is bootable. The name is historical;
 *       @c isValidOpticalImage would describe it more accurately.
 */
bool isValidIso9660(int fd) {
    // Volume Recognition Sequence starts at sector 16; walk a few descriptors.
    for (int i = 0; i < 8; ++i) {
        uint8_t d[7];
        if (pread(fd, d, sizeof(d), 32768 + i * 2048) != sizeof(d)) return false;

        std::string_view id(reinterpret_cast<char*>(d + 1), 5);
        if ((id == "CD001" && d[0] == 0x01) ||        // ISO 9660 PVD
            id == "NSR02" || id == "NSR03")           // UDF
            return true;
        if (id != "CD001" && id != "BEA01" && id != "TEA01")
            return false;                              // not a recognition sequence
    }
    return false;
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
 * @details
 * - **Volume validation:** Requires a Primary Volume Descriptor (type 0x01,
 *   identifier "CD001") at sector 16. Pure-UDF images with "NSR02"/"NSR03"
 *   are also accepted; "BEA01" alone is not sufficient.
 * - **Signature scan:** Searches simultaneously for "bootmgr[.efi]" and
 *   "boot.wim" case-insensitively across ASCII, UTF-16LE, and UTF-16BE in a
 *   single O(n) pass. UTF-16LE is the native encoding used by Windows for
 *   Joliet/UDF filenames. The automaton is built once on first call and reused
 *   across subsequent calls.
 * - **Case folding:** A static 256-entry lookup table replaces @c std::tolower
 *   in the hot scan loop, avoiding locale machinery and branch overhead across
 *   the full mapping window.
 * - **Early exit:** The scan terminates as soon as both signature groups are
 *   matched, without scanning the remainder of the window. On typical Windows
 *   ISOs both signatures appear within the first 2–3 MB, so up to ~29 MB of
 *   iteration is skipped in the common case.
 *
 * @param fd       Open, readable file descriptor positioned anywhere — the
 *                 mapping is always made from offset 0. The caller retains
 *                 ownership; this function does not close the descriptor.
 * @param fileSize Size of the image in bytes, as returned by @c fstat. Used
 *                 for the minimum-size guard and to clamp the mapping window.
 * @return @c true if the volume header is valid and both "bootmgr[.efi]" and
 *         "boot.wim" signatures are present within the mapped window.
 *
 * @note Does not modify the file; read-only mapping.
 * @note The static Aho-Corasick automaton and case-folding table are both
 *       thread-safe under C++11 and later (guaranteed by static-local
 *       initialisation rules).
 * @note @c MADV_WILLNEED is applied to the first 4 MB of the mapping to
 *       begin faulting in pages immediately; @c MADV_SEQUENTIAL covers the
 *       remainder so the prefetcher stays primed if a full scan is needed.
 */
bool isWindowsIsoInitialCheck(int fd, off_t fileSize) {

    // --- Aho-Corasick automaton (built once, shared across calls) ---
    // Logical patterns: 0 = "boot.wim", 1 = "bootmgr.efi", 2 = "bootmgr"
    // Variants: 0 = ASCII, 1 = UTF-16LE, 2 = UTF-16BE  →  bits 0–8
    static const auto ac = [] {
        struct Node {
            std::array<int, 256> next{};
            int fail{0};
            uint32_t match{0};
            Node() { next.fill(-1); }
        };

        std::vector<Node> nodes;
        nodes.emplace_back();

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
                    if (nodes[cur].next[byte] == -1) {
                        nodes[cur].next[byte] = static_cast<int>(nodes.size());
                        nodes.emplace_back();
                    }
                    cur = nodes[cur].next[byte];
                }
            }
            nodes[cur].match |= (1u << (logicalIndex * 3 + variant));
        };

        for (int v = 0; v < 3; ++v) {
            addPattern("boot.wim",    0, v);
            addPattern("bootmgr.efi", 1, v);
            addPattern("bootmgr",     2, v);
        }

        // BFS to build failure links
        std::queue<int> q;
        for (int c = 0; c < 256; ++c) {
            if (nodes[0].next[c] == -1) {
                nodes[0].next[c] = 0;
            } else {
                nodes[nodes[0].next[c]].fail = 0;
                q.push(nodes[0].next[c]);
            }
        }
        while (!q.empty()) {
            int u = q.front(); q.pop();
            nodes[u].match |= nodes[nodes[u].fail].match;
            for (int c = 0; c < 256; ++c) {
                int v = nodes[u].next[c];
                if (v == -1) {
                    nodes[u].next[c] = nodes[nodes[u].fail].next[c];
                } else {
                    nodes[v].fail = nodes[nodes[u].fail].next[c];
                    q.push(v);
                }
            }
        }

        return nodes;
    }();

    // Built once at program start; avoids locale machinery and branching
    // on every byte of the 32 MiB scan window.
    static const auto toLower = [] {
        std::array<uint8_t, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[i] = static_cast<uint8_t>(std::tolower(i));
        return t;
    }();

    // --- Size check ---
    if (fileSize < 0x9000) return false;

    const size_t mapSize = std::min(static_cast<size_t>(fileSize),
                                    static_cast<size_t>(32 * 1024 * 1024));
    char* addr = static_cast<char*>(
        mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (addr == MAP_FAILED) return false;

    // Hint the kernel to prefetch the first 4 MiB immediately — signatures
    // almost always appear within the first 2–3 MiB of a Windows ISO, so
    // WILLNEED on the head reduces time-to-first-match.  SEQUENTIAL covers
    // the remainder so readahead stays primed for the full scan if needed.
    madvise(addr,                  4 * 1024 * 1024, MADV_WILLNEED);
    madvise(addr, mapSize,                          MADV_SEQUENTIAL);

    // --- Volume header validation ---
    {
        const uint8_t descType = static_cast<uint8_t>(addr[0x8000]);
        std::string_view id(addr + 0x8001, 5);
        const bool validIso = (descType == 0x01 && id == "CD001");
        const bool validUdf = (id == "NSR02" || id == "NSR03");
        if (!validIso && !validUdf) {
            munmap(addr, mapSize);
            return false;
        }
    }

    // --- Single-pass Aho-Corasick scan ---
    // Early-exit masks: once both groups are matched there is no need to
    // scan further.  Saves up to ~30 MiB of iteration on typical ISOs where
    // both signatures appear near the front of the directory area.
    //
    // boot.wim  matched in any variant: bits 0–2
    // bootmgr   matched in any variant: bits 3–8
    constexpr uint32_t MASK_BOOTWIM = 0b000000111u;
    constexpr uint32_t MASK_BOOTMGR = 0b111111000u;
    constexpr uint32_t MASK_ALL     = MASK_BOOTWIM | MASK_BOOTMGR;

    uint32_t found = 0;
    int cur = 0;
    for (size_t i = 0; i < mapSize; ++i) {
        cur    = ac[cur].next[toLower[static_cast<uint8_t>(addr[i])]];
        found |= ac[cur].match;

        // Both groups matched — no need to continue scanning.
        if ((found & MASK_ALL) == MASK_ALL) break;
    }

    munmap(addr, mapSize);

    return (found & MASK_BOOTWIM) != 0 &&
           (found & MASK_BOOTMGR) != 0;
}

/**
 * @brief Writes an ISO image to a raw block device, auto-routing Windows vs. Linux configurations.
 *
 * High-level orchestration routine that opens the ISO once, validates it, then intercepts
 * Windows installation media using an optimized metadata signature probe. Standard/Linux
 * distributions fall through directly into a specialized, unbuffered block-level stream.
 *
 * @details
 * - **Single fd Lifecycle:** The ISO is opened once at entry via @c open(O_RDONLY) and
 * held for the duration of the call. The same descriptor is passed to @ref isValidIso9660
 * and @ref isWindowsIsoInitialCheck, avoiding redundant opens. File size is retrieved
 * once via @c fstat and reused across all paths.
 * - **Early Validation:** @ref isValidIso9660 is invoked before any device access or
 * routing decision, ensuring invalid images are rejected with minimal overhead.
 * - **Windows Path Routing:** If @ref isWindowsIsoInitialCheck passes, execution is
 * delegated to @ref writeWindowsIsoToDevice, which automatically handles multi-partitioning,
 * hybrid FAT32+NTFS tables, and cluster tuning.
 * - **Raw O_DIRECT Pipeline:** Non-Windows targets are imaged via raw unbuffered disk I/O.
 * Data transfers use two 4 MiB buffers allocated with @c posix_memalign (8 MiB per call),
 * each rounded down to a multiple of the device's effective sector size and aligned to it.
 * That size is derived by querying both the logical sector size (@c ioctl(BLKSSZGET)) and
 * the physical sector size (@c ioctl(BLKPBSZGET)) and taking the larger of the two. This
 * ensures correct @c O_DIRECT alignment on 512n, 512e, and 4Kn drives. @c BLKPBSZGET is
 * treated as best-effort and falls back to the logical size if the kernel or device does
 * not support it. @c POSIX_FADV_SEQUENTIAL is applied to the source descriptor before the
 * write loop to enable aggressive kernel read-ahead; @c POSIX_FADV_DONTNEED is applied
 * after to release the ISO from page cache once writing completes.
 * - **Double Buffering:** A dedicated reader thread fills one aligned buffer from the ISO
 * (including tail zero-padding) while the calling thread writes the other buffer to the
 * device. The two buffers circulate between an "empty" and a "full" queue, so the device
 * is not left idle while the next chunk is being read. The reader can run at most one
 * buffer ahead of the writer, which bounds memory use per call.
 * - **Chunked Device Writes:** Each device write() is sector-aligned and capped at 4 MiB.
 * Buffer address, length, and device offset all remain aligned for @c O_DIRECT. With the
 * current 4 MiB buffers each buffer goes out as a single write; the cap keeps progress
 * and cancellation granularity fine (important for slow USB drives, < 10 MB/s) if the
 * buffer size is increased later.
 * - **Progress Monitoring:** A separate thread samples the written-byte counter every 500 ms,
 * publishes progress (capped at 99% until completion), and computes an exponentially smoothed
 * speed (heavier smoothing below 10 MB/s). The last known speed is held through stalls and
 * reset to 0 after 60 s without progress. The monitor sleeps on a condition variable, so it
 * wakes immediately on shutdown.
 * - **Tail-Block Padding:** Detects partial blocks at EOF, automatically zero-padding the
 * remaining buffer slice up to the strict sector layout boundary to prevent kernel
 * rejection errors (@c EINVAL). Short @c read() results mid-file are not treated as EOF;
 * the reader keeps reading until the chunk is full or @c read() returns 0.
 * - **Asynchronous Cancellation Safety:** Evaluates @c GlobalState::g_operationCancelled
 * in both the reader and the writer. On user abort, it short-circuits execution, stops the
 * reader thread, and leaves the disk safely without triggering a cascading @c fsync block.
 *
 * @param isoPath Absolute path to the source ISO image file on the host.
 * @param device Target destination block node path (e.g., @c /dev/sdb). WARNING: All existing
 * underlying data will be destructively overwritten.
 * @param progressIndex Unique thread tracking identifier mapped inside the global progress array.
 * @return @c true if the image data was written in full and flushed to the device with
 * @c fsync; @c false on I/O failure, validation failure, or user cancellation. The written
 * data is not read back or verified against the ISO.
 *
 * @see isValidIso9660
 * @see isWindowsIsoInitialCheck
 * @see writeWindowsIsoToDevice
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex) {

    // Open ISO once — shared by all validation and write paths
    int iso_fd = open(isoPath.c_str(), O_RDONLY);
    if (iso_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    auto closeIso = [](int* fd) { if (*fd != -1) close(*fd); };
    std::unique_ptr<int, decltype(closeIso)> isoGuard(&iso_fd, closeIso);

    // Verify it's a valid ISO 9660 filesystem before proceeding
    if (!isValidIso9660(iso_fd)) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Seek back to beginning after validation probe
    if (lseek(iso_fd, 0, SEEK_SET) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Get file size once via fstat — reused by both routing and raw write path
    struct stat sb;
    if (fstat(iso_fd, &sb) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Fast-path: Windows ISO routing (borrows iso_fd)
    if (isWindowsIsoInitialCheck(iso_fd, sb.st_size)) {
        return writeWindowsIsoToDevice(isoPath, device, progressIndex);
    }

    // Seek back to beginning for raw write path
    if (lseek(iso_fd, 0, SEEK_SET) == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Hint sequential read pattern for aggressive read-ahead on the ISO source.
    posix_fadvise(iso_fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    // --- Raw O_DIRECT path for Linux / UEFI ISOs -------------------------

    // Open device with O_DIRECT for unbuffered writes
    int dev_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (dev_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    std::unique_ptr<int, decltype(closeIso)> devGuard(&dev_fd, closeIso);

    // Query logical and physical sector sizes; take the max so that
    // O_DIRECT alignment is correct for 512n, 512e, and 4Kn drives.
    int logicalSectorSize  = 0;
    int physicalSectorSize = 0;

    if (ioctl(dev_fd, BLKSSZGET, &logicalSectorSize) < 0 || logicalSectorSize <= 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    // BLKPBSZGET is best-effort; fall back to logical size if unavailable
    if (ioctl(dev_fd, BLKPBSZGET, &physicalSectorSize) < 0 || physicalSectorSize <= 0)
        physicalSectorSize = logicalSectorSize;

    const int sectorSize = std::max(logicalSectorSize, physicalSectorSize);

    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Pad file size up to sector boundary for O_DIRECT
    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    // Two aligned buffers (4 MiB each, rounded down to sector boundary).
    // These size the read-ahead / hand-off between reader and writer; they do NOT
    // dictate how large each device write() is (see writeChunk below).
    constexpr size_t DESIRED_BUFFER = 4 * 1024 * 1024;
    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    // Device writes are capped at 4 MiB and kept sector-aligned so the progress byte
    // counter advances at fine granularity. With the current 4 MiB buffers each buffer
    // is one write(); the cap matters if DESIRED_BUFFER is raised, because a single
    // very large write() on a slow USB drive (< 10 MB/s) blocks for many seconds and
    // makes the monitor's speed samples noisy. Must stay a multiple of sectorSize
    // for O_DIRECT.
    constexpr size_t MAX_WRITE_CHUNK = 4 * 1024 * 1024;
    static_assert(MAX_WRITE_CHUNK <= DESIRED_BUFFER,
                  "write chunk larger than the buffer is pointless");
    const size_t writeChunk = std::max<size_t>((MAX_WRITE_CHUNK / sectorSize) * sectorSize,
                                               static_cast<size_t>(sectorSize));

    constexpr int NUM_BUFFERS = 2;
    std::unique_ptr<char, decltype(&free)> bufferGuards[NUM_BUFFERS] = {
        {nullptr, &free}, {nullptr, &free}
    };
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        char* p = nullptr;
        if (posix_memalign(reinterpret_cast<void**>(&p), sectorSize, bufferSize) != 0) {
            progressData[progressIndex].failed.store(true);
            return false;
        }
        bufferGuards[i].reset(p);
    }

    // ------------------------------------------------------------------ //
    // Double-buffer queues shared between reader thread and writer (this) //
    // ------------------------------------------------------------------ //
    struct Chunk {
        char*  buf;
        size_t len;   // 0 == end-of-stream sentinel
    };

    std::mutex              qMutex;
    std::condition_variable qCv;
    std::deque<Chunk>       emptyQ;
    std::deque<Chunk>       fullQ;
    bool                    stopReader = false;   // set by writer to tell reader to exit
    bool                    readFailed = false;   // set by reader on read() error

    for (int i = 0; i < NUM_BUFFERS; ++i)
        emptyQ.push_back({bufferGuards[i].get(), 0});

    // ------------------------------------------------------------------ //
    // Asynchronous UI Status Thread Initialization                        //
    // ------------------------------------------------------------------ //
    std::atomic<uint64_t> directBytesWrittenAccumulator{0};
    std::atomic<bool> monitoringActive{true};
    std::mutex              monMutex;
    std::condition_variable monCv;

    auto startTime = std::chrono::high_resolution_clock::now();

    std::thread progressMonitorThread;
    std::thread readerThread;

    // Idempotent: stops and joins both helper threads.
    auto shutdownThreads = [&]() {
        {
            std::lock_guard<std::mutex> lk(qMutex);
            stopReader = true;
        }
        qCv.notify_all();
        if (readerThread.joinable()) readerThread.join();

        {
            std::lock_guard<std::mutex> lk(monMutex);
            monitoringActive.store(false);
        }
        monCv.notify_all();
        if (progressMonitorThread.joinable()) progressMonitorThread.join();
    };

    uint64_t localBytesWritten = 0;
    bool     cancelled = false;
    bool     ioFailed  = false;

    try {
        progressMonitorThread = std::thread([&, fileSize, progressIndex]() {
            auto     lastUpdate    = std::chrono::high_resolution_clock::now();
            uint64_t lastWritten   = 0;
            double   smoothedSpeed = 0.0;
            bool     haveEstimate  = false;

            // Stall tracking
            auto     stallStart    = std::chrono::high_resolution_clock::now();
            bool     stalled       = false;

            constexpr double alpha = 0.3;
            // Increase minimum time between updates for slow drives
            constexpr int MIN_UPDATE_INTERVAL_MS = 1000;
            // Show 0 only after this many seconds of continuous stalling
            constexpr int STALL_TIMEOUT_SECONDS = 60;

            while (monitoringActive.load()) {
                {
                    // Interruptible 500 ms tick: wakes immediately on shutdown
                    // instead of making the caller wait out a sleep.
                    std::unique_lock<std::mutex> lk(monMutex);
                    monCv.wait_for(lk, std::chrono::milliseconds(500),
                                   [&] { return !monitoringActive.load(); });
                }
                if (!monitoringActive.load()) break;

                auto now = std::chrono::high_resolution_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate).count();

                uint64_t currentWritten = directBytesWrittenAccumulator.load();

                // Always update progress even if no new data
                progressData[progressIndex].bytesWritten.store(currentWritten);
                progressData[progressIndex].progress.store(static_cast<int>(
                    std::min(99.0, (static_cast<double>(currentWritten) / fileSize) * 100.0)));

                uint64_t deltaBytes = (currentWritten >= lastWritten) ? (currentWritten - lastWritten) : 0;

                // Only calculate speed if we have enough data and enough time has passed
                if (deltaBytes > 0 && elapsed >= MIN_UPDATE_INTERVAL_MS) {
                    // Progress detected - reset stall tracking
                    stalled = false;

                    double instantSpeed = (static_cast<double>(deltaBytes) / (1024.0 * 1024.0)) / (elapsed / 1000.0);

                    // For slow drives (< 10 MB/s), apply heavier smoothing to reduce
                    // display jitter from bursty O_DIRECT writes at low throughput
                    double smoothingFactor = (instantSpeed < 10.0) ? 0.2 : alpha;

                    smoothedSpeed = haveEstimate
                        ? (smoothingFactor * instantSpeed + (1.0 - smoothingFactor) * smoothedSpeed)
                        : instantSpeed;
                    haveEstimate = true;

                    // Update speed only when we have a valid measurement
                    progressData[progressIndex].speed.store(smoothedSpeed);

                    // Only reset the baseline when bytes actually arrived — this keeps
                    // `elapsed` spanning back to the last real data point instead of an
                    // arbitrary polling tick, which was previously truncating the window
                    // to ~1s and producing a spurious floor (one write chunk per interval)
                    // on slow/bursty O_DIRECT writers.
                    lastWritten = currentWritten;
                    lastUpdate  = now;
                } else if (deltaBytes == 0 && elapsed >= MIN_UPDATE_INTERVAL_MS) {
                    // Stall detected - track how long it's been
                    if (!stalled) {
                        stalled = true;
                        stallStart = now;
                    }

                    auto stallDuration = std::chrono::duration_cast<std::chrono::seconds>(
                                            now - stallStart).count();

                    // Keep last known speed for STALL_TIMEOUT_SECONDS, then show 0
                    if (stallDuration >= STALL_TIMEOUT_SECONDS) {
                        progressData[progressIndex].speed.store(0.0);
                    }
                    // Else: keep the existing smoothedSpeed (don't change it)

                    // Deliberately do NOT touch lastWritten/lastUpdate here — let the
                    // elapsed window keep growing while a slow write is still in flight,
                    // so the eventual speed sample reflects the true transfer duration.
                }
            }
        });

        // ------------------------------------------------------------------ //
        // Reader thread: ISO -> aligned buffer (+ zero padding) -> fullQ     //
        // ------------------------------------------------------------------ //
        readerThread = std::thread([&]() {
            uint64_t readTotal = 0;

            auto pushSentinel = [&](bool failed) {
                {
                    std::lock_guard<std::mutex> lk(qMutex);
                    if (failed) readFailed = true;
                    fullQ.push_back({nullptr, 0});
                }
                qCv.notify_all();
            };

            while (readTotal < paddedSize && !GlobalState::g_operationCancelled.load()) {
                Chunk c;
                {
                    std::unique_lock<std::mutex> lk(qMutex);
                    qCv.wait(lk, [&] { return stopReader || !emptyQ.empty(); });
                    if (stopReader) return;
                    c = emptyQ.front();
                    emptyQ.pop_front();
                }

                const size_t toRead = static_cast<size_t>(
                    std::min<uint64_t>(bufferSize, paddedSize - readTotal));

                size_t got = 0;
                bool err = false;
                bool reachedEof = false;

                while (got < toRead) {
                    ssize_t r = read(iso_fd, c.buf + got, toRead - got);
                    if (r < 0) {
                        if (errno == EINTR) continue;
                        err = true;
                        break;
                    }
                    if (r == 0) {
                        reachedEof = true;
                        break;   // EOF reached
                    }
                    got += static_cast<size_t>(r);
                }

                if (err) {
                    pushSentinel(true);
                    return;
                }

                // CRITICAL CHECK: Did we hit EOF before reading the expected file size?
                if (reachedEof) {
                    if (readTotal + got < fileSize) {
                        // The file on disk is shorter than fstat reported (truncated file)!
                        pushSentinel(true);
                        return;
                    }
                }

                // Tail-block sector alignment padding (only valid if we hit true EOF at fileSize)
                if (got < toRead) {
                    std::memset(c.buf + got, 0, toRead - got);
                }

                c.len = toRead;
                readTotal += toRead;

                {
                    std::lock_guard<std::mutex> lk(qMutex);
                    fullQ.push_back(c);
                }
                qCv.notify_all();

                if (reachedEof) break; // Stop reading once actual EOF is hit
            }

            // Normal end of stream (or cancellation): tell the writer there is no more data.
            pushSentinel(false);
        });

        // ------------------------------------------------------------------ //
        // Writer (this thread): fullQ -> device in writeChunk pieces, then    //
        // recycle the buffer                                                  //
        // ------------------------------------------------------------------ //
        while (true) {
            Chunk c;
            {
                std::unique_lock<std::mutex> lk(qMutex);
                qCv.wait(lk, [&] { return !fullQ.empty(); });
                c = fullQ.front();
                fullQ.pop_front();
            }

            if (c.len == 0) break;   // sentinel: reader finished, failed, or was cancelled

            if (GlobalState::g_operationCancelled.load()) {
                cancelled = true;
                break;
            }

            // Write loop — handles partial writes and EINTR. Each write() is capped at
            // writeChunk (a multiple of sectorSize), so buffer address, length and
            // device offset all stay sector-aligned for O_DIRECT while progress is
            // reported (and cancellation checked) after every write of <= 4 MiB.
            size_t remainingToWrite = c.len;
            char*  writePtr         = c.buf;
            while (remainingToWrite > 0) {
                const size_t thisWrite = std::min(remainingToWrite, writeChunk);
                ssize_t written = write(dev_fd, writePtr, thisWrite);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    ioFailed = true;
                    break;
                }

                if (written == 0) {   // no progress; avoid infinite spin
                    ioFailed = true;
                    break;
                }

                if (GlobalState::g_operationCancelled.load()) {
                    cancelled = true;
                    break;
                }

                remainingToWrite -= static_cast<size_t>(written);
                writePtr         += written;

                // Safely cap reported progress at the true file size bounds to hide padding modifications
                uint64_t reportable = std::min<uint64_t>(
                    static_cast<uint64_t>(written),
                    fileSize - std::min(localBytesWritten, fileSize));

                localBytesWritten += static_cast<uint64_t>(written);

                directBytesWrittenAccumulator.fetch_add(reportable);
            }

            if (ioFailed || cancelled) break;

            // Hand the buffer back so the reader can refill it while we write the other one.
            {
                std::lock_guard<std::mutex> lk(qMutex);
                emptyQ.push_back({c.buf, 0});
            }
            qCv.notify_all();
        }
    } catch (...) {
        shutdownThreads();
        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

    // Safely terminate reader and UI update threads before triggering storage synchronization logic
    shutdownThreads();

    if (readFailed || ioFailed) {
        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

    // Drop ISO from page cache — no point keeping it after writing.
    posix_fadvise(iso_fd, 0, 0, POSIX_FADV_DONTNEED);

    if (!cancelled && !GlobalState::g_operationCancelled.load()) {
        if (fsync(dev_fd) != 0) {
            progressData[progressIndex].failed.store(true);
            return false;
        }
    }

    if (!cancelled && !GlobalState::g_operationCancelled.load() && localBytesWritten >= paddedSize) {
        auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - startTime);
        double seconds  = totalElapsed.count() / 1000.0;
        double avgSpeed = seconds > 0.0 ? (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / seconds : 0.0;

        progressData[progressIndex].speed.store(avgSpeed);
        progressData[progressIndex].progress.store(100);
        progressData[progressIndex].completed.store(true);
        return true;
    }

    return false;
}
