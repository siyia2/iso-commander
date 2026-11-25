// SPDX-License-Identifier: GPL-3.0-or-later
#include "../common/testHarness.h"

import isocmd.filter.filter;
import isocmd.util.stringManipulation;

namespace isocmd_tests {

void filterGrammar() {
    const std::vector<std::string> files{
        "Ubuntu-22.iso", "ubuntu-24.iso", "Fedora boot.iso", "Fedora.iso",
        "a&b.iso", "a;b.iso", "!bang.iso", "path\\name.iso", "Ubuntu boot.iso"};
    const auto match = [&](const std::string& q, Indices expected) {
        CHECK(filterFilesIndices(files, q) == expected);
        auto lower = files;
        for (auto& f : lower) toLowerInPlace(f);
        CHECK(filterFilesIndices(files, q, &lower) == expected);
        const std::vector<std::string> wrongSize{"not the cache"};
        CHECK(filterFilesIndices(files, q, &wrongSize) == expected);
    };
    match("ubuntu", {0,1,8});
    match("Ubuntu", {0,8});
    match("ubuntu&24;fedora", {1,2,3});
    match("ubuntu;fedora&!boot", {0,1,3});
    match("!boot", {0,1,3,4,5,6,7});
    match("a\\&b", {4});
    match("a\\;b", {5});
    match("\\!bang", {6});
    match("path\\\\name", {7});
    match("ubuntu&&24;", {1});
    match(";!&", {0,1,2,3,4,5,6,7,8});
    match("", {});
    match("nonexistent", {});
    CHECK(filterFilesIndices({}, "ubuntu").empty());
    CHECK(filterFilesIndicesSubset(files, {8,1,3,0}, "ubuntu", nullptr) == Indices({8,1,0}));
    bool trailing = false;
    buildQueryGroups("ubuntu;", &trailing);
    CHECK(trailing);
    // A pending negation counts as a started branch in the existing parser.
    buildQueryGroups("ubuntu;!", &trailing);
    CHECK(!trailing);
    buildQueryGroups("ubuntu;;fedora", &trailing);
    CHECK(!trailing);
}

void filterRandom() {
    // Independent scalar oracle for simple terms, including SIMD boundary lengths.
    std::mt19937 rng(0x49534f);
    std::vector<std::string> files;
    for (int i = 0; i < 2048; ++i) {
        std::string s;
        for (unsigned n = rng() % 100; n; --n) s += "aBcDeF"[rng() % 6];
        files.push_back(std::move(s));
    }
    for (int i = 0; i < 180; ++i) {
        std::string q;
        for (unsigned n = 1 + rng() % 36; n; --n) q += "abcdef"[rng() % 6];
        if (i % 2 == 0) {
            const auto& s = files[rng() % files.size()];
            if (!s.empty()) q = s.substr(rng() % s.size(), 1 + rng() % 35);
        }
        bool smart = std::any_of(q.begin(), q.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
        Indices expected;
        for (std::size_t j = 0; j < files.size(); ++j) {
            auto text = files[j];
            if (!smart) for (auto& c : text) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
            if (text.find(q) != std::string::npos) expected.push_back(j);
        }
        CHECK(filterFilesIndices(files, q) == expected);
    }
}

} // namespace isocmd_tests
