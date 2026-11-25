// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

import std;

namespace isocmd_tests {
namespace fs = std::filesystem;
using Indices = std::vector<std::size_t>;

// Unlike assert(), this remains active in release builds.
#define CHECK(...) do { if (!(__VA_ARGS__)) throw std::runtime_error( \
    std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " #__VA_ARGS__); } while (false)

struct TempDir {
    fs::path path;
    TempDir() {
        auto pattern = (fs::temp_directory_path() / "isocmd-unit-XXXXXX").string();
        const char* p = ::mkdtemp(pattern.data());
        if (!p) throw std::runtime_error("mkdtemp failed");
        path = p;
    }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};
struct ReadFd {
    int fd;
    explicit ReadFd(const fs::path& p) : fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC)) {
        CHECK(fd >= 0);
    }
    ~ReadFd() { ::close(fd); }
    ReadFd(const ReadFd&) = delete;
    ReadFd& operator=(const ReadFd&) = delete;
};

using Test = std::pair<const char*, void(*)()>;
inline int runTests(std::string_view suite, std::initializer_list<Test> tests) {
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try { test(); std::cout << "PASS: " << name << std::endl; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL: " << name << ": " << e.what() << '\n'; }
        catch (...) { ++failures; std::cerr << "FAIL: " << name << ": unknown exception\n"; }
    }
    std::cout << tests.size() - failures << '/' << tests.size() << ' ' << suite << " groups passed\n";
    return failures ? 1 : 0;
}
inline void writeFile(const fs::path& path, std::string_view data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.close(); CHECK(out.good());
}
inline std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    CHECK(in.good());
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace isocmd_tests
