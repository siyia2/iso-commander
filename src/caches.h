// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef CACHES_H
#define CACHES_H

// C++ Standard Library Headers
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>

// Transparent hasher to allow find(string_view) without allocations
struct StringViewHash {
    using is_transparent = void;
    size_t operator()(std::string_view sv) const {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string& s) const {
        return std::hash<std::string>{}(s);
    }
};

namespace GlobalCaches {
    // Thread-local caches
    inline thread_local std::unordered_map<
        std::string,
        std::string,
        StringViewHash,
        std::equal_to<>
    > transformationCache;
    inline thread_local std::unordered_map<
        std::string,
        std::tuple<std::string, std::string, std::string>,
        StringViewHash,
        std::equal_to<>
    > cachedParsesForUmount;

    inline std::map<std::string, std::string> g_configCache;
	inline std::string g_cachedPath;
}

#endif // CACHES_H
