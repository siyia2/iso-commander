// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Checks real file operations, failure cleanup and directory traversal on temporary fixtures.
 */

#include "../common/testHarness.h"

import isocmd.ops.cpMvRm;
import isocmd.database.databaseOps;

namespace isocmd_tests {

	static void checkCopyFailures(const fs::path& directory, const fs::path& src, const fs::path& dst, const std::string& payload, std::atomic<std::size_t>& bytes) {

		std::error_code ec;
		CHECK(!bufferedCopyWithProgress(directory/"missing", dst, &bytes, ec) && ec);
		CHECK(!fs::exists(dst) && !fs::exists(dst.string()+".part"));
		// Rename failure after copying must preserve both source and existing directory.
		fs::create_directory(dst); writeFile(dst/"keep", "sentinel"); ec.clear();
		CHECK(!bufferedCopyWithProgress(src, dst, nullptr, ec) && ec);
		CHECK(readFile(src) == payload && readFile(dst/"keep") == "sentinel" && !fs::exists(dst.string()+".part"));

	}

	static void checkMoveAndDelete(OperationContext& ctx, const fs::path& dst, const fs::path& moved, const std::string& payload, const std::atomic<std::size_t>& done, const std::atomic<std::size_t>& failed, const std::atomic<std::size_t>& bytes, const std::atomic<bool>& ok) {

		CHECK(performMoveOperation(ctx, {dst, moved, "test", "copy.iso", "test/", "moved.iso"}, payload.size()));
		CHECK(!fs::exists(dst) && readFile(moved) == payload && done == 2 && bytes == 2*payload.size());
		performDeleteOperation(ctx, moved, "test", "moved.iso", payload.size());
		CHECK(!fs::exists(moved) && done == 3 && bytes == 3*payload.size());
		performDeleteOperation(ctx, moved, "test", "moved.iso", payload.size());
		CHECK(failed == 1 && !ok && bytes == 3*payload.size());

	}

	void filesystemOperations() {

		TempDir tmp;
		const auto src = tmp.path / "source.iso", dst = tmp.path / "copy.iso", moved = tmp.path / "moved.iso";
		std::string payload(4*1024*1024+37, '\0');
		for (std::size_t i = 0; i<payload.size(); ++i) payload[i] = static_cast<char>(i % 251);
		writeFile(src, payload); writeFile(dst, "old");
		std::unordered_set<std::string> successes, errors; std::mutex mutex;
		BatchReporter reporter(successes, errors, mutex, 2);
		std::atomic<std::size_t> done{0}, failed{0}, bytes{0}; std::atomic<bool> ok{true};
		int ownershipCalls = 0;
		OperationContext ctx{reporter, &done, &failed, ok, [&](const fs::path&) {++ownershipCalls;}, &bytes};
		CHECK(performBufferedCopyOperation(ctx, {src, dst, "test", "source.iso", "test/", "copy.iso"}));
		CHECK(readFile(src) == payload && readFile(dst) == payload && !fs::exists(dst.string()+".part"));
		CHECK(done == 1 && failed == 0 && bytes == payload.size() && ownershipCalls == 1 && ok);
		checkMoveAndDelete(ctx, dst, moved, payload, done, failed, bytes, ok);
		reporter.flush(); CHECK(successes.size() == 3 && errors.size() == 1 && ownershipCalls == 2);
		CHECK(errors.begin()->contains(": File not found") && !errors.begin()->contains('\n'));
		checkCopyFailures(tmp.path, src, dst, payload, bytes);

	}

	void directoryTraversal() {

		TempDir tmp; fs::create_directories(tmp.path/"nested/deeper");
		writeFile(tmp.path/"a.ISO", "a"); writeFile(tmp.path/"ignore.txt", "x");
		writeFile(tmp.path/"nested/b.iso", "b"); writeFile(tmp.path/"nested/deeper/c.iso", "c");
		fs::create_directory_symlink(tmp.path/"nested", tmp.path/"alias");
		CHECK(isValidDirectory(tmp.path.string()) && !isValidDirectory((tmp.path/"a.ISO").string()));
		for (int depth : {0, 1, -1}) {
			std::vector<std::string> files; std::unordered_set<std::string> errors;
			std::atomic<std::size_t> total{0}; std::mutex fm, em;
			traverse(tmp.path, files, errors, total, fm, em, depth, false);
			std::vector<std::string> expected{(tmp.path/"a.ISO").string()};
			if (depth != 0) expected.push_back((tmp.path/"nested/b.iso").string());
			if (depth == -1) expected.push_back((tmp.path/"nested/deeper/c.iso").string());
			std::sort(files.begin(), files.end()); std::sort(expected.begin(), expected.end());
			CHECK(files == expected && errors.empty());
		}

	}
}
