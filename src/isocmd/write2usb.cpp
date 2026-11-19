// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// C / System Headers
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Project Headers
#include "../state.h"
#include "../write2usbUI.h"
#include "../write2usb.h"

namespace {

//-------------------------------------------------------------
//    1. ISO detection
//-------------------------------------------------------------

//---- ISO 9660 / UDF volume recognition ----------------------
namespace iso_layout {

constexpr size_t  kSectorSize  = 2048;
constexpr size_t  kVrsStart    = 16 * kSectorSize;   // 0x8000: first volume descriptor
constexpr int     kVrsSectors  = 8;                  // sectors 16-23 are examined
constexpr size_t  kIdOffset    = 1;                  // identifier follows the type byte
constexpr size_t  kIdLength    = 5;
constexpr size_t  kHeaderSize  = kIdOffset + kIdLength;
constexpr uint8_t kTypePrimary = 0x01;               // Primary Volume Descriptor

/**
 * @brief Tells whether a volume descriptor identifies a usable disc image.
 *
 * Accepts an ISO 9660 Primary Volume Descriptor (type 0x01, "CD001"), which
 * also covers UDF-bridge images, or a UDF "NSR02"/"NSR03" descriptor, which
 * covers pure-UDF images.
 *
 * @par Utility
 * A disc image begins with small "labels" that say what kind of disc it is.
 * This function is the single place that decides whether a label means "yes,
 * this is a real disc image", so every check applies exactly the same rule.
 *
 * @param type Descriptor type byte (offset 0 of the descriptor).
 * @param id   Five-byte standard identifier (offset 1 of the descriptor).
 * @return true if the descriptor is accepted.
 */
constexpr bool isAcceptedDescriptor(uint8_t type, std::string_view id) {
    return (type == kTypePrimary && id == "CD001") ||
           id == "NSR02" || id == "NSR03";
}

/**
 * @brief Tells whether an identifier may appear in a Volume Recognition Sequence.
 *
 * "CD001", "BEA01" and "TEA01" are legitimate entries that the sequence walk
 * skips over; any other identifier means the image is not a recognised
 * optical image and the walk stops.
 *
 * @par Utility
 * Between the main labels, a disc image may contain a few harmless marker
 * entries that only mean "the list starts here" or "the list ends here". This
 * tells those markers apart from entries that mean the file is not a disc image
 * at all, so real images are not rejected and unrelated files fail quickly.
 *
 * @param id Five-byte standard identifier of the descriptor.
 * @return true if the identifier is a known sequence entry.
 */
constexpr bool isRecognitionSequenceId(std::string_view id) {
    return id == "CD001" || id == "BEA01" || id == "TEA01";
}

/**
 * @brief Validates the descriptor at sector 16 of an already-mapped image.
 *
 * @par Utility
 * This is a quick sanity test of the very beginning of the image. Most files
 * that are not disc images fail it at once, so the slower search for Windows
 * files is never wasted on them.
 *
 * @param addr    Pointer to the mapped file memory window.
 * @param mapSize Size of the mapped window in bytes.
 */
bool hasValidVolumeHeader(const char* addr, size_t mapSize) {
    if (mapSize < kVrsStart + kHeaderSize) return false;
    const auto type = static_cast<uint8_t>(addr[kVrsStart]);
    const std::string_view id(addr + kVrsStart + kIdOffset, kIdLength);
    return isAcceptedDescriptor(type, id);
}

} // namespace iso_layout

//---- Case folding -------------------------------------------
namespace case_fold {

/**
 * @brief Returns a static 256-entry ASCII case-folding table.
 *
 * Deliberately locale-independent (unlike std::tolower), so bytes >= 0x80
 * never fold and the automaton build and the scan loop always agree.
 *
 * @par Utility
 * Lets "BOOTMGR", "bootmgr" and "BootMgr" count as the same word. It uses
 * a fixed table instead of the computer's language settings, so the result is
 * the same on every machine, and converting a letter is a single quick lookup.
 */
const std::array<uint8_t, 256>& table() {
    static const auto t = [] {
        std::array<uint8_t, 256> a{};
        for (int i = 0; i < 256; ++i)
            a[i] = static_cast<uint8_t>((i >= 'A' && i <= 'Z') ? i + ('a' - 'A') : i);
        return a;
    }();
    return t;
}

} // namespace case_fold

//---- Windows signature bitmask layout -----------------------
//
// Each pattern occupies 3 consecutive bits, one per encoding:
//   bit (pattern * 3 + encoding)
//
namespace win_signature {

/// Index of each signature in kPatterns (also its group number in the bitmask).
enum Pattern  : int { BootWim = 0, BootmgrEfi = 1, Bootmgr = 2 };

/// Byte encoding in which a signature is searched for.
enum Encoding : int {
    Ascii   = 0,   ///< one byte per character
    Utf16Le = 1,   ///< character byte followed by 0x00 (Joliet/UDF on Windows)
    Utf16Be = 2    ///< 0x00 followed by the character byte
};

/// Number of encodings per pattern.
constexpr int kEncodingCount = 3;

/// Lower-case signatures, indexed by Pattern.
constexpr std::array<std::string_view, 3> kPatterns{
    "boot.wim", "bootmgr.efi", "bootmgr"};

/**
 * @brief Returns the bitmask bit for one (pattern, encoding) pair.
 *
 * @par Utility
 * The program has to remember many yes/no findings, such as "found boot.wim
 * written as plain text". This gives every combination of file name and
 * writing style its own on/off switch, so all the findings fit in one number.
 *
 * @param pattern  Pattern index.
 * @param encoding Encoding index.
 * @return A mask with exactly one bit set: 1 << (pattern * 3 + encoding).
 */
constexpr uint32_t bit(int pattern, int encoding) {
    return 1u << (pattern * kEncodingCount + encoding);
}

/**
 * @brief Returns the mask covering all three encodings of one pattern.
 *
 * @par Utility
 * The same file name can be written on a disc in three different ways, so
 * there are three switches per name. This combines them, so the code can ask one
 * question: "was this name found in any of the three ways?"
 *
 * @param pattern Pattern index.
 * @return The three adjacent bits belonging to @p pattern.
 */
constexpr uint32_t group(int pattern) {
    return bit(pattern, Ascii) | bit(pattern, Utf16Le) | bit(pattern, Utf16Be);
}

/// Any encoding of "boot.wim" (0b000000111).
constexpr uint32_t kMaskBootWim = group(BootWim);

/// Any encoding of "bootmgr.efi" or "bootmgr" (0b111111000).
constexpr uint32_t kMaskBootMgr = group(BootmgrEfi) | group(Bootmgr);

/**
 * @brief Tells whether the matches found so far are enough to call it Windows media.
 *
 * A Windows ISO needs at least one hit from each group: "boot.wim" and
 * "bootmgr[.efi]", in any encoding.
 *
 * @par Utility
 * This is the one place that defines what "looks like a Windows install disc"
 * means: boot.wim was found, and so was a bootmgr file.
 *
 * @param found Bitmask accumulated by aho_corasick::scan().
 * @return true if both groups have at least one match.
 */
constexpr bool isComplete(uint32_t found) {
    return (found & kMaskBootWim) != 0 && (found & kMaskBootMgr) != 0;
}

} // namespace win_signature

//---- Aho-Corasick automaton ---------------------------------
namespace aho_corasick {

/**
 * @brief A node in the automaton trie.
 *
 * After construction @c next is a full goto table (no -1 entries remain), so
 * the scan loop needs no failure-link chasing.
 */
struct Node {
    std::array<int, 256> next{};   ///< Transition per input byte; -1 only while building.
    int      fail{0};              ///< Failure link: longest proper suffix that is also a prefix.
    uint32_t match{0};             ///< win_signature bits of every pattern ending here (incl. via fail links).
    Node() { next.fill(-1); }
};

namespace detail {

/**
 * @brief Inserts one pattern in one encoding into the trie.
 *
 * Characters are case-folded with case_fold::table() and expanded to the
 * requested encoding (one byte, or two bytes with a 0x00 before/after). The
 * terminal node is tagged with win_signature::bit(pattern, encoding).
 *
 * @par Utility
 * Disc formats store file names in different ways: as plain letters, or as
 * letters each padded with an empty byte (two bytes per letter), in either
 * order. This adds one search word in one of those forms, so every name can be
 * found however the disc happens to store it.
 *
 * @param trie     Trie under construction; node 0 is the root.
 * @param text     Pattern text (any case; folded to lower case).
 * @param pattern  Pattern index.
 * @param encoding Encoding index.
 */
void addPattern(std::vector<Node>& trie,
                       std::string_view text, int pattern, int encoding) {
    using namespace win_signature;
    const auto& fold = case_fold::table();

    int cur = 0;
    auto step = [&](uint8_t byte) {
        if (trie[cur].next[byte] == -1) {
            trie[cur].next[byte] = static_cast<int>(trie.size());
            trie.emplace_back();
        }
        cur = trie[cur].next[byte];
    };

    for (char raw : text) {
        const uint8_t c = fold[static_cast<uint8_t>(raw)];
        switch (encoding) {
            case Ascii:   step(c);                 break;
            case Utf16Le: step(c);    step(0x00);  break;
            case Utf16Be: step(0x00); step(c);     break;
        }
    }
    trie[cur].match |= bit(pattern, encoding);
}

/**
 * @brief Computes failure links and completes the goto function (BFS).
 *
 * Processes nodes in breadth-first order so each node's failure target is
 * already finished when it is needed. Missing transitions are filled from the
 * failure target, and each node inherits its failure target's match bits.
 *
 * @par Utility
 * Finishes the search tool so it can look for all words at the same time while
 * reading the data only once and never going back. It also makes sure a name
 * hidden inside a longer one ("bootmgr" inside "bootmgr.efi") is still noticed.
 *
 * @param trie Trie whose patterns have all been inserted.
 */
void buildFailureLinks(std::vector<Node>& trie) {
    std::queue<int> q;
    for (int c = 0; c < 256; ++c) {
        int& child = trie[0].next[c];
        if (child == -1) {
            child = 0;
        } else {
            trie[child].fail = 0;
            q.push(child);
        }
    }
    while (!q.empty()) {
        const int u = q.front();
        q.pop();
        trie[u].match |= trie[trie[u].fail].match;
        for (int c = 0; c < 256; ++c) {
            const int v = trie[u].next[c];
            if (v == -1) {
                trie[u].next[c] = trie[trie[u].fail].next[c];
            } else {
                trie[v].fail = trie[trie[u].fail].next[c];
                q.push(v);
            }
        }
    }
}

} // namespace detail

/**
 * @brief Builds (once) and returns the immutable automaton.
 *
 * Matches "boot.wim", "bootmgr.efi" and "bootmgr" case-insensitively in
 * ASCII, UTF-16LE and UTF-16BE. Thread-safe via static-local initialisation.
 *
 * @par Utility
 * Building the search tool takes some work, but the result never changes. So
 * it is built the first time it is needed and reused after that.
 */
const std::vector<Node>& automaton() {
    static const auto nodes = [] {
        std::vector<Node> trie;
        trie.emplace_back();

        for (int p = 0; p < static_cast<int>(win_signature::kPatterns.size()); ++p)
            for (int e = 0; e < win_signature::kEncodingCount; ++e)
                detail::addPattern(trie, win_signature::kPatterns[p], p, e);

        detail::buildFailureLinks(trie);
        return trie;
    }();
    return nodes;
}

/**
 * @brief Scans a memory window in a single O(n) pass.
 *
 * Stops as soon as both signature groups have been seen.
 *
 * @par Utility
 * Looks for all the file names, in all their forms, in a single pass through the
 * data, instead of reading it once per name. That matters because up to 32 MiB
 * are searched.
 *
 * @return Bitmask of matched (pattern, encoding) bits.
 */
uint32_t scan(const char* addr, size_t size) {
    const auto& ac   = automaton();
    const auto& fold = case_fold::table();

    uint32_t found = 0;
    int cur = 0;
    for (size_t i = 0; i < size; ++i) {
        cur    = ac[cur].next[fold[static_cast<uint8_t>(addr[i])]];
        found |= ac[cur].match;
        if (win_signature::isComplete(found)) break;
    }
    return found;
}

} // namespace aho_corasick

//---- Read-only file mapping (RAII) --------------------------
/**
 * @brief RAII wrapper around a private, read-only mmap of a file from offset 0.
 *
 * Unmaps on destruction. Not copyable. Check ok() before using the data.
 *
 * @par Utility
 * "Mapping" lets the program look at a file as if it were already in memory,
 * while the system loads only the parts that are actually used. This wrapper
 * releases the mapping automatically, so it can never be left behind.
 */
class ScopedMapping {
public:
    /**
     * @brief Maps the first @p length bytes of @p fd read-only.
     *
     * @par Utility
     * Only the parts of the file that are looked at are really read from disk, so
     * checking a multi-gigabyte image stays fast.
     *
     * @param fd     Open, readable file descriptor (not owned, not closed).
     * @param length Number of bytes to map; must not exceed the file size.
     */
    ScopedMapping(int fd, size_t length)
        : addr_(mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0)),
          length_(length) {}

    /**
     * @brief Unmaps the window if the mapping succeeded.
     *
     * @par Utility
     * Cleanup happens by itself, so nobody has to remember to do it.
     *
     */
    ~ScopedMapping() {
        if (ok()) munmap(addr_, length_);
    }

    ScopedMapping(const ScopedMapping&)            = delete;
    ScopedMapping& operator=(const ScopedMapping&) = delete;

    /**
     * @brief Reports whether mmap succeeded.
     *
     * @par Utility
     * The system reports a failed mapping with a special value that is easy to
     * overlook. This turns it into a plain yes or no.
     *
     * @return true if mmap succeeded.
     */
    bool        ok()   const { return addr_ != MAP_FAILED; }

    /**
     * @brief Returns the start of the mapped window.
     *
     * @par Utility
     * Tells the other functions where the image data begins.
     *
     * @return Pointer to the start of the mapped window (valid only if ok()).
     */
    const char* data() const { return static_cast<const char*>(addr_); }

    /**
     * @brief Returns the length of the mapped window.
     *
     * @par Utility
     * Tells the other functions how much data is available, so nothing reads past
     * the end. This can be less than the whole file.
     *
     * @return Size of the mapped window in bytes.
     */
    size_t      size() const { return length_; }

    /**
     * @brief Hints the kernel about the upcoming sequential scan.
     *
     * Applies MADV_WILLNEED to the first @p eagerBytes so page-faulting starts
     * immediately, and MADV_SEQUENTIAL to the whole window so read-ahead stays
     * primed if the full window is scanned. Hints are best-effort; failures
     * are ignored.
     *
     * @par Utility
     * It tells the operating system "I will read this from start to end, in order".
     * The system can then load the next part of the file ahead of time, so the
     * search does not have to wait for the disk.
     *
     * @param eagerBytes Prefix length to prefetch (clamped to the window size).
     */
    void adviseSequentialScan(size_t eagerBytes) const {
        madvise(addr_, std::min(eagerBytes, length_), MADV_WILLNEED);
        madvise(addr_, length_, MADV_SEQUENTIAL);
    }

private:
    void*  addr_;
    size_t length_;
};

//---- ISO detection entry points -----------------------------

/**
 * @brief Checks that a file descriptor refers to an ISO 9660 or UDF disc image.
 *
 * Walks the Volume Recognition Sequence (sectors 16-23, 2048-byte spacing)
 * with pread, so the descriptor's offset is untouched. Accepts an ISO 9660
 * PVD (type 0x01, "CD001"), which covers UDF-bridge images, or a UDF
 * "NSR02"/"NSR03" descriptor, which covers pure-UDF images. "CD001" non-PVD
 * entries, "BEA01" and "TEA01" are skipped; any other identifier rejects the
 * image immediately.
 *
 * @param fd Open, readable descriptor for the image. Not closed by this
 *           function.
 * @return true if an accepted descriptor is found; false on a short read, an
 *         unrecognized identifier, or no match within 8 sectors.
 *
 * @note Acceptance does not imply the image is bootable. The name is
 *       historical; isValidOpticalImage would describe it more accurately.
 */
bool isValidIso9660(int fd) {
    using namespace iso_layout;

    for (int i = 0; i < kVrsSectors; ++i) {
        uint8_t d[kHeaderSize];
        const off_t offset = static_cast<off_t>(kVrsStart + i * kSectorSize);
        if (pread(fd, d, sizeof(d), offset) != static_cast<ssize_t>(sizeof(d)))
            return false;

        const std::string_view id(reinterpret_cast<const char*>(d) + kIdOffset, kIdLength);
        if (isAcceptedDescriptor(d[0], id)) return true;
        if (!isRecognitionSequenceId(id))   return false;
    }
    return false;
}

/**
 * @brief Probes an ISO image to detect Windows 10/11 installation media.
 *
 * Maps up to the first 32 MiB of the file and checks, in order:
 *  1. a valid volume header at sector 16 (PVD "CD001", or UDF "NSR02"/"NSR03");
 *  2. a single-pass Aho-Corasick scan for "boot.wim" and "bootmgr[.efi]",
 *     case-insensitive, in ASCII / UTF-16LE / UTF-16BE (the latter is the
 *     native Joliet/UDF filename encoding).
 *
 * The 32 MiB window suffices because the directory entries for bootmgr and
 * boot.wim always sit within the first few MiB. The scan exits as soon as
 * both signature groups have matched, so on typical Windows ISOs most of the
 * window is never touched.
 *
 * @param fd       Open, readable descriptor; mapped from offset 0, not closed here.
 * @param fileSize Image size in bytes (from fstat); used for the minimum-size
 *                 guard and to clamp the mapping window.
 * @return true if the header is valid and both signature groups are present.
 *
 * @note Read-only; the file is not modified.
 */
bool isWindowsIsoInitialCheck(int fd, off_t fileSize) {
    constexpr off_t  kMinFileSize = 0x9000;
    constexpr size_t kMaxWindow   = 32 * 1024 * 1024;
    constexpr size_t kEagerBytes  = 4 * 1024 * 1024;

    if (fileSize < kMinFileSize) return false;

    const ScopedMapping map(fd, std::min(static_cast<size_t>(fileSize), kMaxWindow));
    if (!map.ok()) return false;

    map.adviseSequentialScan(kEagerBytes);

    if (!iso_layout::hasValidVolumeHeader(map.data(), map.size())) return false;

    return win_signature::isComplete(aho_corasick::scan(map.data(), map.size()));
}

//-------------------------------------------------------------
//    2. Write pipeline
//-------------------------------------------------------------

/// Clock used for all progress and speed timing.
using Clock = std::chrono::high_resolution_clock;

/// Target size of each I/O buffer (rounded down to a sector multiple at use).
constexpr size_t DESIRED_BUFFER  = 4 * 1024 * 1024;

/// Upper bound for a single device write(); keeps progress/cancel granularity fine.
constexpr size_t MAX_WRITE_CHUNK = 4 * 1024 * 1024;

/// Number of circulating buffers (double buffering).
constexpr int    NUM_BUFFERS     = 2;
static_assert(MAX_WRITE_CHUNK <= DESIRED_BUFFER,
              "write chunk larger than the buffer is pointless");

/// Pool of sector-aligned buffers. AlignedBuffer (write2usb.h) is non-movable,
/// so the pool holds unique_ptrs.
using IsoBufferPool = std::vector<std::unique_ptr<AlignedBuffer>>;

/**
 * @brief Flags one operation as failed.
 *
 * @par Utility
 * When something goes wrong, two things must always happen: the progress
 * display is told this job failed, and the function reports failure. Doing both
 * in one place keeps every error path consistent.
 *
 * @param progressData Progress array shared with the UI.
 * @param index        Slot belonging to this operation.
 * @return Always false, so callers can write `return markFailed(...)`.
 */
bool markFailed(std::vector<ProgressInfo>& progressData, size_t index) {
    progressData[index].failed.store(true);
    return false;
}

/**
 * @brief RAII owner of a file descriptor.
 *
 * Closes the descriptor on scope exit if it is valid (!= -1). Not copyable.
 */
struct FdGuard {
    int fd;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() { if (fd != -1) close(fd); }
    FdGuard(const FdGuard&)            = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

/**
 * @brief Queries the sector size that O_DIRECT I/O must be aligned to.
 *
 * Returns max(logical, physical) sector size, which is correct for 512n,
 * 512e and 4Kn drives. BLKPBSZGET is best-effort: if it fails, the logical
 * size is used.
 *
 * @par Utility
 * Drives store data in fixed-size blocks called sectors, usually 512 or 4096
 * bytes. Writing straight to the drive, bypassing the system's cache, only works
 * if the data is sent in whole sectors of exactly the right size. The size
 * differs between drives, so the program has to ask the drive.
 *
 * @param devFd      Open block-device descriptor.
 * @param[out] sectorSize Receives the effective sector size in bytes.
 * @return true on success; false if the logical size cannot be determined.
 */
bool queryEffectiveSectorSize(int devFd, int& sectorSize) {
    int logical  = 0;
    int physical = 0;

    if (ioctl(devFd, BLKSSZGET, &logical) < 0 || logical <= 0)
        return false;
    if (ioctl(devFd, BLKPBSZGET, &physical) < 0 || physical <= 0)
        physical = logical;

    sectorSize = std::max(logical, physical);
    return true;
}

/**
 * @brief Allocates NUM_BUFFERS sector-aligned buffers into @p out.
 *
 * @par Utility
 * Direct writes also need the data to sit in memory at a specially aligned
 * address, which ordinary memory allocation does not promise. This prepares
 * those memory areas ("buffers"). There are two, so one can be filled from the
 * ISO while the other is being written to the drive.
 *
 * @param out        Pool to fill (appended to).
 * @param sectorSize Alignment of every buffer, in bytes.
 * @param bufferSize Size of every buffer, in bytes (multiple of sectorSize).
 * @return true if all buffers were allocated; false on the first failure.
 */
bool allocateAlignedBuffers(IsoBufferPool& out, int sectorSize, size_t bufferSize) {
    for (int i = 0; i < NUM_BUFFERS; ++i) {
        auto buf = std::make_unique<AlignedBuffer>(bufferSize, static_cast<size_t>(sectorSize));
        if (!buf->ok())
            return false;
        out.push_back(std::move(buf));
    }
    return true;
}

/**
 * @brief Validates the ISO, rewinds it, and fetches its stat info.
 *
 * @par Utility
 * Gathers the checks that must pass before any copying starts: the file is a
 * real disc image, it will be read from its beginning, and its size is known.
 * The caller handles one failure instead of three.
 *
 * @param isoFd      Open, readable ISO descriptor.
 * @param[out] sb    Receives fstat() data on success.
 * @return true if the image is a valid ISO 9660/UDF image, the offset was
 *         reset to 0 and fstat succeeded.
 */
bool prepareIso(int isoFd, struct stat& sb) {
    return isValidIso9660(isoFd)
        && lseek(isoFd, 0, SEEK_SET) != -1
        && fstat(isoFd, &sb) != -1;
}

/**
 * @brief Exponentially smoothed write-speed estimator with stall handling.
 *
 * Fed periodically with the cumulative byte count. Samples closer than one
 * second apart are ignored. While bytes keep arriving it publishes a smoothed
 * MB/s figure (heavier smoothing below 10 MB/s). If nothing arrives it holds
 * the last figure, and reports 0 after 60 s without progress.
 *
 * @par Utility
 * USB drives write in bursts, so the raw speed jumps up and down and would
 * flicker on screen. This evens it out, keeps showing the last speed during short
 * pauses, and shows 0 if nothing has been written for a minute.
 */
class SpeedEstimator {
public:
    /**
     * @brief Creates an estimator whose baseline is the transfer start.
     *
     * @par Utility
     * Starting the clock at the real start time makes the first measurement
     * accurate.
     * @param start Time the transfer started (initial baseline).
     */
    explicit SpeedEstimator(Clock::time_point start)
        : lastUpdate_(start), stallStart_(start) {}

    /**
     * @brief Feeds one observation.
     *
     * @par Utility
     * It is the single place where measurements are fed in. It ignores updates that
     * arrive too quickly and picks the right handling for "data was written" or
     * "nothing was written".
     *
     * @param currentWritten Cumulative bytes written so far.
     * @param now            Time of the observation.
     * @return Speed in MB/s to publish, or std::nullopt if nothing should change.
     */
    std::optional<double> sample(uint64_t currentWritten, Clock::time_point now) {
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - lastUpdate_).count();
        if (elapsedMs < MIN_UPDATE_INTERVAL_MS)
            return std::nullopt;

        const uint64_t delta = (currentWritten >= lastWritten_) ? (currentWritten - lastWritten_) : 0;
        return delta > 0 ? onProgress(currentWritten, delta, elapsedMs, now)
                         : onNoProgress(now);
    }

private:
    /// Smoothing factor for normal speeds (0.2 is used below 10 MB/s).
    static constexpr double ALPHA = 0.3;
    /// Minimum time between estimates.
    static constexpr int MIN_UPDATE_INTERVAL_MS = 1000;
    /// Time without progress after which the speed is reported as 0.
    static constexpr int STALL_TIMEOUT_SECONDS  = 60;

    /**
     * @brief Handles a sample in which bytes arrived.
     *
     * Computes the instantaneous speed over the window since the last real
     * data point, folds it into the smoothed value and resets the baseline.
     *
     * @par Utility
     * Works out the speed from what was written since the last measurement, and
     * blends it with earlier values so the number changes smoothly.
     *
     * @param currentWritten Cumulative bytes written.
     * @param delta          Bytes written since the last baseline (> 0).
     * @param elapsedMs      Milliseconds since the last baseline.
     * @param now            Time of the observation.
     * @return The new smoothed speed in MB/s.
     */
    double onProgress(uint64_t currentWritten, uint64_t delta, long long elapsedMs,
                      Clock::time_point now) {
        stalled_ = false;

        const double instant = (static_cast<double>(delta) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
        // Heavier smoothing for slow drives (< 10 MB/s) to reduce display
        // jitter from bursty O_DIRECT writes at low throughput.
        const double factor = (instant < 10.0) ? 0.2 : ALPHA;

        smoothed_ = haveEstimate_ ? (factor * instant + (1.0 - factor) * smoothed_) : instant;
        haveEstimate_ = true;

        // Only reset the baseline when bytes actually arrived — this keeps
        // `elapsed` spanning back to the last real data point instead of an
        // arbitrary polling tick, which would truncate the window to ~1s and
        // produce a spurious floor (one write chunk per interval) on
        // slow/bursty O_DIRECT writers.
        lastWritten_ = currentWritten;
        lastUpdate_  = now;
        return smoothed_;
    }

    /**
     * @brief Handles a sample in which no bytes arrived.
     *
     * Starts (or continues) stall tracking and leaves the baseline untouched
     * so the next real sample spans the full transfer duration.
     *
     * @par Utility
     * Handles the case where nothing new was written. It waits, keeps the last
     * speed on screen, and only reports 0 after a full minute of silence. The
     * measuring window keeps growing meanwhile, so a slow write is judged fairly.
     *
     * @param now Time of the observation.
     * @return 0.0 once the stall timeout is reached; otherwise std::nullopt
     *         (keep showing the last speed).
     */
    std::optional<double> onNoProgress(Clock::time_point now) {
        if (!stalled_) {
            stalled_    = true;
            stallStart_ = now;
        }
        const auto stallSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                      now - stallStart_).count();
        // Deliberately do NOT touch lastWritten_/lastUpdate_ here — let the
        // elapsed window keep growing while a slow write is still in flight,
        // so the eventual speed sample reflects the true transfer duration.
        if (stallSeconds >= STALL_TIMEOUT_SECONDS)
            return 0.0;
        return std::nullopt;
    }

    Clock::time_point lastUpdate_;
    Clock::time_point stallStart_;
    uint64_t lastWritten_  = 0;
    double   smoothed_     = 0.0;
    bool     haveEstimate_ = false;
    bool     stalled_      = false;
};

/**
 * @brief Streams an ISO to a block device using double buffering.
 *
 * A reader thread fills aligned buffers from the ISO (zero-padding the tail);
 * the calling thread writes them to the device with O_DIRECT; a monitor
 * thread publishes progress and speed. Buffers circulate between an "empty"
 * and a "full" queue. Single use: construct, call run() once.
 */
class IsoStreamer {
public:
    /**
     * @param isoFd         ISO descriptor, positioned at offset 0 (not owned).
     * @param devFd         Block device opened with O_DIRECT (not owned).
     * @param fileSize      True ISO size in bytes.
     * @param paddedSize    @p fileSize rounded up to a sector multiple.
     * @param bufferSize    Size of each buffer (multiple of the sector size).
     * @param writeChunk    Maximum bytes per write() (multiple of the sector size).
     * @param buffers       Pre-allocated aligned buffers; they must outlive the streamer.
     * @param progressData  Progress array shared with the UI.
     * @param progressIndex Slot of this operation in @p progressData.
     */
    IsoStreamer(int isoFd, int devFd, uint64_t fileSize, uint64_t paddedSize,
                size_t bufferSize, size_t writeChunk,
                IsoBufferPool& buffers,
                std::vector<ProgressInfo>& progressData, size_t progressIndex)
        : isoFd_(isoFd), devFd_(devFd), fileSize_(fileSize), paddedSize_(paddedSize),
          bufferSize_(bufferSize), writeChunk_(writeChunk),
          progressData_(progressData), progressIndex_(progressIndex) {
        for (auto& b : buffers)
            emptyQ_.push_back({b->data, 0});
    }

    /**
     * @brief Runs the transfer to completion, failure or cancellation.
     *
     * Starts the helper threads, writes on the calling thread, joins the
     * helpers, then (on success) drops the ISO from the page cache and
     * fsync()s the device.
     *
     * @return true if every byte was written and flushed; false on read
     *         failure, I/O failure, fsync failure or cancellation.
     */
    bool run() {
        startTime_ = Clock::now();

        try {
            monitorThread_ = std::thread([this] { monitorLoop(); });
            readerThread_  = std::thread([this] { readerLoop(); });
            pumpToDevice();
        } catch (...) {
            shutdownThreads();
            if (!isCancelled()) markFailed();
            return false;
        }

        shutdownThreads();

        if (readFailed_ || ioFailed_) {
            if (!isCancelled()) markFailed();
            return false;
        }

        posix_fadvise(isoFd_, 0, 0, POSIX_FADV_DONTNEED);

        if (!aborted() && fsync(devFd_) != 0) {
            markFailed();
            return false;
        }

        return finishIfComplete();
    }

private:
    /// A buffer handed between the reader and the writer.
    struct Chunk {
        char*  buf;   ///< Aligned buffer start.
        size_t len;   ///< Valid bytes; 0 == end-of-stream sentinel.
    };

    /**
     * @brief Reads the global cancellation flag.
     *
     * @par Utility
     * The user can press cancel at any moment. This is the one way every part of
     * the copy asks "should I stop?".
     *
     * @return true if the user requested cancellation globally.
     */
    static bool isCancelled() { return GlobalState::g_operationCancelled.load(); }

    /**
     * @brief Tells whether the transfer was stopped on purpose.
     *
     * @par Utility
     * It tells "stopped on purpose" apart from "something went wrong". A cancelled
     * copy should not be shown as an error and does not need the final flush.
     *
     * @return true if this streamer stopped, or the user cancelled.
     */
    bool aborted() const { return cancelled_ || isCancelled(); }

    /**
     * @brief Flags this operation as failed.
     *
     * @par Utility
     * A shortcut for telling the progress display that this copy failed.
     *
     * @return Always false.
     */
    bool markFailed() {
        return ::markFailed(progressData_, progressIndex_);
    }

    // ---- lifecycle -------------------------------------------------------

    /**
     * @brief Stops and joins the reader and monitor threads.
     *
     * Idempotent. The condition variables are notified so neither thread
     * stays blocked.
     *
     * @par Utility
     * The copy uses two helper workers, one reading the ISO and one reporting
     * progress. They must always be stopped and waited for, even after an error,
     * otherwise the program could crash or hang.
     */
    void shutdownThreads() {
        {
            std::lock_guard<std::mutex> lk(qMutex_);
            stopReader_ = true;
        }
        qCv_.notify_all();
        if (readerThread_.joinable()) readerThread_.join();

        {
            std::lock_guard<std::mutex> lk(monMutex_);
            monitoringActive_.store(false);
        }
        monCv_.notify_all();
        if (monitorThread_.joinable()) monitorThread_.join();
    }

    /**
     * @brief Publishes the final result if the whole image was written.
     *
     * Stores the average speed, sets progress to 100 and marks completion.
     *
     * @par Utility
     * It makes sure "100 %" and "done" are shown only when every byte has really
     * been written and the copy was not cancelled.
     *
     * @return true if the transfer completed; false if aborted or incomplete.
     */
    bool finishIfComplete() {
        if (aborted() || localBytesWritten_ < paddedSize_)
            return false;

        const auto totalElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      Clock::now() - startTime_);
        const double seconds  = totalElapsed.count() / 1000.0;
        const double avgSpeed = seconds > 0.0
            ? (static_cast<double>(fileSize_) / (1024.0 * 1024.0)) / seconds
            : 0.0;

        auto& info = progressData_[progressIndex_];
        info.speed.store(avgSpeed);
        info.progress.store(100);
        info.completed.store(true);
        return true;
    }

    // ---- progress monitor ------------------------------------------------

    /**
     * @brief Waits one 500 ms monitor tick, interruptibly.
     *
     * Sleeps on a condition variable so shutdown wakes it immediately
     * instead of waiting out a fixed sleep.
     *
     * @return false once shutdown was requested.
     */
    bool waitForTick() {
        {
            std::unique_lock<std::mutex> lk(monMutex_);
            monCv_.wait_for(lk, std::chrono::milliseconds(500),
                            [this] { return !monitoringActive_.load(); });
        }
        return monitoringActive_.load();
    }

    /**
     * @brief Publishes byte count and percentage to the UI.
     *
     * The percentage is capped at 99 until finishIfComplete() sets 100.
     *
     * @par Utility
     * Turns "bytes written" into the percentage shown on screen. It stops at 99 %
     * because the data is not safely on the drive until the final flush is done.
     *
     * @param currentWritten Cumulative bytes written, excluding tail padding.
     */
    void publishProgress(uint64_t currentWritten) {
        auto& info = progressData_[progressIndex_];
        info.bytesWritten.store(currentWritten);
        info.progress.store(static_cast<int>(
            std::min(99.0, (static_cast<double>(currentWritten) / fileSize_) * 100.0)));
    }

    /**
     * @brief Monitor-thread body: every tick, publishes progress and the smoothed speed.
     *
     * @par Utility
     * A separate worker updates the progress and speed, so the screen stays
     * current even while the drive is busy with a slow write.
     */
    void monitorLoop() {
        SpeedEstimator estimator(Clock::now());

        while (monitoringActive_.load()) {
            if (!waitForTick()) break;

            const auto now = Clock::now();
            const uint64_t currentWritten = directBytesWritten_.load();

            publishProgress(currentWritten);

            if (auto speed = estimator.sample(currentWritten, now))
                progressData_[progressIndex_].speed.store(*speed);
        }
    }

    // ---- reader thread ---------------------------------------------------

    /**
     * @brief Queues the end-of-stream marker for the writer.
     *
     * @par Utility
     * The writer waits for data, so it has to be told clearly when no more will
     * come, whether the file ended normally or reading failed. The failure note is
     * recorded together with this message so the writer cannot miss it.
     *
     * @param failed If true, also records that the reader failed.
     */
    void pushSentinel(bool failed) {
        {
            std::lock_guard<std::mutex> lk(qMutex_);
            if (failed) readFailed_ = true;
            fullQ_.push_back({nullptr, 0});
        }
        qCv_.notify_all();
    }

    /**
     * @brief Hands a filled buffer to the writer and wakes it.
     *
     * @par Utility
     * Passes the buffer on and alerts the writer in one step, so a buffer is never
     * left waiting unnoticed.
     */
    void pushFull(const Chunk& c) {
        {
            std::lock_guard<std::mutex> lk(qMutex_);
            fullQ_.push_back(c);
        }
        qCv_.notify_all();
    }

    /**
     * @brief Blocks until an empty buffer is available.
     *
     * @par Utility
     * The reader waits here until a buffer is free. That stops it from racing ahead
     * of the writer and using more and more memory. It can also be interrupted, so
     * the copy can always be stopped.
     *
     * @param[out] out The acquired buffer.
     * @return false if the reader was told to stop while waiting.
     */
    bool acquireEmptyBuffer(Chunk& out) {
        std::unique_lock<std::mutex> lk(qMutex_);
        qCv_.wait(lk, [this] { return stopReader_ || !emptyQ_.empty(); });
        if (stopReader_) return false;
        out = emptyQ_.front();
        emptyQ_.pop_front();
        return true;
    }

    /**
     * @brief Reads until @p toRead bytes are gathered, EOF, or an error.
     *
     * Retries on EINTR. A short read mid-file is not treated as EOF; only a
     * read() returning 0 is.
     *
     * @par Utility
     * A read can return less data than asked for even though more is coming, and
     * the drive wants full blocks. This keeps asking until the buffer is full or
     * the file really ends, and it tells a real end apart from an error.
     *
     * @param buf            Destination buffer.
     * @param toRead         Bytes wanted.
     * @param[out] got       Bytes actually read.
     * @param[out] reachedEof Set when read() returned 0.
     * @return false on a read error; true otherwise.
     */
    bool readFully(char* buf, size_t toRead, size_t& got, bool& reachedEof) {
        got = 0;
        reachedEof = false;
        while (got < toRead) {
            ssize_t r = read(isoFd_, buf + got, toRead - got);
            if (r < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            if (r == 0) {
                reachedEof = true;
                break;
            }
            got += static_cast<size_t>(r);
        }
        return true;
    }

    /**
     * @brief Reader-thread body.
     *
     * Fills buffers until the padded size is read, zero-padding the tail.
     * A source shorter than fstat reported is treated as a failure instead of
     * being silently padded. Always ends by pushing the end-of-stream
     * sentinel (unless told to stop).
     *
     * @par Utility
     * Reading the ISO runs alongside writing to the USB drive, so neither has to
     * wait for the other.
     */
    void readerLoop() {
        uint64_t readTotal = 0;

        while (readTotal < paddedSize_ && !isCancelled()) {
            Chunk c;
            if (!acquireEmptyBuffer(c)) return;

            const size_t toRead = static_cast<size_t>(
                std::min<uint64_t>(bufferSize_, paddedSize_ - readTotal));

            size_t got = 0;
            bool reachedEof = false;

            if (!readFully(c.buf, toRead, got, reachedEof)) {
                pushSentinel(true);
                return;
            }

            // A source shorter than fstat reported (truncated file) must
            // fail rather than be zero-padded silently.
            if (reachedEof && readTotal + got < fileSize_) {
                pushSentinel(true);
                return;
            }

            if (got < toRead)
                std::memset(c.buf + got, 0, toRead - got);

            c.len = toRead;
            readTotal += toRead;
            pushFull(c);

            if (reachedEof) break;
        }

        pushSentinel(false);
    }

    // ---- writer (calling thread) -----------------------------------------

    /**
     * @brief Blocks for the next filled buffer.
     *
     * @param[out] out The received chunk.
     * @return false on the end-of-stream sentinel (reader finished, failed
     *         or was cancelled).
     */
    bool popFull(Chunk& out) {
        std::unique_lock<std::mutex> lk(qMutex_);
        qCv_.wait(lk, [this] { return !fullQ_.empty(); });
        out = fullQ_.front();
        fullQ_.pop_front();
        return out.len != 0;
    }

    /**
     * @brief Returns a written buffer to the empty queue and wakes the reader.
     */
    void recycle(char* buf) {
        {
            std::lock_guard<std::mutex> lk(qMutex_);
            emptyQ_.push_back({buf, 0});
        }
        qCv_.notify_all();
    }

    /**
     * @brief Accounts for bytes just written to the device.
     *
     * Updates the internal total (including padding) and the reported total,
     * which is capped at the true file size so padding stays hidden.
     *
     * @par Utility
     * Keeps the running total. The padding added to fill the last block is left
     * out of the reported figure, so progress never goes beyond the real file size,
     * but it is still counted internally to know when everything is done.
     *
     * @param written Bytes written by the last write() call.
     */
    void recordWritten(size_t written) {
        // Cap reported progress at the true file size to hide padding.
        const uint64_t reportable = std::min<uint64_t>(
            static_cast<uint64_t>(written),
            fileSize_ - std::min(localBytesWritten_, fileSize_));

        localBytesWritten_ += static_cast<uint64_t>(written);
        directBytesWritten_.fetch_add(reportable);
    }

    /**
     * @brief Writes one buffer to the device.
     *
     * Handles partial writes and EINTR. Each write() is capped at
     * writeChunk_ (a multiple of the sector size), so buffer address, length
     * and device offset all stay sector-aligned for O_DIRECT, while progress
     * is reported and cancellation checked after every write of <= 4 MiB.
     *
     * @par Utility
     * Sending data to a drive may take several tries, because it can be partial or
     * interrupted. This deals with that, and checks for cancel after each piece,
     * so the main loop can stay simple.
     *
     * @param c Chunk to write.
     * @return false on I/O failure or cancellation (ioFailed_ / cancelled_
     *         say which).
     */
    bool writeBuffer(const Chunk& c) {
        size_t remaining = c.len;
        char*  ptr       = c.buf;

        while (remaining > 0) {
            const size_t thisWrite = std::min(remaining, writeChunk_);
            ssize_t written = write(devFd_, ptr, thisWrite);

            if (written < 0) {
                if (errno == EINTR) continue;
                ioFailed_ = true;
                return false;
            }
            if (written == 0) {   // no progress; avoid infinite spin
                ioFailed_ = true;
                return false;
            }
            if (isCancelled()) {
                cancelled_ = true;
                return false;
            }

            remaining -= static_cast<size_t>(written);
            ptr       += written;
            recordWritten(static_cast<size_t>(written));
        }
        return true;
    }

    /**
     * @brief Writer loop (calling thread): pops full chunks, writes them, recycles buffers.
     *
     * @par Utility
     * This is the main loop of the writing side: take a filled buffer, write it,
     * hand it back. It is kept separate so errors can be handled in one place.
     */
    void pumpToDevice() {
        Chunk c;
        while (popFull(c)) {
            if (isCancelled()) {
                cancelled_ = true;
                break;
            }
            if (!writeBuffer(c)) break;
            recycle(c.buf);
        }
    }

    // ---- state -----------------------------------------------------------

    const int      isoFd_;
    const int      devFd_;
    const uint64_t fileSize_;
    const uint64_t paddedSize_;
    const size_t   bufferSize_;
    const size_t   writeChunk_;

    std::vector<ProgressInfo>& progressData_;
    const size_t               progressIndex_;

    std::mutex              qMutex_;
    std::condition_variable qCv_;
    std::deque<Chunk>       emptyQ_;
    std::deque<Chunk>       fullQ_;
    bool                    stopReader_ = false;
    bool                    readFailed_ = false;

    std::atomic<uint64_t>   directBytesWritten_{0};
    std::atomic<bool>       monitoringActive_{true};
    std::mutex              monMutex_;
    std::condition_variable monCv_;

    std::thread       readerThread_;
    std::thread       monitorThread_;
    Clock::time_point startTime_;

    uint64_t localBytesWritten_ = 0;
    bool     cancelled_         = false;
    bool     ioFailed_          = false;
};

} // namespace

//-------------------------------------------------------------
//    3. Public entry point
//-------------------------------------------------------------

/**
 * @brief Writes an ISO image to a raw block device, routing Windows and
 *        Linux media to the appropriate writer.
 *
 * Opens the ISO once, validates it with isValidIso9660, then checks for
 * Windows media with isWindowsIsoInitialCheck. Windows images are handed to
 * writeWindowsIsoToDevice (multi-partition layout, FAT32+NTFS, cluster
 * tuning). Everything else is written as a raw image using O_DIRECT.
 *
 * @par Raw write path
 * Uses two 4 MiB sector-aligned buffers, rounded down to the device's
 * effective sector size (the larger of BLKSSZGET and BLKPBSZGET; the physical
 * size is best-effort) so O_DIRECT alignment holds on 512n, 512e and 4Kn
 * drives. A reader thread fills one buffer, including zero-padding of the
 * final partial sector, while the calling thread writes the other. Device
 * writes are sector-aligned and capped at 4 MiB to keep progress and
 * cancellation responsive on slow USB drives. Short reads mid-file are not
 * treated as EOF.
 *
 * @par Progress
 * A monitor thread samples the written-byte count every 500 ms and publishes
 * progress (capped at 99% until done) and a smoothed speed. Speed holds
 * through stalls and resets to 0 after 60 s without progress.
 *
 * @par Cancellation
 * Both reader and writer check GlobalState::g_operationCancelled. On abort
 * they stop promptly and skip the final fsync.
 *
 * @param isoPath       Path to the source ISO.
 * @param device        Target block device (e.g. /dev/sdb).
 *                      WARNING: existing data is destroyed.
 * @param progressIndex Slot in the progress array for this operation.
 * @param progressData  Operation-local progress, speed and completion data.
 * @return true if the image was fully written and fsynced; false on I/O
 *         error, validation failure or cancellation. The result is not read
 *         back or verified against the ISO.
 *
 * @see isValidIso9660
 * @see isWindowsIsoInitialCheck
 * @see writeWindowsIsoToDevice
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex, std::vector<ProgressInfo>& progressData) {

    FdGuard iso(open(isoPath.c_str(), O_RDONLY));
    if (iso.fd == -1) return markFailed(progressData, progressIndex);

    struct stat sb;
    if (!prepareIso(iso.fd, sb)) return markFailed(progressData, progressIndex);

    if (isWindowsIsoInitialCheck(iso.fd, sb.st_size))
        return writeWindowsIsoToDevice(isoPath, device, progressIndex, progressData);

    if (lseek(iso.fd, 0, SEEK_SET) == -1) return markFailed(progressData, progressIndex);
    posix_fadvise(iso.fd, 0, 0, POSIX_FADV_SEQUENTIAL);

    FdGuard dev(open(device.c_str(), O_WRONLY | O_DIRECT));
    if (dev.fd == -1) return markFailed(progressData, progressIndex);

    int sectorSize = 0;
    if (!queryEffectiveSectorSize(dev.fd, sectorSize))
        return markFailed(progressData, progressIndex);

    const uint64_t fileSize = static_cast<uint64_t>(sb.st_size);
    if (fileSize == 0) return markFailed(progressData, progressIndex);

    const uint64_t paddedSize = ((fileSize + sectorSize - 1) / sectorSize) * sectorSize;

    size_t bufferSize = (DESIRED_BUFFER / sectorSize) * sectorSize;
    if (bufferSize == 0) bufferSize = sectorSize;

    // Device writes are capped at 4 MiB and kept sector-aligned so the
    // progress byte counter advances at fine granularity. With the current
    // 4 MiB buffers each buffer is one write(); the cap matters if
    // DESIRED_BUFFER is raised, because a single very large write() on a slow
    // USB drive (< 10 MB/s) blocks for many seconds and makes the monitor's
    // speed samples noisy. Must stay a multiple of sectorSize for O_DIRECT.
    const size_t writeChunk = std::max<size_t>((MAX_WRITE_CHUNK / sectorSize) * sectorSize,
                                               static_cast<size_t>(sectorSize));

    IsoBufferPool buffers;
    if (!allocateAlignedBuffers(buffers, sectorSize, bufferSize))
        return markFailed(progressData, progressIndex);

    IsoStreamer streamer(iso.fd, dev.fd, fileSize, paddedSize, bufferSize, writeChunk,
                         buffers, progressData, progressIndex);
    return streamer.run();
}
