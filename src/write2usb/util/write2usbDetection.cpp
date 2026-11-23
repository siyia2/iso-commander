// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <queue>
#include <string_view>
#include <vector>

// C / System Headers
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

// Project Headers
#include "isocmd/write2usb/isoDetect.h"

namespace {

    // ===========================================================================
    // ISO detection
    // ===========================================================================

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
         * @return true if the descriptor at sector 16 is accepted.
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

    //---- Read-only file mapping ---------------------------------
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

} // namespace

namespace iso_detect {

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

} // namespace iso_detect
