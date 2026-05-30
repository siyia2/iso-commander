// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

// Project Headers
#include "../state.h"
#include "../write2usb.h"

namespace fs = std::filesystem;

/**
 * @brief Runs an external command with stdout/stderr suppressed.
 *
 * Forks a child process, redirects its stdout and stderr to /dev/null,
 * and exec's the given argument vector. The parent waits for the child
 * and returns its exit code.
 *
 * @param args Argument vector where args[0] is the executable name.
 * @return Exit code of the child process, or -1 on fork/exec failure.
 */
int runCommand(const std::vector<std::string>& args) {
    std::vector<const char*> argv;
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devNull = open("/dev/null", O_WRONLY);
        dup2(devNull, STDOUT_FILENO);
        dup2(devNull, STDERR_FILENO);
        execvp(argv[0], const_cast<char* const*>(argv.data()));
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/**
 * @brief Detects whether an ISO image is Windows installation media.
 *
 * Loop-mounts the ISO read-only into a temporary directory and checks
 * for the co-presence of @c sources/boot.wim (Windows setup payload)
 * and @c bootmgr / @c bootmgr.efi (Windows boot manager). Both markers
 * must be present; either alone is insufficient.
 *
 * The temporary mount point is always unmounted and removed before the
 * function returns, regardless of outcome. All mount/umount output is
 * suppressed via @ref runCommand() to avoid cluttering the terminal
 * with spurious error messages.
 *
 * @param isoPath Absolute path to the ISO image.
 * @return @c true if the ISO appears to be Windows installation media.
 */
bool isWindowsIso(const std::string& isoPath) {
    char tmpDir[] = "/tmp/iso_probe_XXXXXX";
    if (!mkdtemp(tmpDir)) return false;

    auto cleanup = [&]() {
        runCommand({"umount", "-l", tmpDir});
        rmdir(tmpDir);
    };

    if (runCommand({"mount", "-o", "ro,loop", isoPath, tmpDir}) != 0) {
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
 * @brief Probes /proc/filesystems and system kernel release metadata to determine
 * the most modern and performant NTFS driver available on the host platform.
 *
 * Priority Tiering:
 * 1. "ntfs" via Kernel 7.1+ (NTFSPLUS — community driver based on revert of
 *    the original NTFS codebase, with expanded write support and performance
 *    improvements over the Paragon NTFS3 driver)
 * 2. "ntfs3" via Kernel 5.15+ (Paragon Native Driver)
 *
 * Note: The legacy read-only "ntfs" driver was removed in kernel 7.1. Any
 * kernel reporting "ntfs" in /proc/filesystems at 7.1+ is therefore the
 * NTFSPLUS-derived driver, not the legacy one. The legacy driver is never
 * returned; this function is used exclusively for write operations.
 */
std::string getBestNtfsDriver() {
    runCommand({"modprobe", "ntfs3"});
    runCommand({"modprobe", "ntfs"});

    struct utsname osInfo;
    int major = 0, minor = 0;
    bool haveVersion = (uname(&osInfo) == 0 &&
                        sscanf(osInfo.release, "%d.%d", &major, &minor) == 2);

    std::ifstream filesystems("/proc/filesystems");
    bool hasNtfs3 = false;
    bool hasNtfs  = false;

    if (filesystems.is_open()) {
        std::string line;
        while (std::getline(filesystems, line)) {
            if (line.find("ntfs3") != std::string::npos)
                hasNtfs3 = true;
            else if (line.find("ntfs") != std::string::npos)
                hasNtfs  = true;
        }
    }

    bool isModernNtfs = hasNtfs && haveVersion &&
                        (major > 7 || (major == 7 && minor >= 1));

    if (isModernNtfs) return "ntfs";   // NTFSPLUS — write-capable, modern 7.1+
    if (hasNtfs3)     return "ntfs3";  // Paragon — write-capable, 5.15+
    return {};                         // No write-capable driver found
}

/**
* @brief Writes a Windows ISO to a block device using a hybrid FAT32 + NTFS layout (UEFI only).
*
* Partition layout written to @p device:
*   GPT partition table.
*   Partition 1: FAT32 (1024 MiB, EFI System Partition) — receives:
*     - efi/
*     - boot/
*     - sources/boot.wim
*
*   This satisfies Windows UEFI boot requirements while remaining within
*   FAT32 limitations.
*
*   Partition 2: NTFS (remainder) — receives all remaining files, including
*   large files such as sources/install.wim that exceed FAT32's 4 GiB limit.
*   Formatted with 4 KiB clusters (@c -c 4096).
*
* This layout mirrors the approach used by modern Windows installation media:
* a small FAT32 ESP for firmware compatibility, and a larger NTFS partition
* for bulk data storage.
*
* The NTFS partition is mounted using the best available kernel NTFS driver,
* selected at runtime by @c getBestNtfsDriver(). On kernel 7.1+, the
* NTFSPLUS driver is preferred; on 5.15–7.0, the Paragon ntfs3 driver is
* used. The function will fail if neither driver is available.
*
* @par Copy strategy
* Files are copied using a single 4 MiB buffer allocated once (via
* @c posix_memalign, 4096-byte aligned) and reused across all files.
* The source file descriptor uses @c posix_fadvise(POSIX_FADV_SEQUENTIAL)
* to enable kernel read-ahead from the loop-mounted ISO.
*
* FAT32 destination fds are opened with @c O_DIRECT, bypassing the page
* cache entirely — writes go straight to the USB device with no kernel
* buffering. This eliminates post-cancel dirty-page writeback for the ESP.
* Short final-chunk reads are padded to the next 512-byte sector boundary
* (required by @c O_DIRECT) with zeroed bytes. After the loop,
* @c ftruncate(fd_out, fileSize) strips the padding so the on-disk file is
* byte-for-byte identical to the source — progress and @c writtenInFile
* always track @c bytes_read (real bytes), never the padded write length.
*
* NTFS destination fds use buffered I/O (kernel NTFS drivers do not support
* @c O_DIRECT). No periodic mid-copy sync is performed — @c fdatasync and
* @c sync_file_range over USB are too costly and caused stalls in testing.
* The kernel flushes all dirty pages at unmount, which is the final flush
* guarantee for both ntfs and ntfs3 drivers.
*
* @par Cancellation
* @c GlobalState::g_operationCancelled is checked before and during every
* file copy. On cancellation:
*   - Copying stops at the next chunk boundary
*   - @c ftruncate(fd_out, 0) is called on the current output fd to
*     invalidate its dirty pages before @c close(); combined with
*     @c POSIX_FADV_DONTNEED this asks the kernel to discard rather than
*     flush those pages. Honoured on a best-effort basis by kernel drivers.
*   - @c POSIX_FADV_DONTNEED followed by @c BLKFLSBUF is issued on both
*     partition block devices unconditionally before every unmount — on
*     cancellation this covers dirty pages from previously completed files
*     which are no longer reachable via any open fd
*   - All three mounts are lazily unmounted (@c umount -l)
*   - The function returns @c false without setting the failure flag
*/
bool writeWindowsIsoToDevice(const std::string& isoPath,
                                    const std::string& device,
                                    size_t             progressIndex) {
    auto fail = [&]() -> bool {
        progressData[progressIndex].failed.store(true);
        return false;
    };

    // ------------------------------------------------------------------ //
    // 1. Wipe + repartition: GPT, FAT32 ESP (~1 GiB) + NTFS (remainder) //
    // ------------------------------------------------------------------ //
    if (runCommand({"wipefs", "-a", device}) != 0) return fail();

    if (runCommand({"parted", "-s", device,
                    "mklabel", "gpt",
                    "mkpart", "ESP",  "fat32", "1MiB",    "1025MiB",
                    "mkpart", "DATA", "ntfs",  "1025MiB", "100%",
                    "set", "1", "esp",  "on",
                    "set", "1", "boot", "on"}) != 0) return fail();

    if (runCommand({"udevadm", "settle"}) != 0) return fail();

    auto derivePartition = [&](int n) -> std::string {
        std::string c1 = device + std::to_string(n);
        std::string c2 = device + "p" + std::to_string(n);
        for (int attempt = 0; attempt < 3; ++attempt) {
            if (fs::exists(c1)) return c1;
            if (fs::exists(c2)) return c2;
            sleep(1);
        }
        return {};
    };

    std::string fatPart  = derivePartition(1);
    std::string ntfsPart = derivePartition(2);
    if (fatPart.empty() || ntfsPart.empty()) return fail();

    if (runCommand({"mkfs.fat", "-F", "32", "-n", "WINBOOT", fatPart}) != 0)
        return fail();

    if (runCommand({"mkfs.ntfs", "-f", "-c", "4096", "-L", "WINDATA", ntfsPart}) != 0)
        return fail();

    if (GlobalState::g_operationCancelled.load()) return false;

    // ------------------------------------------------------------------ //
    // 2. Mount ISO, FAT32 partition, and NTFS partition                  //
    // ------------------------------------------------------------------ //
    char isoMnt[]  = "/tmp/win_iso_XXXXXX";
    char fatMnt[]  = "/tmp/win_fat_XXXXXX";
    char ntfsMnt[] = "/tmp/win_ntfs_XXXXXX";

    if (!mkdtemp(isoMnt) || !mkdtemp(fatMnt) || !mkdtemp(ntfsMnt))
        return fail();

    auto dropPageCache = [](const std::string& blockDev) {
        int fd = open(blockDev.c_str(), O_RDWR);
        if (fd >= 0) {
            // FADV_DONTNEED on the block device asks the kernel to drop
            // dirty pages for the entire device — more effective than
            // BLKFLSBUF alone for flushed-but-not-committed pages.
            posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
            ioctl(fd, BLKFLSBUF);
            close(fd);
        }
    };

    bool alreadyUnmounted = false;
    auto unmountAll = [&]() {
        if (alreadyUnmounted) return;
        alreadyUnmounted = true;

        // Always drop page cache before unmounting — even on clean exit
        // the block devices benefit from cache eviction before teardown.
        // On cancellation this is the primary mechanism for stopping
        // residual USB writes from previously completed files.
        dropPageCache(fatPart);
        dropPageCache(ntfsPart);

        runCommand({"umount", "-l", isoMnt});
        runCommand({"umount", "-l", fatMnt});
        runCommand({"umount", "-l", ntfsMnt});
        rmdir(isoMnt);
        rmdir(fatMnt);
        rmdir(ntfsMnt);
    };

    if (runCommand({"mount", "-o", "ro,loop", isoPath, isoMnt}) != 0) {
        unmountAll(); return fail();
    }
    if (runCommand({"mount", fatPart, fatMnt}) != 0) {
        unmountAll(); return fail();
    }
    // NTFS driver autodetection: newer ntfs is default, ntfs3 is fallback for older systems
    std::string ntfsDriver = getBestNtfsDriver();
    if (ntfsDriver.empty()) { unmountAll(); return fail(); }
    if (runCommand({"mount", "-t", ntfsDriver, ntfsPart, ntfsMnt}) != 0) {
        unmountAll(); return fail();
    }

    if (GlobalState::g_operationCancelled.load()) { unmountAll(); return false; }

    // ------------------------------------------------------------------ //
    // 3. Collect all ISO entries in a single pass, compute total bytes,  //
    //    and classify each entry (ESP vs NTFS) up front.                 //
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
        for (const auto& entry : fs::recursive_directory_iterator(std::string(isoMnt))) {
            fs::path rel   = fs::relative(entry.path(), std::string(isoMnt));
            bool     toESP = belongsOnESP(rel);
            fs::path dest  = fs::path(toESP ? std::string(fatMnt)
                                            : std::string(ntfsMnt)) / rel;

            uint64_t sz = entry.is_regular_file() ? entry.file_size() : 0;
            totalBytes += sz;

            IsoEntry e { entry.path(), dest, sz, entry.is_directory() };
            if (toESP) espEntries.push_back(std::move(e));
            else       ntfsEntries.push_back(std::move(e));
        }
    } catch (...) { unmountAll(); return fail(); }

    if (totalBytes == 0) { unmountAll(); return fail(); }

    // ------------------------------------------------------------------ //
    // 4. Copy with live progress                                         //
    //                                                                    //
    //    Key choices:                                                    //
    //    - Buffer allocated once (4 MiB, 4096-aligned) and reused.      //
    //    - FAT32: O_DIRECT bypasses page cache entirely — no post-cancel //
    //      dirty-page writeback on the ESP. Final short chunk is padded  //
    //      to the next 512-byte sector boundary then truncated back to   //
    //      exact file size after the loop so Secure Boot verification    //
    //      sees the correct bytes. Progress tracking uses bytes_read     //
    //      (real bytes), never the padded write length.                  //
    //    - NTFS: does not support O_DIRECT; buffered I/O is used.        //
    //      No periodic mid-copy sync — fdatasync/sync_file_range over    //
    //      USB caused stalls in testing. Dirty pages are flushed at      //
    //      unmount for both ntfs and ntfs3 drivers.                      //
    //    - On cancellation: ftruncate(fd_out, 0) + FADV_DONTNEED on the  //
    //      current fd, plus FADV_DONTNEED + BLKFLSBUF on both block      //
    //      devices via unmountAll, covers pages from completed files.    //
    //    - posix_fadvise(SEQUENTIAL) on each source fd for prefetch.     //
    // ------------------------------------------------------------------ //
    auto startTime  = std::chrono::high_resolution_clock::now();
    auto lastUpdate = startTime;
    uint64_t bytesInWindow = 0;
    constexpr int UPDATE_INTERVAL_MS = 300;

    auto updateProgress = [&](uint64_t justCopied) {
        progressData[progressIndex].bytesWritten.fetch_add(justCopied);
        bytesInWindow += justCopied;

        auto now = std::chrono::high_resolution_clock::now();
        auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - lastUpdate).count();
        if (ms >= UPDATE_INTERVAL_MS) {
            uint64_t written = progressData[progressIndex].bytesWritten.load();
            progressData[progressIndex].progress.store(
                static_cast<int>(
                    (static_cast<double>(written) / totalBytes) * 100));
            if (bytesInWindow > 0 && ms > 0) {
                progressData[progressIndex].speed.store(
                    (static_cast<double>(bytesInWindow) /
                     (1024.0 * 1024.0)) / (ms / 1000.0));
                bytesInWindow = 0;
            }
            lastUpdate = now;
        }
    };

    // Single reusable 4 MiB buffer — must be 4096-aligned for O_DIRECT
    // on FAT32. Allocated once and reused across all files.
    constexpr size_t BUF_SIZE = 4 * 1024 * 1024;

    void* rawBuf = nullptr;
    if (posix_memalign(&rawBuf, 4096, BUF_SIZE) != 0) { unmountAll(); return fail(); }
    auto ioBuf     = static_cast<char*>(rawBuf);
    auto freeIoBuf = [&]() { free(rawBuf); rawBuf = nullptr; ioBuf = nullptr; };

    auto copyWithProgress = [&](const fs::path& src, const fs::path& dst,
                                uint64_t fileSize) -> bool {
        const bool isNtfs = dst.string().rfind(ntfsMnt, 0) == 0;

        int fd_in = open(src.c_str(), O_RDONLY);
        if (fd_in < 0) return false;

        // Hint to the kernel: we will read this file sequentially.
        // The ISO is on the host filesystem, so prefetching is cheap and
        // effective — the kernel will read ahead into page cache while we
        // write to the USB device.
        posix_fadvise(fd_in, 0, 0, POSIX_FADV_SEQUENTIAL);

        // FAT32: O_DIRECT bypasses the page cache entirely — writes go
        // straight to the USB device with no kernel buffering, eliminating
        // post-cancel dirty-page writeback for the ESP.
        // NTFS: kernel NTFS drivers reject O_DIRECT; use buffered I/O.
        int outFlags = O_WRONLY | O_CREAT | O_TRUNC;
        if (!isNtfs) outFlags |= O_DIRECT;

        int fd_out = open(dst.c_str(), outFlags, 0644);
        if (fd_out < 0) { close(fd_in); return false; }

        bool     success       = true;

        while (!GlobalState::g_operationCancelled.load()) {
            ssize_t bytes_read = read(fd_in, ioBuf, BUF_SIZE);
            if (bytes_read < 0) {
                if (errno == EINTR) continue;
                success = false;
                break;
            }
            if (bytes_read == 0) break;  // EOF

            // O_DIRECT requires the write length to be a sector multiple.
            // Pad the final short chunk to the next 512-byte boundary with
            // zeros. Progress and writtenInFile always track bytes_read (the
            // real byte count) — never writeLen — so reported size and the
            // post-loop ftruncate both use the correct value.
            ssize_t writeLen = bytes_read;
            if (!isNtfs) {
                ssize_t aligned = (bytes_read + 511) & ~511;
                if (aligned > bytes_read)
                    memset(ioBuf + bytes_read, 0, aligned - bytes_read);
                writeLen = aligned;
            }

            ssize_t bytes_written = 0;
            while (bytes_written < writeLen) {
                if (GlobalState::g_operationCancelled.load()) {
                    success = false;
                    goto done;
                }
                ssize_t written = write(fd_out,
                                        ioBuf + bytes_written,
                                        writeLen - bytes_written);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    success = false;
                    goto done;
                }
                bytes_written += written;
            }

            // Track real bytes (not padded writeLen) for accurate progress
            // reporting and correct post-loop ftruncate on FAT32.
            updateProgress(static_cast<uint64_t>(bytes_read));
        }

    done:
        // Strip O_DIRECT sector-alignment padding written on the final chunk
        // so Secure Boot signature verification sees the exact source bytes.
        // Must happen before the cancellation ftruncate(0) which overwrites it.
        if (!isNtfs && success && fileSize > 0)
            ftruncate(fd_out, static_cast<off_t>(fileSize));

        if (GlobalState::g_operationCancelled.load()) {
            // Ask the kernel to discard dirty pages for this file rather than
            // flushing them to the USB device. ftruncate invalidates the pages;
            // FADV_DONTNEED reinforces that the cache can be dropped.
            // Honoured on a best-effort basis by kernel NTFS drivers.
            ftruncate(fd_out, 0);
            posix_fadvise(fd_out, 0, 0, POSIX_FADV_DONTNEED);
        }

        close(fd_in);
        close(fd_out);
        return success && !GlobalState::g_operationCancelled.load();
    };

    // ------------------------------------------------------------------ //
    // 5. Two-pass copy: NTFS data first, ESP bootloader second.          //
    //    Windows setup is sensitive to the ESP appearing complete; by    //
    //    writing all NTFS content first we ensure the bootloader only    //
    //    lands after the data it points to is already on the drive.      //
    // ------------------------------------------------------------------ //
    auto processEntries = [&](const std::vector<IsoEntry>& entries) -> bool {
        for (const auto& e : entries) {
            if (GlobalState::g_operationCancelled.load()) {
                freeIoBuf(); unmountAll(); return false;
            }
            if (e.isDir) {
                fs::create_directories(e.dst);
            } else {
                fs::create_directories(e.dst.parent_path());
                if (!copyWithProgress(e.src, e.dst, e.size)) {
                    freeIoBuf(); unmountAll(); return fail();
                }
            }
        }
        return true;
    };

    if (!processEntries(ntfsEntries)) return false;
    if (!processEntries(espEntries))  return false;

    freeIoBuf();

    // ------------------------------------------------------------------ //
    // 6. Flush and unmount                                               //
    //                                                                    //
    //    Note: USB mass storage devices may acknowledge fsync/flush      //
    //    commands before their internal write cache is fully committed   //
    //    to flash. There is no reliable userspace workaround for this.   //
    //    The umount calls below force the kernel to flush all dirty      //
    //    pages; that is the strongest guarantee we can give.             //
    // ------------------------------------------------------------------ //
    if (!GlobalState::g_operationCancelled.load()) {
        int diskFd = open(device.c_str(), O_RDWR);
        if (diskFd >= 0) {
            fsync(diskFd);
            close(diskFd);
        }
    }
    unmountAll();

    auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - startTime);
    double seconds  = totalElapsed.count() / 1000.0;
    double avgSpeed = seconds > 0.0
        ? (static_cast<double>(totalBytes) / (1024.0 * 1024.0)) / seconds
        : 0.0;
    progressData[progressIndex].speed.store(avgSpeed);
    progressData[progressIndex].progress.store(100);
    progressData[progressIndex].completed.store(true);
    return true;
}

/**
 * @brief Validates that a file descriptor refers to a valid ISO 9660 filesystem image.
 *
 * Reads the Primary Volume Descriptor at Sector 16 (byte offset 32768) using
 * positional I/O, leaving the file descriptor's offset cursor unmodified.
 *
 * Checks performed:
 *   - Descriptor type byte (buffer[0]) must equal 0x01 (Primary Volume Descriptor).
 *   - Standard Identifier bytes 1–5 must equal "CD001" (ECMA-119 §8.1).
 *
 * @param fd  An open, readable file descriptor referencing the target image.
 * @return    @c true if Sector 16 contains a valid ISO 9660 Primary Volume Descriptor;
 *            @c false if the read fails, the descriptor type is wrong, or the
 *            identifier does not match.
 */
bool isValidIso9660(int fd) {
    uint8_t buffer[2048];
    if (pread(fd, buffer, sizeof(buffer), 32768) != 2048) return false;

    // buffer[0]: Volume Descriptor Type — 0x01 = Primary Volume Descriptor (ECMA-119 §8.3).
    // buffer[1..5]: Standard Identifier — must be "CD001" (ECMA-119 §8.1).
    if (buffer[0] != 0x01) return false;

    std::string_view id(reinterpret_cast<char*>(buffer + 1), 5);
    return id == "CD001";
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
 * @note The static Aho-Corasick automaton is thread-safe under C++11 and later.
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
                    bytes[0] = c; bytes[1] = 0x00; // UTF-16LE: [char, 0x00]
                    byteCount = 2;
                } else {
                    bytes[0] = 0x00; bytes[1] = c; // UTF-16BE: [0x00, char]
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

    // --- Size check (fd already open, caller owns it) ---
    if (fileSize < 0x9000) return false;

    const size_t mapSize = std::min(static_cast<size_t>(fileSize),
                                    static_cast<size_t>(32 * 1024 * 1024));
    char* addr = static_cast<char*>(mmap(nullptr, mapSize, PROT_READ, MAP_PRIVATE, fd, 0));
    if (addr == MAP_FAILED) return false;
    madvise(addr, mapSize, MADV_SEQUENTIAL);

    // --- Volume header validation ---
    // Sector 16 (offset 0x8000): byte 0 = descriptor type, bytes 1–5 = identifier.
    // Accept ISO 9660 PVD (type=0x01, id="CD001") or UDF NSR (id="NSR02"/"NSR03").
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
    uint32_t found = 0;
    int cur = 0;
    for (size_t i = 0; i < mapSize; ++i) {
        const int c = static_cast<unsigned char>(
            std::tolower(static_cast<unsigned char>(addr[i])));
        cur = ac[cur].next[c];
        if (ac[cur].match)
            found |= ac[cur].match;
    }

    munmap(addr, mapSize);
    // fd is NOT closed — caller owns it

    // boot.wim  matched in any variant: bits 0,1,2
    // bootmgr   matched in any variant: bits 3–8
    const bool hasBootWim = (found & 0b000000111u) != 0;
    const bool hasBootMgr = (found & 0b111111000u) != 0;
    return hasBootWim && hasBootMgr;
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
 *   held for the duration of the call. The same descriptor is passed to @ref isValidIso9660
 *   and @ref isWindowsIsoInitialCheck, avoiding redundant opens. File size is retrieved
 *   once via @c fstat and reused across all paths.
 * - **Early Validation:** @ref isValidIso9660 is invoked before any device access or
 *   routing decision, ensuring invalid images are rejected with minimal overhead.
 * - **Windows Path Routing:** If @ref isWindowsIsoInitialCheck passes, execution is
 *   delegated to @ref writeWindowsIsoToDevice, which automatically handles multi-partitioning,
 *   hybrid FAT32+NTFS tables, and cluster tuning.
 * - **Raw O_DIRECT Pipeline:** Non-Windows targets are imaged via raw unbuffered disk I/O.
 *   Data transfers utilize a dedicated page-aligned memory buffer (@c posix_memalign) matching
 *   the physical device's underlying sector constraints queried from @c ioctl(BLKSSZGET).
 * - **Tail-Block Padding:** Detects partial blocks at EOF or short reads, automatically
 *   zero-padding the remaining buffer slice up to the strict sector layout boundary to prevent
 *   kernel rejection errors (@c EINVAL).
 * - **Asynchronous Cancellation Safety:** Evaluates @c GlobalState::g_operationCancelled
 *   at every inner loop pass. On user abort, it short-circuits execution and leaves the disk
 *   safely without triggering a cascading @c fsync block.
 *
 * @param isoPath Absolute path to the source ISO image file on the host.
 * @param device Target destination block node path (e.g., @c /dev/sdb). WARNING: All existing
 * underlying data will be destructively overwritten.
 * @param progressIndex Unique thread tracking identifier mapped inside the global progress array.
 * @return @c true if the image data was transferred successfully, verified, and safely flushed;
 * @c false on physical I/O failure, validation collapse, or user cancellation.
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

    // --- Raw O_DIRECT path for Linux / UEFI ISOs -------------------------

    // Open device with O_DIRECT for unbuffered writes
    int dev_fd = open(device.c_str(), O_WRONLY | O_DIRECT);
    if (dev_fd == -1) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    auto closeDev = [](int* fd) { if (*fd != -1) close(*fd); };
    std::unique_ptr<int, decltype(closeDev)> devGuard(&dev_fd, closeDev);

    // Get sector size (required for O_DIRECT alignment)
    int sectorSize = 0;
    if (ioctl(dev_fd, BLKSSZGET, &sectorSize) < 0 || sectorSize <= 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Use file size from fstat rather than reopening via std::filesystem
    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }

    // Pad file size up to sector boundary for O_DIRECT
    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    // Allocate aligned buffer (8 MiB, rounded down to sector boundary)
    constexpr size_t DESIRED_BUFFER = 8 * 1024 * 1024;
    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    char* alignedBuffer = nullptr;
    if (posix_memalign(reinterpret_cast<void**>(&alignedBuffer), sectorSize, bufferSize) != 0) {
        progressData[progressIndex].failed.store(true);
        return false;
    }
    std::unique_ptr<char, decltype(&free)> bufferGuard(alignedBuffer, &free);

    // Progress / speed tracking
    auto startTime  = std::chrono::high_resolution_clock::now();
    auto lastUpdate = startTime;
    uint64_t bytesInWindow = 0;
    constexpr int UPDATE_INTERVAL_MS = 300;

    auto updateSpeed = [&](auto now) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUpdate);
        if (bytesInWindow > 0 && elapsed.count() > 0) {
            double seconds = elapsed.count() / 1000.0;
            progressData[progressIndex].speed.store(
                (static_cast<double>(bytesInWindow) / (1024.0 * 1024.0)) / seconds);
            bytesInWindow = 0;
            lastUpdate = now;
        }
    };

    uint64_t localBytesWritten = 0;

    try {
        while (localBytesWritten < paddedSize && !GlobalState::g_operationCancelled.load()) {
            uint64_t remaining   = paddedSize - localBytesWritten;
            size_t   bytesToRead = static_cast<size_t>(std::min<uint64_t>(bufferSize, remaining));

            ssize_t bytesRead = 0;
            while (true) {
                bytesRead = read(iso_fd, alignedBuffer, bytesToRead);
                if (bytesRead >= 0) break;
                if (errno == EINTR) continue;
                throw std::runtime_error("Read error: " + std::string(strerror(errno)));
            }

            if (bytesRead == 0) {
                // EOF — remainder is padding; zero the full chunk
                std::memset(alignedBuffer, 0, bytesToRead);
            } else if (static_cast<size_t>(bytesRead) < bytesToRead) {
                // Short read on final chunk — zero-pad the tail
                std::memset(alignedBuffer + bytesRead, 0, bytesToRead - bytesRead);
            }

            // Write loop — handles partial writes and EINTR
            size_t remainingToWrite = bytesToRead;
            char*  writePtr         = alignedBuffer;
            while (remainingToWrite > 0) {
                ssize_t written = write(dev_fd, writePtr, remainingToWrite);
                if (written < 0) {
                    if (errno == EINTR) continue;
                    throw std::runtime_error("Write error: " + std::string(strerror(errno)));
                }

                remainingToWrite -= static_cast<size_t>(written);
                writePtr         += written;

                uint64_t reportable = std::min<uint64_t>(
                    static_cast<uint64_t>(written),
                    fileSize - std::min(localBytesWritten, fileSize));

                localBytesWritten += static_cast<size_t>(written);
                bytesInWindow     += static_cast<uint64_t>(written);   // physical throughput for speed
                progressData[progressIndex].bytesWritten.fetch_add(reportable); // capped for UI
            }

            // Throttled progress + speed update
            auto now           = std::chrono::high_resolution_clock::now();
            auto msSinceUpdate = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - lastUpdate).count();
            if (msSinceUpdate >= UPDATE_INTERVAL_MS) {
                uint64_t reported = progressData[progressIndex].bytesWritten.load();
                progressData[progressIndex].progress.store(
                    static_cast<int>((static_cast<double>(reported) / fileSize) * 100));
                updateSpeed(now);
            }
        }
    } catch (...) {
        if (!GlobalState::g_operationCancelled.load()) {
            progressData[progressIndex].failed.store(true);
        }
        return false;
    }

    if (!GlobalState::g_operationCancelled.load()) {
        if (fsync(dev_fd) != 0) {
            progressData[progressIndex].failed.store(true);
            return false;
        }
    }

    if (!GlobalState::g_operationCancelled.load() && localBytesWritten >= paddedSize) {
        auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - startTime);
        double seconds  = totalElapsed.count() / 1000.0;
        double avgSpeed = seconds > 0.0
            ? (static_cast<double>(fileSize) / (1024.0 * 1024.0)) / seconds
            : 0.0;
        progressData[progressIndex].speed.store(avgSpeed);
        progressData[progressIndex].progress.store(100);
        progressData[progressIndex].completed.store(true);
        return true;
    }

    return false;
}
