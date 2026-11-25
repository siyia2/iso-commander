// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.alignedBuffer;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/util/alignedBuffer ----

/**
 * @struct AlignedBuffer
 * @brief RAII wrapper for an aligned I/O buffer (std::aligned_alloc).
 */
struct AlignedBuffer {
	void*  ptr  = nullptr;
	char*  data = nullptr;
	std::size_t size;

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

	~AlignedBuffer() {

		std::free(ptr);

	}

	bool ok() const {

		return data != nullptr;

	}

	explicit operator bool() const {

		return ok();

	}
};

} // extern "C++"
