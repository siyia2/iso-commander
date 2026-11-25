// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.core.caches;

import std;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- core/caches ----

// Transparent hasher to allow find(string_view) without allocations
struct StringViewHash {
	using is_transparent = void;

	std::size_t operator()(std::string_view sv) const {

		return std::hash<std::string_view>{}(sv);

	}

	std::size_t operator()(const std::string& s) const {

		return std::hash<std::string>{}(s);

	}
};

namespace GlobalCaches {
	// Thread-local caches
	extern thread_local std::unordered_map<
	    std::string,
	    std::string,
	    StringViewHash,
	    std::equal_to<>
	> transformationCache;
	extern thread_local std::unordered_map<
	    std::string,
	    std::tuple<std::string, std::string, std::string>,
	    StringViewHash,
	    std::equal_to<>
	> cachedParsesForUmount;
}

} // extern "C++"
