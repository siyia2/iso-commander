// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks filter grammar and compares randomized results with a scalar reference.
 */

#include "../common/testHarness.h"

import isocmd.filter.filter;
import isocmd.util.stringManipulation;

namespace isocmd_tests {

	/**
	 * @brief Checks deterministic filter grammar and matching cases.
	 */
	void filterGrammar() {

		const std::vector<std::string> files{
			"Ubuntu-22.iso", "ubuntu-24.iso", "Fedora boot.iso", "Fedora.iso",
			"a&b.iso", "a;b.iso", "!bang.iso", "path\\name.iso", "Ubuntu boot.iso"};
		std::vector<std::string_view> views(files.begin(), files.end());
		const auto match = [&](const std::string& q, Indices expected) {

			CHECK(filterFilesIndices(files, q) == expected);
			CHECK(filterViewIndices(views, q) == expected);
			auto lower = files;
			for (auto& f : lower) toLowerInPlace(f);
			CHECK(filterFilesIndices(files, q, &lower) == expected);
			CHECK(filterViewIndices(views, q, &lower) == expected);
			const std::vector<std::string> wrongSize{"not the cache"};
			CHECK(filterFilesIndices(files, q, &wrongSize) == expected);
			CHECK(filterViewIndices(views, q, &wrongSize) == expected);

		};
		match("ubuntu", {0, 1, 8});
		match("Ubuntu", {0, 8});
		match("ubuntu&24;fedora", {1, 2, 3});
		match("ubuntu;fedora&!boot", {0, 1, 3});
		match("!boot", {0, 1, 3, 4, 5, 6, 7});
		match("a\\&b", {4});
		match("a\\;b", {5});
		match("\\!bang", {6});
		match("path\\\\name", {7});
		match("ubuntu&&24;", {1});
		match(";!&", {0, 1, 2, 3, 4, 5, 6, 7, 8});
		match("", {});
		match("nonexistent", {});
		CHECK(filterFilesIndices({}, "ubuntu").empty());
		CHECK(filterFilesIndicesSubset(files, {8, 1, 3, 0}, "ubuntu", nullptr) == Indices({8, 1, 0}));
		CHECK(filterViewIndicesSubset(views, {8, 1, 3, 0}, "ubuntu", nullptr) == Indices({8, 1, 0}));
		auto lower = files;
		for (auto& f : lower) toLowerInPlace(f);
		CHECK(filterViewIndicesSubset(views, {8, 1, 3, 0}, "ubuntu", &lower) == Indices({8, 1, 0}));

		// Every possible byte must retain the active locale's original mapping.
		std::string bytes;
		for (int i = 0; i < 256; ++i) bytes.push_back(static_cast<char>(i));
		auto expectedLower = bytes;
		for (auto& c : expectedLower)
		    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		toLowerInPlace(bytes);
		CHECK(bytes == expectedLower);

		// Borrowed projections must preserve the exact previous label rules.
		CHECK(filterTargetView("/a~b/Game~old~hash", false, true) == "Game~old");
		CHECK(filterTargetView("/a~b/Game~old~hash", true) == "Game~old~hash");
		CHECK(filterTargetView("/a~b/Game~old~hash", false) == "/a~b/Game~old~hash");
		CHECK(filterTargetView("plain.iso", true) == "plain.iso");
		CHECK(filterTargetView("/a/", true).empty());
		CHECK(filterTargetView("~hash", false, true).empty());

		// No-match and all-match commits must leave an existing view untouched.
		const std::vector<std::string> source{"/iso/Alpha.iso", "/iso/beta.iso", "/iso/gamma.iso"};
		std::vector<std::string> displayed{source[1]};
		bool filtered = true, clear = false, history = false;
		std::size_t page = 2;
		FilterContext ctx{displayed, filtered, clear, history, page, &source};
		filteringStack = {FilteringState{{1}, "beta", true, false}};
		CHECK(!applyFilterCore("unmatched", ctx));
		CHECK(applyFilterCore("iso", ctx));
		CHECK(displayed == std::vector<std::string>{source[1]});
		CHECK(filtered && !clear && page == 2 && filteringStack.size() == 1);
		CHECK(filteringStack.back().originalIndices == Indices{1});
		CHECK(filteringStack.back().query == "beta");
		CHECK(applyFilterCore("gamma", ctx));
		CHECK(displayed == std::vector<std::string>{source[2]});
		CHECK(filtered && clear && page == 0);
		CHECK(filteringStack.back().originalIndices == Indices{2});
		filteringStack.clear();

		bool trailing = false;
		buildQueryGroups("ubuntu;", &trailing);
		CHECK(trailing);
		// A pending negation counts as a started branch in the existing parser.
		buildQueryGroups("ubuntu;!", &trailing);
		CHECK(!trailing);
		buildQueryGroups("ubuntu;;fedora", &trailing);
		CHECK(!trailing);

	}

	/**
	 * @brief Compares randomized filter results with a scalar reference implementation.
	 */
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
