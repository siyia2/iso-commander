// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.alignedBuffer;

import std;

// C++ linkage keeps declarations compatible with definitions in the .cpp files.
export extern "C++" {

/**
 * @struct AlignedBuffer
 * @brief scope-bound wrapper for an aligned I/O buffer (std::aligned_alloc).
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
