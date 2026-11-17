// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <stdint.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Third-Party Library Headers
#include <libmount/libmount.h>

// Project Headers
#include "../globalMutexes.h"
#include "../mount.h"
#include "../state.h"
#include "../stringManipulation.h"
#include "../verbose.h"

struct libmnt_context;

namespace fs = std::filesystem;

/**
 * @brief Provides default LSan suppression rules for known third-party leaks.
 *
 * Suppresses false-positive leak reports originating from @c libmount's
 * internal @c realpath() allocation inside @c mnt_context_prepare_mount(),
 * which is never freed by the library before LSan's end-of-process scan.
 * The leak is confined entirely to @c libmount.so and is not reachable or
 * fixable from user code.
 *
 * This function is a weak symbol recognised by LeakSanitizer at startup;
 * it has no effect in non-ASan builds.
 *
 * @return A newline-separated list of LSan suppression patterns.
 */
extern "C" const char* __lsan_default_suppressions() {
    return "leak:mnt_context_prepare_mount\n";
}

// ---------------------------------------------------------------------------
// Image detection
// ---------------------------------------------------------------------------

/// Filesystem family that should be used to mount a detected image.
enum class ImageKind {
    None,         ///< Not recognised.
    Iso9660Udf,   ///< ISO 9660 / UDF (kernel autodetects between "udf" and "iso9660").
    HfsPlus,      ///< HFS+ / HFSX (kernel "hfsplus" driver).
    Hfs           ///< Classic HFS (kernel "hfs" driver), e.g. Mac OS 8.0 and earlier media.
};

/**
 * @brief Result of image detection.
 *
 * For images where the filesystem does not start at byte 0 (Apple Partition Map or
 * GUID Partition Table layouts), @c offset and @c size describe the partition so the
 * loop device can be set up with @c offset= / @c sizelimit=.
 */
struct ImageInfo {
    ImageKind kind   = ImageKind::None;
    off_t     offset = 0;  ///< Byte offset of the filesystem inside the image file.
    off_t     size   = 0;  ///< Byte length of the filesystem; 0 means "until end of file".
};

namespace {

/// Bounds-checked pread() wrapper over an open image file.
class ImageReader {
public:
    ImageReader(int fd, off_t fileSize) : fd_(fd), size_(fileSize) {}

    bool read(off_t offset, void* dst, std::size_t n) const {
        if (offset < 0 || n == 0) return false;
        if (offset > size_ || static_cast<off_t>(n) > size_ - offset) return false;
        return pread(fd_, dst, n, offset) == static_cast<ssize_t>(n);
    }

    off_t fileSize() const { return size_; }

private:
    int   fd_;
    off_t size_;
};

uint32_t be32(const unsigned char* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

uint32_t le32(const unsigned char* p) {
    return (uint32_t(p[3]) << 24) | (uint32_t(p[2]) << 16) |
           (uint32_t(p[1]) << 8)  |  uint32_t(p[0]);
}

uint64_t le64(const unsigned char* p) {
    return (uint64_t(le32(p + 4)) << 32) | le32(p);
}

/**
 * @brief Probes for an HFS or HFS+/HFSX volume starting at @p base.
 *
 * - HFS+ / HFSX: volume header at base + 1024, signature "H+" (v4) or "HX" (v5).
 * - HFS wrapper (Mac OS 8.1-9 era): a classic HFS Master Directory Block at base + 1024
 *   ("BD") whose embedded-volume signature (MDB offset 124) is "H+" or "HX". Plain HFS
 *   volumes without an embedded HFS+ volume are reported as Hfs. The kernel's hfsplus driver
 *   locates the embedded volume itself, so the wrapper is reported as HfsPlus with no
 *   extra offset.
 */
ImageKind probeVolumeAt(const ImageReader& rd, off_t base) {
    unsigned char sig[4];

    if (rd.read(base + 1024, sig, 2) &&
        (std::memcmp(sig, "H+", 2) == 0 || std::memcmp(sig, "HX", 2) == 0))
        return ImageKind::HfsPlus;

    // HFS wrapper around an embedded HFS+ volume.
    unsigned char embedSig[2];
    if (rd.read(base + 1024, sig, 2) && std::memcmp(sig, "BD", 2) == 0 &&
        rd.read(base + 1024 + 124, embedSig, 2) &&
        (std::memcmp(embedSig, "H+", 2) == 0 || std::memcmp(embedSig, "HX", 2) == 0))
        return ImageKind::HfsPlus;

    // Plain classic HFS: MDB signature "BD" with no embedded HFS+ volume.
    if (rd.read(base + 1024, sig, 2) && std::memcmp(sig, "BD", 2) == 0)
        return ImageKind::Hfs;

    return ImageKind::None;
}

/// Clamps a partition size so a truncated image never yields a sizelimit past EOF.
off_t clampedSize(const ImageReader& rd, off_t base, uint64_t bytes) {
    const uint64_t maxBytes = static_cast<uint64_t>(rd.fileSize() - base);
    return (bytes == 0 || bytes > maxBytes) ? 0 : static_cast<off_t>(bytes);
}

/**
 * @brief Walks an Apple Partition Map and returns the first HFS/HFS+ partition.
 *
 * Layout: Driver Descriptor Record ("ER") at block 0 holding the block size, followed
 * by "PM" partition entries starting at block 1. Every partition is probed by content,
 * not by its type string, so mislabelled partitions still work.
 */
bool scanApm(const ImageReader& rd, uint32_t bs, ImageInfo& out) {
    uint32_t mapEntries = 1;
    for (uint32_t i = 1; i <= mapEntries && i <= 128; ++i) {
        unsigned char e[80];
        if (!rd.read(off_t(i) * bs, e, sizeof(e)) || e[0] != 'P' || e[1] != 'M') break;

        if (i == 1) mapEntries = be32(e + 4);

        const uint64_t start = be32(e + 8);
        const uint64_t count = be32(e + 12);
        const uint64_t base  = start * bs;
        if (base >= static_cast<uint64_t>(rd.fileSize())) continue;

        const ImageKind kind = probeVolumeAt(rd, static_cast<off_t>(base));
        if (kind != ImageKind::None) {
            out.kind   = kind;
            out.offset = static_cast<off_t>(base);
            out.size   = clampedSize(rd, out.offset, count * bs);
            return true;
        }
    }
    return false;
}

/**
 * @brief Finds an HFS/HFS+ partition in an Apple Partition Map.
 *
 * Tries the block size declared in the driver descriptor first, then 512 and 2048,
 * because old CD images (Mac OS 8 era) don't always declare the block size that the
 * partition map entries are actually laid out with.
 */
bool findInApm(const ImageReader& rd, ImageInfo& out) {
    unsigned char ddr[4];
    if (!rd.read(0, ddr, 4) || ddr[0] != 'E' || ddr[1] != 'R') return false;

    const uint32_t declared = (uint32_t(ddr[2]) << 8) | ddr[3];
    for (const uint32_t bs : {declared, 512u, 2048u}) {
        if (bs != 512 && bs != 1024 && bs != 2048 && bs != 4096) continue;
        if (scanApm(rd, bs, out)) return true;
    }
    return false;
}

/**
 * @brief Walks a GUID Partition Table and returns the first HFS/HFS+ partition.
 *
 * Modern macOS installer media (and anything created with a GPT layout) uses this.
 * Sector sizes 512 and 4096 are both tried. Partitions are probed by content, which
 * covers Apple HFS+ (48465300-...) and mislabelled entries alike.
 */
bool findInGpt(const ImageReader& rd, ImageInfo& out) {
    for (const uint32_t ss : {512u, 4096u}) {
        unsigned char hdr[92];
        if (!rd.read(ss, hdr, sizeof(hdr)) || std::memcmp(hdr, "EFI PART", 8) != 0)
            continue;

        const uint64_t entriesLba = le64(hdr + 72);
        const uint32_t numEntries = le32(hdr + 80);
        const uint32_t entrySize  = le32(hdr + 84);
        if (entrySize < 48 || entrySize > 4096 || numEntries == 0) continue;

        const uint32_t limit = numEntries < 256 ? numEntries : 256;
        for (uint32_t i = 0; i < limit; ++i) {
            const uint64_t entryOff = entriesLba * ss + uint64_t(i) * entrySize;
            if (entryOff > static_cast<uint64_t>(rd.fileSize())) break;

            unsigned char e[48];
            if (!rd.read(static_cast<off_t>(entryOff), e, sizeof(e))) break;

            // Unused entry: all-zero type GUID.
            bool empty = true;
            for (int b = 0; b < 16; ++b)
                if (e[b]) { empty = false; break; }
            if (empty) continue;

            const uint64_t first = le64(e + 32);
            const uint64_t last  = le64(e + 40);
            if (last < first) continue;

            const uint64_t base = first * ss;
            if (base >= static_cast<uint64_t>(rd.fileSize())) continue;

            const ImageKind kind = probeVolumeAt(rd, static_cast<off_t>(base));
            if (kind != ImageKind::None) {
                out.kind   = kind;
                out.offset = static_cast<off_t>(base);
                out.size   = clampedSize(rd, out.offset, (last - first + 1) * ss);
                return true;
            }
        }
    }
    return false;
}

} // namespace

/**
 * @brief Heuristic detection of mountable disk images.
 *
 * Recognised, in order:
 *  1. ISO 9660 Primary Volume Descriptor ("CD001" at sector 16). Hybrid ISO9660+HFS
 *     discs are mounted through their ISO 9660 side.
 *  2. UDF Volume Recognition Sequence ("BEA01", "NSR02", "NSR03", "TEA01").
 *  3. Bare HFS, HFS+/HFSX (or HFS-wrapped HFS+) at offset 0.
 *  4. Apple Partition Map containing HFS/HFS+ (classic hdiutil "SPUD" layout).
 *  5. GUID Partition Table containing HFS+ (modern macOS installer images).
 *
 * Validation is signature-based only; it does not parse the filesystem.
 *
 * @param path Filesystem path of the image.
 * @param st   stat() of the file (used for size gating).
 * @return ImageInfo with @c kind == ImageKind::None if nothing was recognised.
 */
static ImageInfo detectImage(const std::string& path, const struct stat& st) {
    ImageInfo info;
    if (st.st_size < 2048) return info;

    const int fd = open(path.c_str(), O_RDONLY | O_NOATIME | O_CLOEXEC);
    // O_NOATIME silently has no effect if the caller doesn't own the file;
    // open() still succeeds, so this is safe for a read-only validator.
    if (fd < 0) return info;

    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) {
                posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
                close(fd);
            }
        }
    } fdGuard{fd};

    posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    const ImageReader rd(fd, st.st_size);

    unsigned char sig[6]{};

    // --- 1. ISO 9660 Primary Volume Descriptor at sector 16 (ECMA-119 §8.1) ---
    if (rd.read(32768, sig, 6) && sig[0] == 0x01 && std::memcmp(sig + 1, "CD001", 5) == 0) {
        info.kind = ImageKind::Iso9660Udf;
        return info;
    }

    // --- 2. UDF Volume Recognition Sequence (ECMA-167 §2/3) ---
    // Up to 6 descriptors at 2048- and 4096-byte strides, identifier at byte 1.
    for (const off_t stride : {off_t{2048}, off_t{4096}}) {
        for (int i = 0; i < 6; ++i) {
            if (!rd.read(32768 + i * stride + 1, sig, 5)) break;
            if (!std::memcmp(sig, "BEA01", 5) || !std::memcmp(sig, "NSR02", 5) ||
                !std::memcmp(sig, "NSR03", 5) || !std::memcmp(sig, "TEA01", 5)) {
                info.kind = ImageKind::Iso9660Udf;
                return info;
            }
        }
    }

    // --- 3. Bare HFS / HFS+ / HFSX ---
    if (const ImageKind k = probeVolumeAt(rd, 0); k != ImageKind::None) {
        info.kind = k;
        return info;
    }

    // --- 4. Apple Partition Map, then 5. GUID Partition Table ---
    if (findInApm(rd, info)) return info;
    if (findInGpt(rd, info)) return info;

    return ImageInfo{};
}

extern char** environ;

/**
 * @brief Returns true if the kernel currently has filesystem type @p name registered.
 *
 * Parses /proc/filesystems, where each line is either "<TAB>name" or "nodev<TAB>name".
 */
static bool isFilesystemRegistered(std::string_view name) {
    std::ifstream f("/proc/filesystems");
    std::string line;
    while (std::getline(f, line)) {
        const std::string_view sv(line);
        const auto pos = sv.find_last_of(" \t");
        const std::string_view fsName =
            (pos == std::string_view::npos) ? sv : sv.substr(pos + 1);
        if (fsName == name) return true;
    }
    return false;
}

/**
 * @brief Best-effort load of the kernel module providing filesystem @p name.
 *
 * Does nothing if the filesystem is already registered. Otherwise runs
 * `modprobe -q <name>` (absolute path, so a user-controlled PATH is never consulted,
 * with all stdio redirected to /dev/null) and re-checks /proc/filesystems.
 * Uses posix_spawn rather than system()/fork() so it is safe in a multi-threaded process.
 *
 * @return true if the filesystem is registered after the call.
 */
static bool ensureFilesystemModule(const char* name) {
    if (isFilesystemRegistered(name)) return true;

    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) return false;
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO,  "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    char* const argv[] = {
        const_cast<char*>("modprobe"), const_cast<char*>("-q"),
        const_cast<char*>(name), nullptr
    };

    for (const char* exe : {"/usr/sbin/modprobe", "/sbin/modprobe"}) {
        pid_t pid = 0;
        if (posix_spawn(&pid, exe, &actions, nullptr, argv, environ) != 0)
            continue; // binary not at this path, try the next one

        int status = 0;
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) break;
        }
        break;
    }

    posix_spawn_file_actions_destroy(&actions);
    return isFilesystemRegistered(name);
}

/**
 * @brief Scans /proc/mounts and returns the set of currently active mount targets.
 */
static std::unordered_set<std::string> buildMountPointCache() {
    std::unordered_set<std::string> mounted;
    struct libmnt_table* tb = mnt_new_table_from_file("/proc/mounts");
    if (!tb) return mounted;

    struct libmnt_iter* itr = mnt_new_iter(MNT_ITER_FORWARD);
    struct libmnt_fs*   fs  = nullptr;

    while (mnt_table_next_fs(tb, itr, &fs) == 0) {
        if (const char* target = mnt_fs_get_target(fs))
            mounted.emplace(target);
    }

    mnt_free_iter(itr);
    mnt_unref_table(tb);
    return mounted;
}

/**
 * @brief Computes a deterministic 5-character base-36 mount point suffix from a path.
 *
 * Uses FNV-1a for a stable, platform-independent hash (unlike std::hash).
 */
static std::string mountPointSuffix(const std::string& isoPath) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : isoPath) {
        h ^= c;
        h *= 1099511628211ULL;
    }

    static constexpr std::string_view kBase36 =
        "0123456789abcdefghijklmnopqrstuvwxyz";

    char out[6] = {};
    for (int i = 0; i < 5; ++i) {
        out[i] = kBase36[h % 36];
        h /= 36;
    }
    return out;
}

// ---------------------------------------------------------------------------
// mountIsoFiles helpers
// ---------------------------------------------------------------------------

namespace {

/// Frees a libmnt_context on scope exit.
struct CtxGuard {
    libmnt_context* c;
    ~CtxGuard() { mnt_free_context(c); }
};

/// Frees a libmnt_table on scope exit.
struct TableGuard {
    libmnt_table* t;
    ~TableGuard() { if (t) mnt_free_table(t); }
};

/// Frees a libmnt_iter on scope exit.
struct IterGuard {
    libmnt_iter* i;
    ~IterGuard() { if (i) mnt_free_iter(i); }
};

/// Packs (st_dev, st_ino) into a single uint64_t key so the cache is correct
/// across multiple filesystems (inodes are only unique per device).
uint64_t makeInodeKey(const struct stat& st) {
    return (static_cast<uint64_t>(st.st_dev) << 32) | st.st_ino;
}

/**
 * @brief Resolves a loop device source (e.g. /dev/loop0) to the inode key of its
 * backing file, via /sys/block/loopN/loop/backing_file.
 *
 * stat() on the backing path gives the stable inode regardless of any renames that
 * happened since the mount. Returns false if @p src is not a loop device or the
 * backing file cannot be resolved.
 */
bool loopBackingInodeKey(const char* src, uint64_t& key) {
    unsigned int loopNum = 0;
    if (std::sscanf(src, "/dev/loop%u", &loopNum) != 1) return false;

    char backingPath[256];
    const int written = snprintf(backingPath, sizeof(backingPath),
                                 "/sys/block/loop%u/loop/backing_file", loopNum);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(backingPath))
        return false; // Path too long, skip this entry

    std::ifstream f(backingPath);
    if (!f.is_open()) return false;

    std::string realPath;
    if (!std::getline(f, realPath) || realPath.empty()) return false;

    while (!realPath.empty() && (realPath.back() == '\n' || realPath.back() == '\r'))
        realPath.pop_back();

    struct stat backingSt{};
    if (::stat(realPath.c_str(), &backingSt) != 0) return false;

    key = makeInodeKey(backingSt);
    return true;
}

/**
 * @brief Inode key of the file behind a mount source.
 *
 * Block devices are resolved through sysfs when they are loop devices; any other
 * source is a direct file mount and is stat()ed normally.
 */
bool mountSourceInodeKey(const char* src, uint64_t& key) {
    struct stat st{};
    if (::stat(src, &st) != 0) return false;

    if (S_ISBLK(st.st_mode))
        return loopBackingInodeKey(src, key);

    key = makeInodeKey(st);
    return true;
}

/**
 * @brief Builds a cache of already-mounted source inodes so that a renamed ISO
 * (same inode, different path) is recognised as already mounted.
 *
 * mnt_fs_get_source() returns the loop device (e.g. /dev/loop0) for ISO mounts,
 * not the backing file path, so it is resolved through sysfs.
 */
std::unordered_set<uint64_t> buildMountedInodeCache() {
    std::unordered_set<uint64_t> inodes;
    libmnt_table* tbl = mnt_new_table_from_file("/proc/self/mountinfo");
    if (!tbl) return inodes;
    TableGuard tableGuard{tbl};

    libmnt_iter* itr = mnt_new_iter(MNT_ITER_FORWARD);
    if (!itr) return inodes;
    IterGuard iterGuard{itr};

    libmnt_fs* fs = nullptr;
    while (mnt_table_next_fs(tbl, itr, &fs) == 0) {
        const char* src = mnt_fs_get_source(fs);
        if (!src) continue;

        uint64_t key = 0;
        if (mountSourceInodeKey(src, key))
            inodes.insert(key);
    }
    return inodes;
}

/// Records "needsRoot" for every file and counts them all as failed.
void failAllNeedsRoot(const std::vector<std::string>& isoFiles,
                      std::atomic<size_t>* failedTasks,
                      bool silentMode) {
    if (!silentMode) {
        VerbosityFormatter formatter;
        std::vector<std::string> fails;
        fails.reserve(isoFiles.size());
        for (const auto& isoFile : isoFiles) {
            auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
            fails.push_back(formatter.formatError(std::string(dir), std::string(file), "needsRoot"));
        }
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
        verboseSets.operationFailed.insert(fails.begin(), fails.end());
    }
    failedTasks->fetch_add(isoFiles.size(), std::memory_order_relaxed);
}

void reportContextFailure(bool silentMode) {
    if (silentMode) return;
    std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
    verboseSets.operationFailed.insert("\033[1;91mFailed to create mount context.\033[0m");
}

/// Kernel filesystem type needed for an image, and the error code to report if
/// the kernel has no such driver. Both null for ISO 9660 / UDF (autodetected).
struct FsDriver {
    const char* fsType      = nullptr;
    const char* missingCode = nullptr;
};

/// Makes sure the kernel module for @p kind is loaded (once per process per driver).
FsDriver prepareDriver(ImageKind kind) {
    switch (kind) {
        case ImageKind::HfsPlus: {
            static const bool hfsModuleReady = ensureFilesystemModule("hfsplus");
            (void)hfsModuleReady;
            return {"hfsplus", "needsHFSPlus"};
        }
        case ImageKind::Hfs: {
            static const bool hfsClassicModuleReady = ensureFilesystemModule("hfs");
            (void)hfsClassicModuleReady;
            return {"hfs", "needsHFS"};
        }
        default:
            return {};
    }
}

/// Loop options: partitioned Apple images need offset/sizelimit so the kernel
/// driver sees only the partition, not the partition table.
std::string buildMountOptions(const ImageInfo& imgInfo) {
    std::string opts = "loop,ro";
    if (imgInfo.offset > 0)
        opts += ",offset=" + std::to_string(static_cast<long long>(imgInfo.offset));
    if (imgInfo.size > 0)
        opts += ",sizelimit=" + std::to_string(static_cast<long long>(imgInfo.size));
    return opts;
}

/// Creates the mount directory. Returns false if it cannot be used.
bool prepareMountPointDir(const std::string& mountPoint) {
    std::error_code ec;
    fs::create_directories(mountPoint, ec);

    if (ec && ec != std::errc::file_exists) return false;
    return !(fs::exists(mountPoint) && !fs::is_directory(mountPoint));
}

/// "/mnt/ISOs/<stem>~<suffix>"
std::string buildMountPoint(const std::string& isoFile) {
    std::string mountPoint;
    mountPoint.reserve(256); // Typical path length
    mountPoint = "/mnt/ISOs/";
    mountPoint += fs::path(isoFile).stem().string();
    mountPoint += "~";
    mountPoint += mountPointSuffix(isoFile);
    return mountPoint;
}

/**
 * @brief State of one mountIsoFiles() batch: the libmount context, the dedup
 * caches, and the buffered verbose results (flushed to verboseSets in batches to
 * limit contention on GlobalMutexes::globalSetsMutex).
 */
class MountBatch {
public:
    MountBatch(libmnt_context* context,
               std::atomic<size_t>* completed,
               std::atomic<size_t>* failed,
               bool silent)
        : ctx_(context),
          completedTasks_(completed),
          failedTasks_(failed),
          silentMode_(silent),
          mountPointCache_(buildMountPointCache()),
          mountedInodes_(buildMountedInodeCache()) {
        if (!silentMode_) {
            tempCompleted_.reserve(kBatchSize);
            tempSkipped_.reserve(kBatchSize);
            tempFailed_.reserve(kBatchSize);
        }
    }

    /// Mounts one image (or records why it was skipped / failed).
    void process(const std::string& isoFile) {
        struct stat isoStat{};
        ImageInfo imgInfo;
        if (const char* err = precheck(isoFile, isoStat, imgInfo)) {
            recordFail(isoFile, err);
            return;
        }

        const std::string mountPoint = buildMountPoint(isoFile);

        if (isAlreadyMounted(mountPoint, isoStat)) {
            recordSkip(isoFile, mountPoint);
            return;
        }

        if (!prepareMountPointDir(mountPoint)) {
            recordFail(isoFile, "mkdirFailed");
            return;
        }

        const FsDriver driver = configureMount(isoFile, mountPoint, imgInfo);

        if (mnt_context_mount(ctx_) == 0)
            onMountSuccess(isoFile, mountPoint, isoStat);
        else
            onMountFailure(isoFile, mountPoint, driver);
    }

    void maybeFlush() {
        if (!silentMode_ &&
            (tempCompleted_.size() >= kBatchSize ||
             tempSkipped_.size()   >= kBatchSize ||
             tempFailed_.size()    >= kBatchSize)) {
            flush();
        }
    }

    void flush() {
        if (silentMode_) return;
        std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
        verboseSets.operationCompleted.insert(tempCompleted_.begin(), tempCompleted_.end());
        verboseSets.operationSkipped.insert(tempSkipped_.begin(), tempSkipped_.end());
        verboseSets.operationFailed.insert(tempFailed_.begin(), tempFailed_.end());
        tempCompleted_.clear();
        tempSkipped_.clear();
        tempFailed_.clear();
    }

private:
    static constexpr size_t kBatchSize = 50;

    // Cancellation, existence (single stat() also feeds the inode cache),
    // regular-file check and image detection. Returns an error code, or nullptr.
    const char* precheck(const std::string& isoFile, struct stat& isoStat, ImageInfo& imgInfo) const {
        if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) return "cxl";
        if (::stat(isoFile.c_str(), &isoStat) != 0) return "missingISO";
        if (!S_ISREG(isoStat.st_mode)) return "badFS";

        imgInfo = detectImage(isoFile, isoStat);
        if (imgInfo.kind == ImageKind::None) return "badFS";
        return nullptr;
    }

    // Path-based skip (file was not renamed), or inode-based skip (file was
    // renamed after being mounted: the kernel tracks loop backing files by
    // inode, so the original mount stays intact under a stale name-derived path).
    bool isAlreadyMounted(const std::string& mountPoint, const struct stat& isoStat) const {
        return mountPointCache_.count(mountPoint) ||
               mountedInodes_.count(makeInodeKey(isoStat));
    }

    void recordFail(const std::string& isoFile, const char* reason) {
        if (!silentMode_) {
            auto [dir, file] = extractDirectoryAndFilename(isoFile, "mount");
            tempFailed_.push_back(formatter_.formatError(std::string(dir), std::string(file), reason));
        }
        failedTasks_->fetch_add(1, std::memory_order_relaxed);
    }

    void recordSkip(const std::string& isoFile, const std::string& mountPoint) {
        if (!silentMode_) {
            auto [mntDir, mntName] = extractDirectoryAndFilename(mountPoint, "mount");
            auto [isoDir, isoName] = extractDirectoryAndFilename(isoFile, "mount");
            tempSkipped_.push_back(
                formatter_.formatSkipped(std::string(isoDir), std::string(isoName),
                                         std::string(mntDir), std::string(mntName)));
        }
        completedTasks_->fetch_add(1, std::memory_order_relaxed);
    }

    // Sets up the libmount context for this image and returns the driver info.
    FsDriver configureMount(const std::string& isoFile, const std::string& mountPoint,
                            const ImageInfo& imgInfo) {
        const std::string mountOpts = buildMountOptions(imgInfo);

        mnt_reset_context(ctx_);
        mnt_context_set_source(ctx_, isoFile.c_str());
        mnt_context_set_target(ctx_, mountPoint.c_str());
        mnt_context_set_options(ctx_, mountOpts.c_str());

        const FsDriver driver = prepareDriver(imgInfo.kind);
        if (driver.fsType) {
            mnt_context_set_fstype(ctx_, driver.fsType);
        } else {
            // Clear any fstype left over from a previous HFS/HFS+ iteration.
            mnt_context_set_fstype(ctx_, nullptr);
            mnt_context_set_fstype_pattern(ctx_, "udf,iso9660");
        }
        return driver;
    }

    void onMountSuccess(const std::string& isoFile, const std::string& mountPoint,
                        const struct stat& isoStat) {
        if (!silentMode_) {
            auto [mntDir, mntName] = extractDirectoryAndFilename(mountPoint, "mount");
            auto [isoDir, isoName] = extractDirectoryAndFilename(isoFile, "mount");
            const char* rawFsType = mnt_context_get_fstype(ctx_);
            const std::string fsType = rawFsType ? rawFsType : "unknown";
            tempCompleted_.push_back(
                formatter_.formatMountSuccess(std::string(isoDir), std::string(isoName),
                                              std::string(mntDir), std::string(mntName), fsType));
        }
        mountPointCache_.emplace(mountPoint);
        // Keep the inode cache consistent so subsequent entries in the same
        // batch that share the same file are also skipped correctly.
        mountedInodes_.insert(makeInodeKey(isoStat));
        completedTasks_->fetch_add(1, std::memory_order_relaxed);
    }

    void onMountFailure(const std::string& isoFile, const std::string& mountPoint,
                        const FsDriver& driver) {
        // Tell "kernel has no driver for this filesystem" apart from "bad image".
        // Checked only on failure, so the success path pays nothing for it.
        const bool driverMissing = driver.fsType && !isFilesystemRegistered(driver.fsType);
        recordFail(isoFile, driverMissing ? driver.missingCode : "badFS");

        // Only remove the directory we just created if it is empty;
        // a concurrent mount may have legitimately used it.
        std::error_code ec;
        if (fs::is_directory(mountPoint) && fs::is_empty(mountPoint))
            fs::remove(mountPoint, ec);
    }

    libmnt_context*                ctx_;
    std::atomic<size_t>*           completedTasks_;
    std::atomic<size_t>*           failedTasks_;
    const bool                     silentMode_;
    VerbosityFormatter             formatter_;
    std::unordered_set<std::string> mountPointCache_;
    std::unordered_set<uint64_t>   mountedInodes_;
    std::vector<std::string>       tempCompleted_;
    std::vector<std::string>       tempSkipped_;
    std::vector<std::string>       tempFailed_;
};

} // namespace

/**
 * @brief Mounts a batch of disk images to unique directories under /mnt using loop devices.
 *
 * Supported images: ISO 9660, UDF, and Apple images (HFS or HFS+/HFSX, either bare or
 * inside an Apple Partition Map / GPT layout, as used by macOS installer media).
 * Multi-stage validation (root privileges, cancellation state, file existence, and
 * filesystem format) minimises unnecessary I/O.
 *
 * ### Integrity & Performance Features:
 * - **Resource Efficiency:** Uses a single `libmnt_context` per batch, reset via `mnt_reset_context`
 * to avoid repeated allocation overhead.
 * - **Intelligent Deduplication:**
 * 1. **Path-based:** Filters via `mountPointCache` for quick lookups.
 * 2. **Inode-based:** Inspects `/sys/block/loopN/loop/backing_file` to detect files that have
 * been renamed or hard-linked but are already active, ensuring idempotency.
 * - **Concurrency Strategy:** Results are buffered in thread-local vectors and flushed to
 * `globalSets` periodically to minimize lock contention on `GlobalMutexes::globalSetsMutex`.
 *
 * ### Mount Path Schema:
 * `/mnt/ISOs/<stem>~<5-char base-36 FNV-1a suffix>`
 *
 * ### Partitioned Apple images:
 * When the filesystem lives inside a partition, the loop device is configured with
 * `offset=` and `sizelimit=` so the kernel sees only that partition.
 *
 * ### Kernel requirements:
 * - HFS+ needs the `hfsplus` module.
 * - Classic HFS (Mac OS 8.0 and earlier) needs the `hfs` module.
 * - APFS is not supported: mainline Linux has no APFS driver.
 *
 * @param isoFiles      Vector of absolute paths to the image files.
 * @param completedTasks Atomic counter for successful mounts and skipped duplicates.
 * @param failedTasks   Atomic counter for any failures (e.g., missing root, I/O errors).
 * @param silentMode    If true, suppresses all log generation; only updates atomics.
 *
 * @warning Requires root privileges (geteuid() == 0). If invoked without root, all
 * operations will immediately fail with a "needsRoot" error.
 * @note The inode cache is updated dynamically during the loop, allowing the function to
 * properly handle batches containing duplicate files.
 */
void mountIsoFiles(
    const std::vector<std::string>& isoFiles,
    std::atomic<size_t>* completedTasks,
    std::atomic<size_t>* failedTasks,
    bool silentMode)
{
    if (geteuid() != 0) {
        failAllNeedsRoot(isoFiles, failedTasks, silentMode);
        return;
    }

    libmnt_context* ctx = mnt_new_context();
    if (!ctx) {
        reportContextFailure(silentMode);
        return;
    }
    CtxGuard ctxGuard{ctx};

    // Ensure the base ISOs directory exists once, up front, so we don't
    // redundantly re-check/re-create it on every iteration of the loop below.
    {
        std::error_code baseEc;
        fs::create_directories("/mnt/ISOs", baseEc);
    }

    MountBatch batch(ctx, completedTasks, failedTasks, silentMode);

    for (const auto& isoFile : isoFiles) {
        batch.process(isoFile);
        batch.maybeFlush();
    }
    batch.flush();
}
