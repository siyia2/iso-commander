// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief ISO 9660/UDF recognition and multi-encoding Windows signature scanning.
 */

#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

import std;

namespace {

	// ===========================================================================
	// ISO detection
	// ===========================================================================

	//---- ISO 9660 / UDF volume recognition ----------------------
	namespace iso_layout {

		constexpr std::size_t kSectorSize  = 2048;
		constexpr std::size_t kVrsStart    = 16 * kSectorSize;   // 0x8000: first volume descriptor.
		constexpr int         kVrsSectors  = 8;                  // Sectors 16–23 are examined.
		constexpr std::size_t kIdOffset    = 1;                  // Identifier follows the type byte.
		constexpr std::size_t kIdLength    = 5;
		constexpr std::size_t kHeaderSize  = kIdOffset + kIdLength;
		constexpr std::uint8_t kTypePrimary = 0x01;               // Primary Volume Descriptor.

		/**
		 * @brief Recognizes the ISO 9660 and UDF volume descriptors used to identify disc images.
		 * Accepts ISO 9660 primary descriptors (type 0x01, CD001) and UDF NSR02/NSR03.
		 */
		constexpr bool isAcceptedDescriptor(std::uint8_t type, std::string_view id) {

			return (type == kTypePrimary && id == "CD001") ||
			       id == "NSR02" || id == "NSR03";

		}

		/**
		 * @brief Recognizes CD001, BEA01 and TEA01 entries that can be skipped while
		 * searching for a UDF volume descriptor. An unknown identifier ends the scan.
		 */
		constexpr bool isRecognitionSequenceId(std::string_view id) {

			return id == "CD001" || id == "BEA01" || id == "TEA01";

		}

		/// Checks the descriptor at sector 16, if the mapped region contains it.
		bool hasValidVolumeHeader(const char* addr, std::size_t mapSize) {

			if (mapSize < kVrsStart + kHeaderSize) return false;
			const auto type = static_cast<std::uint8_t>(addr[kVrsStart]);
			const std::string_view id(addr + kVrsStart + kIdOffset, kIdLength);
			return isAcceptedDescriptor(type, id);

		}

	} // namespace iso_layout

	//---- Case folding -------------------------------------------
	namespace case_fold {

		/**
		 * @brief Converts ASCII uppercase bytes to lowercase using a lookup table.
		 * Leaves non-ASCII bytes unchanged so the scan is independent of locale.
		 */
		const std::array<std::uint8_t, 256>& table() {

			static const auto t = [] {

				std::array<std::uint8_t, 256> a{};
				for (int i = 0; i < 256; ++i)
				    a[i] = static_cast<std::uint8_t>((i >= 'A' && i <= 'Z') ? i + ('a' - 'A') : i);
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
			Ascii   = 0, ///< one byte per character
			Utf16Le = 1, ///< character byte followed by 0x00 (Joliet/UDF on Windows)
			Utf16Be = 2  ///< 0x00 followed by the character byte
		};

		/// Number of encodings per pattern.
		constexpr int kEncodingCount = 3;

		/// Lower-case signatures, indexed by Pattern.
		constexpr std::array<std::string_view, 3> kPatterns{
			"boot.wim", "bootmgr.efi", "bootmgr"};

		/// Assigns one bit to each filename/encoding pair in the match mask.
		constexpr std::uint32_t bit(int pattern, int encoding) {

			return 1u << (pattern * kEncodingCount + encoding);

		}

		/// Combines the bits for ASCII, UTF-16LE and UTF-16BE of one filename.
		constexpr std::uint32_t group(int pattern) {

			return bit(pattern, Ascii) | bit(pattern, Utf16Le) | bit(pattern, Utf16Be);

		}

		/// Any encoding of "boot.wim" (0b000000111).
		constexpr std::uint32_t kMaskBootWim = group(BootWim);

		/// Any encoding of "bootmgr.efi" or "bootmgr" (0b111111000).
		constexpr std::uint32_t kMaskBootMgr = group(BootmgrEfi) | group(Bootmgr);

		/**
		 * @brief Recognizes likely Windows media when boot.wim and either bootmgr
		 * or bootmgr.efi have been found, in any supported encoding.
		 */
		constexpr bool isComplete(std::uint32_t found) {

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
			std::array<int, 256> next{};  ///< Transition per input byte; -1 only while building.
			int                 fail{0};  ///< Longest proper suffix that is also a prefix.
			std::uint32_t       match{0}; ///< Match bits, including those inherited via failure links.

			Node() {

				next.fill(-1);

			}
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
			 * Adds one byte representation of an ASCII filename signature. Searching
			 * all three supported encodings handles common optical-image layouts
			 * without parsing the filesystem.
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
				auto step = [&](std::uint8_t byte) {

					if (trie[cur].next[byte] == -1) {
						trie[cur].next[byte] = static_cast<int>(trie.size());
						trie.emplace_back();
					}
					cur = trie[cur].next[byte];

				};

				for (char raw : text) {
					const std::uint8_t c = fold[static_cast<std::uint8_t>(raw)];
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
			 * Completed transitions allow a single-pass scan without failure-link
			 * traversal. Inherited matches preserve signatures ending at a suffix
			 * of a matched prefix.
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
		std::uint32_t scan(const char* addr, std::size_t size) {

			const auto& ac   = automaton();
			const auto& fold = case_fold::table();

			std::uint32_t found = 0;
			int           cur   = 0;
			for (std::size_t i = 0; i < size; ++i) {
				cur    = ac[cur].next[fold[static_cast<std::uint8_t>(addr[i])]];
				found |= ac[cur].match;
				if (win_signature::isComplete(found)) break;
			}
			return found;

		}

	} // namespace aho_corasick

	//---- Read-only file mapping ---------------------------------
	/**
	 * @brief Maps part of a file into memory for read-only access.
	 * The operating system loads pages on demand. The mapping is released when
	 * this object goes out of scope; check ok() before accessing the data.
	 */
	class ScopedMapping {
	public:
		/**
		 * @brief Maps the first length bytes of an open file without taking ownership
		 * of the file descriptor. The length must not exceed the file size.
		 */
		ScopedMapping(int fd, std::size_t length)
		    : addr_(mmap(nullptr, length, PROT_READ, MAP_PRIVATE, fd, 0)),
		      length_(length) {}

		/// Releases a successful mapping automatically.
		~ScopedMapping() {

			if (ok()) munmap(addr_, length_);

		}

		ScopedMapping(const ScopedMapping&)            = delete;
		ScopedMapping& operator=(const ScopedMapping&) = delete;

		/// Reports whether the file mapping succeeded.
		bool ok() const {

			return addr_ != MAP_FAILED;

		}

		/// Returns the mapped data pointer; valid only if ok() is true.
		const char* data() const {

			return static_cast<const char*>(addr_);

		}

		/// Returns the mapped length in bytes, not necessarily the file size.
		std::size_t size() const {

			return length_;

		}

		/**
		 * @brief Requests read-ahead for a sequential scan.
		 * MADV_WILLNEED requests prefetch of the initial region; MADV_SEQUENTIAL
		 * describes the access pattern for the whole mapping. Both hints are best-effort.
		 */
		void adviseSequentialScan(std::size_t eagerBytes) const {

			madvise(addr_, std::min(eagerBytes, length_), MADV_WILLNEED);
			madvise(addr_, length_, MADV_SEQUENTIAL);

		}

	private:
		void*       addr_;
		std::size_t length_;
	};

} // namespace

namespace iso_detect {

	/**
	 * @brief Recognizes accepted ISO 9660 or UDF volume descriptors.
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
	 * @note Acceptance does not validate the full filesystem or imply bootability.
	 */
	bool isValidIso9660(int fd) {

		using namespace iso_layout;

		for (int i = 0; i < kVrsSectors; ++i) {
			std::uint8_t d[kHeaderSize];
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
	 * @brief Probes an ISO image for Windows installation-media signatures.
	 *
	 * Maps up to the first 32 MiB of the file and checks, in order:
	 *  1. a valid volume header at sector 16 (PVD "CD001", or UDF "NSR02"/"NSR03");
	 *  2. a single-pass Aho-Corasick scan for "boot.wim" and "bootmgr[.efi]",
	 *     case-insensitive, in ASCII, UTF-16LE and UTF-16BE.
	 *
	 * The 32 MiB window is a heuristic: images with signatures outside that
	 * window may not be recognized. The scan exits as soon as both signature
	 * groups have matched; the remainder of the mapped window need not be scanned.
	 *
	 * @param fd       Open, readable descriptor; mapped from offset 0, not closed here.
	 * @param fileSize Image size in bytes (from fstat); used for the minimum-size
	 *                 guard and to clamp the mapping window.
	 * @return true if the header is valid and both signature groups are present.
	 *
	 * @note Read-only; the file is not modified. Matching signatures do not
	 *       establish the Windows version or bootability.
	 */
	bool isWindowsIsoInitialCheck(int fd, off_t fileSize) {

		constexpr off_t       kMinFileSize = 0x9000;
		constexpr std::size_t kMaxWindow   = 32 * 1024 * 1024;
		constexpr std::size_t kEagerBytes  = 4 * 1024 * 1024;

		if (fileSize < kMinFileSize) return false;

		const ScopedMapping map(fd, std::min(static_cast<std::size_t>(fileSize), kMaxWindow));
		if (!map.ok()) return false;

		map.adviseSequentialScan(kEagerBytes);

		if (!iso_layout::hasValidVolumeHeader(map.data(), map.size())) return false;

		return win_signature::isComplete(aho_corasick::scan(map.data(), map.size()));

	}

} // namespace iso_detect
