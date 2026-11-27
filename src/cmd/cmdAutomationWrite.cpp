// SPDX-License-Identifier: GPL-3.0-or-later

#include <unistd.h>

import std;
import isocmd.cmd.cmdAutomation;
import isocmd.core.cancellation;
import isocmd.input.terminalControl;
import isocmd.settings.settings;
import isocmd.threadpool.threadpool;
import isocmd.ui.display.themes;
import isocmd.write2usb.util.drainingState;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;

namespace fs = std::filesystem;

// ─── Write-to-USB branch (w2usb) ────────────────────────────────────────────

/**
 * @brief Validates root privileges and partitions command-line arguments into ISOs and devices.
 *
 * Checks that the current process has root privileges (`geteuid() == 0`) and separates
 * input paths starting with `/dev/` into device arguments and all other paths into ISO arguments.
 * Validates that at least one ISO and one target device are specified, and enforces
 * that either a single ISO is used for all devices or counts match one-to-one.
 *
 * @param args Parsed command-line arguments containing paths and execution flags.
 * @param wt The active write theme configuration for styling error messages.
 * @param[out] isoArgs Output vector populated with extracted ISO file paths.
 * @param[out] devArgs Output vector populated with extracted target device paths.
 * @return true if all prerequisite and argument count checks pass; false otherwise.
 */
static bool validatePrerequisitesAndArgs(const ParsedArgs& args, const WriteTheme& wt,
                                       std::vector<std::string>& isoArgs,
                                       std::vector<std::string>& devArgs) {

	using namespace WriteValidation;

	if (geteuid() != 0) {
		IsoCli::errMsg("Root privileges required for writing ISOs to USB devices.");
		return false;
	}

	for (const auto& a : args.paths) {
		(a.rfind("/dev/", 0) == 0 ? devArgs : isoArgs).push_back(a);
	}

	if (isoArgs.empty()) {
		printValidationErrors(wt, {plainErr(wt, "No ISO files specified")});
		return false;
	}
	if (devArgs.empty()) {
		printValidationErrors(wt, {plainErr(wt, "No target devices specified")});
		return false;
	}
	if (isoArgs.size() != 1 && isoArgs.size() != devArgs.size()) {
		printValidationErrors(wt, {plainErr(wt,
			"Got " + std::to_string(isoArgs.size()) + " ISOs and " +
			std::to_string(devArgs.size()) +
			" devices; use 1 ISO for all devices, or one device per ISO")});
		return false;
	}
	return true;

}

/**
 * @brief Validates a collection of ISO file paths and populates their metadata.
 *
 * Iterates through raw ISO path arguments, verifying that each entry points to a
 * valid regular file with a `.iso` extension, is non-empty, and is readable.
 * Successfully validated files are converted into structured `IsoInfo` objects.
 *
 * @param isoArgs Vector of raw ISO file path strings from command-line arguments.
 * @param wt The active write theme configuration for styling error messages.
 * @param[out] errors Collection accumulating formatted validation error strings.
 * @return std::vector<IsoInfo> A vector containing metadata for all successfully validated ISOs.
 */
static std::vector<IsoInfo> validateIsos(const std::vector<std::string>& isoArgs,
                                          const WriteTheme& wt,
                                          std::vector<std::string>& errors) {

	using namespace WriteValidation;

	std::vector<IsoInfo> isos;
	for (const auto& raw : isoArgs) {
		std::error_code ec;
		fs::path p(raw);
		if (!fs::is_regular_file(p, ec)) {
			errors.push_back(pathErr(wt, raw, " is not a regular file"));
			continue;
		}
		std::string ext = p.extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), static_cast<int (*)(int)>(std::tolower));
		if (ext != ".iso") {
			errors.push_back(pathErr(wt, raw, " is not an ISO file"));
			continue;
		}
		const auto size = fs::file_size(p, ec);
		if (ec || size == 0) {
			errors.push_back(pathErr(wt, raw, " is empty or unreadable"));
			continue;
		}
		const fs::path canon = fs::canonical(p, ec);
		const std::string full = ec ? p.string() : canon.string();
		isos.push_back(IsoInfo{full, fs::path(full).filename().string(), size,
			                   formatFileSize(size)});
	}
	return isos;

}

/**
 * @brief Validates target storage devices for write operations.
 *
 * Resolves canonical device paths, checks for duplicate device entries in the argument list,
 * and delegates device-specific checks via
 * @ref WriteValidation::checkDevice.
 *
 * @param devArgs Vector of raw target device path strings.
 * @param wt The active write theme configuration for styling error messages.
 * @param[out] errors Collection accumulating formatted validation error strings.
 * @param[out] devices Output vector populated with validated canonical device paths.
 * @param[out] deviceSizes Output vector populated with the capacities (in bytes) of validated devices.
 */
static void validateDevices(const std::vector<std::string>& devArgs,
                            const WriteTheme& wt,
                            std::vector<std::string>& errors,
                            std::vector<std::string>& devices,
                            std::vector<std::uint64_t>& deviceSizes) {

	using namespace WriteValidation;
	std::unordered_set<std::string> seen;
	for (const auto& raw : devArgs) {
		std::error_code ec;
		const fs::path canon = fs::canonical(raw, ec);
		const std::string dev = ec ? raw : canon.string();

		if (!seen.insert(dev).second) {
			errors.push_back(pathErr(wt, raw, " is listed more than once"));
			continue;
		}
		std::uint64_t size = 0;
		if (auto err = checkDevice(wt, dev, raw, size)) {
			errors.push_back(std::move(*err));
			continue;
		}
		devices.push_back(dev);
		deviceSizes.push_back(size);
	}

}

/**
 * @brief Executes silent write operations in asynchronous worker threads.
 *
 * Disables standard input and dispatches parallel tasks to the static thread pool,
 * managing futures and tracking completion/failure statuses.
 *
 * @param pairsPtr Shared pointer containing paired ISO metadata and target device paths.
 * @param progressDataPtr Shared pointer to the progress state tracking vector.
 * @param pool Reference to the static thread pool used for concurrent writes.
 */
static void executeSilentWriteOperations(
    std::shared_ptr<std::vector<std::pair<IsoInfo, std::string>>> pairsPtr,
    std::shared_ptr<std::vector<ProgressInfo>> progressDataPtr,
    ThreadPool& pool) {

	disableInput();

	std::vector<std::future<void>> futures;
	futures.reserve(pairsPtr->size());

	for (std::size_t i = 0; i < pairsPtr->size(); ++i) {
		futures.emplace_back(pool.enqueue(
		    [pairsPtr, progressDataPtr, i]() {

				const auto& [iso, device] = (*pairsPtr)[i];

				const bool success = writeIsoToDevice(
				    iso.path, device, i, *progressDataPtr);

				auto& progress = (*progressDataPtr)[i];

				if (success) {
					progress.completed.store(true);
				} else if (!GlobalState::g_operationCancelled.load()) {
					progress.failed.store(true);
				}

			}));
	}

	for (auto& future : futures) {
		if (future.valid())
		    future.wait();
	}

}

/**
 * @brief Collects capacity validation errors across all target devices.
 *
 * Pairs each target device with either the single supplied ISO or the corresponding
 * positional ISO, then appends any error returned by `WriteValidation::checkCapacity`.
 *
 * @param wt Active `write2usb` UI theme used for formatting error messages.
 * @param isos Validated ISO metadata entries (must have size 1 or match `devices.size()`).
 * @param devices Validated target device paths.
 * @param deviceSizes Capacity in bytes corresponding to each entry in `devices`.
 * @param[in,out] errors Error list to which capacity check failures are appended.
 */
static void collectCapacityErrors(const WriteTheme& wt,
                                  const std::vector<IsoInfo>& isos,
                                  const std::vector<std::string>& devices,
                                  const std::vector<std::uint64_t>& deviceSizes,
                                  std::vector<std::string>& errors) {

	for (std::size_t d = 0; d < devices.size(); ++d) {
		const IsoInfo& iso = (isos.size() == 1) ? isos[0] : isos[d];
		if (auto err = WriteValidation::checkCapacity(wt, iso, devices[d], deviceSizes[d])) {
			errors.push_back(std::move(*err));
		}
	}

}

/**
 * @brief Builds ISO-to-device write pairs from validated ISOs and devices.
 *
 * Broadcasts a single ISO across all target devices when `isos.size() == 1`;
 * otherwise pairs `isos[d]` with `devices[d]` in order.
 *
 * @param isos Validated ISO metadata entries.
 * @param devices Validated target device paths.
 * @return std::vector<std::pair<IsoInfo, std::string>> Ordered list of write pairs.
 */
static std::vector<std::pair<IsoInfo, std::string>> buildWritePairs(
    const std::vector<IsoInfo>& isos,
    const std::vector<std::string>& devices) {

	std::vector<std::pair<IsoInfo, std::string>> pairs;
	pairs.reserve(devices.size());
	for (std::size_t d = 0; d < devices.size(); ++d) {
		pairs.emplace_back(isos.size() == 1 ? isos[0] : isos[d], devices[d]);
	}
	return pairs;

}

/**
 * @brief Allocates and initializes progress tracking records for each write pair.
 *
 * @param pairs ISO-to-device pairs scheduled for writing.
 * @return std::shared_ptr<std::vector<ProgressInfo>> Shared progress vector passed to worker execution.
 */
static std::shared_ptr<std::vector<ProgressInfo>> buildProgressData(
    const std::vector<std::pair<IsoInfo, std::string>>& pairs) {

	auto progressDataPtr = std::make_shared<std::vector<ProgressInfo>>();
	progressDataPtr->reserve(pairs.size());
	for (const auto& [iso, device] : pairs) {
		progressDataPtr->emplace_back(iso.filename, device, iso.sizeStr, iso.path);
	}
	return progressDataPtr;

}

/**
 * @brief Verifies that requested write jobs fit within the thread pool capacity.
 *
 * Prints the formatted `write2usb` selection limit error to `std::cerr` if
 * `jobCount` exceeds `pool.threadCount()`.
 *
 * @param wt Active `write2usb` UI theme used for diagnostic formatting.
 * @param jobCount Number of write pairs to execute concurrently.
 * @param pool Worker thread pool whose capacity limits parallel jobs.
 * @return bool `true` if `jobCount` is within `pool.threadCount()`; `false` otherwise.
 */
static bool checkThreadPoolLimit(const WriteTheme& wt,
                                 std::size_t jobCount,
                                 const ThreadPool& pool) {

	if (jobCount <= pool.threadCount()) {
		return true;
	}

	std::cerr << "\n" << wt.colorFailure << "Error: "
	          << UI::Palette::Yellow << "write2usb"
	          << wt.colorFailure << " selection limit is "
	          << wt.colorWarning << pool.threadCount()
	          << wt.colorFailure << " (thread pool size)."
	          << wt.editorResetCol << "\n\n";
	return false;

}

/**
 * @brief Counts how many write jobs reported completion and returns the process exit code.
 *
 * @param progressData Progress records updated by worker threads.
 * @return int `0` if every job completed and operation was not cancelled; `1` otherwise.
 */
static int evaluateWriteResult(const std::vector<ProgressInfo>& progressData) {

	std::size_t ok = 0;
	for (const auto& progress : progressData) {
		if (progress.completed.load()) {
			++ok;
		}
	}

	return (ok == progressData.size() &&
	        !GlobalState::g_operationCancelled.load()) ? 0 : 1;

}

/**
 * @brief Non-interactive ISO -> USB writer orchestration function.
 *
 * Arguments starting with `/dev/` are treated as target devices; all others are treated as ISOs.
 * A single ISO can be written to multiple devices, or ISO and device counts must match
 * and are paired in argument order.
 *
 * Every validation error is collected and reported (using the write2usb UI theme) before
 * any hardware is touched. Per device, this includes checks in @ref WriteValidation::checkDevice
 * and capacity checks in @ref WriteValidation::checkCapacity.
 *
 * @param args Parsed command-line arguments containing target devices, ISO paths, and execution flags.
 * @return int Returns `0` if all write operations completed successfully; `1` on validation failure,
 *             write failure, thread pool capacity overflow, or user cancellation.
 */
int handleWriteUsb(const ParsedArgs& args) {

	using namespace WriteValidation;
	const WriteTheme wt = getWriteTheme();

	std::vector<std::string> isoArgs, devArgs;
	if (!validatePrerequisitesAndArgs(args, wt, isoArgs, devArgs)) {
		return 1;
	}

	std::vector<std::string> errors;
	std::vector<IsoInfo> isos = validateIsos(isoArgs, wt, errors);

	std::vector<std::string> devices;
	std::vector<std::uint64_t> deviceSizes;
	validateDevices(devArgs, wt, errors, devices, deviceSizes);

	if (errors.empty()) {
		collectCapacityErrors(wt, isos, devices, deviceSizes, errors);
	}

	if (!errors.empty()) {
		// Deduplicate diagnostics only; repeated ISO arguments and duplicate
		// target validation must retain their original mapping semantics.
		std::unordered_set<std::string> seenErrors;
		std::erase_if(errors, [&seenErrors](const auto& error) {
			return !seenErrors.insert(error).second;
		});
		printValidationErrors(wt, errors);
		return 1;
	}

	ThreadPool& pool = getStaticThreadPool();
	auto pairsPtr = std::make_shared<std::vector<std::pair<IsoInfo, std::string>>>(
	    buildWritePairs(isos, devices));

	if (!checkThreadPoolLimit(wt, pairsPtr->size(), pool)) {
		return 1;
	}

	if (!args.silentMode) {
		bool allSucceeded = false;
		performWriteOperation(*pairsPtr, &allSucceeded, args.verifyUsb);
		g_drainingManager.joinAll();
		std::cout << "\n";
		return (allSucceeded && !GlobalState::g_operationCancelled.load()) ? 0 : 1;
	}

	auto progressDataPtr = buildProgressData(*pairsPtr);
	const bool buffered = write2usbBufferedEnabled();
	for (auto& row : *progressDataPtr) {
		row.verifyRequested = args.verifyUsb;
		row.bufferedWrites = buffered;
	}
	executeSilentWriteOperations(pairsPtr, progressDataPtr, pool);
	g_drainingManager.joinAll();
	return evaluateWriteResult(*progressDataPtr);

}
