// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Scope-bound aligned storage for USB I/O.
 */

export module isocmd.write2usb.util.alignedBuffer;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @struct AlignedBuffer
 * @brief Owns an aligned I/O buffer and releases it on scope exit.
 *
 * Allocation size is rounded up to an alignment multiple, while @c size
 * retains the requested capacity. Allocation failure is reported by ok().
 * The buffer is neither copyable nor movable.
 */
struct AlignedBuffer {
	void*       ptr  = nullptr; ///< Allocation owned by this object.
	char*       data = nullptr; ///< Byte-access alias of ptr.
	std::size_t size;           ///< Requested capacity in bytes, before rounding.

	/**
	 * @brief Allocates storage suitable for aligned I/O.
	 * @param size Requested capacity in bytes.
	 * @param alignment Nonzero alignment supported by std::aligned_alloc().
	 * @pre Rounding size up to alignment must not overflow std::size_t.
	 * @note Check ok() before using data; allocation failure leaves both pointers null.
	 */
	AlignedBuffer(std::size_t size, std::size_t alignment) : size(size) {

		std::size_t remainder    = size % alignment;
		std::size_t adjustedSize = (remainder == 0) ? size : (size + alignment - remainder);
		ptr = std::aligned_alloc(alignment, adjustedSize);
		if (ptr) {
			data = static_cast<char*>(ptr);
		}

	}

	AlignedBuffer(const AlignedBuffer&)            = delete;
	AlignedBuffer& operator=(const AlignedBuffer&) = delete;
	AlignedBuffer(AlignedBuffer&&)                 = delete;
	AlignedBuffer& operator=(AlignedBuffer&&)      = delete;

	/// Frees the allocation, including a null pointer after allocation failure.
	~AlignedBuffer() {

		std::free(ptr);

	}

	/// Reports whether allocation succeeded.
	bool ok() const {

		return data != nullptr;

	}

	/// Allows contextual Boolean checks of allocation success.
	explicit operator bool() const {

		return ok();

	}
};

} // extern "C++"
