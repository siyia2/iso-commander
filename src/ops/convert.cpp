// SPDX-License-Identifier: GPL-3.0-or-later

// C / POSIX Headers
#include <sys/types.h>
#include <unistd.h>

import std;
import isocmd.core.cancellation;
import isocmd.core.globalMutexes;
import isocmd.core.lists;
import isocmd.ops.convert;
import isocmd.ui.display.display;
import isocmd.ui.display.themes;
import isocmd.ui.info.verbose;
import isocmd.util.stringManipulation;

/**
 * @brief Checks whether a filesystem entry exists at the given path.
 *
 * @param fullPath Absolute or relative path to inspect.
 * @return @c true if the path exists (file, directory, or symlink target);
 *         @c false otherwise.
 *
 * @note This is a thin wrapper over `std::filesystem::exists` kept for
 *       call-site readability.
 */
static bool fileExists(const std::string& fullPath) {

	return std::filesystem::exists(fullPath);

}

/**
 * @brief Resolves the real (effective-at-login) user and group identity.
 *
 * Implemented in another translation unit; declared here so the batch driver
 * can stamp produced ISO files with the invoking user's ownership rather than
 * the daemon's.
 *
 * @param[out] real_uid         Receives the real user ID.
 * @param[out] real_gid         Receives the real group ID.
 * @param[out] real_username    Receives the real user name, when available.
 * @param[out] real_groupname   Receives the real group name, when available.
 */
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname);

namespace {

	/// Outcome of validating a source image file before conversion.
	enum class ValidationResult {
		Valid,    ///< File exists and can be opened for reading.
		Missing,  ///< No filesystem entry at the requested path.
		NoAccess  ///< Entry exists but could not be opened (permissions, I/O error).
	};

	/**
	 * @brief Per-batch state shared by every file in a `convertToISO()` run.
	 *
	 * Aggregates configuration, atomic counters, optional output collection,
	 * and the message-batching machinery so that helper functions need only a
	 * single context argument instead of a dozen positional parameters.
	 *
	 * Kept in an anonymous namespace because it depends on project globals
	 * (`verboseSets`, `GlobalMutexes`, `GlobalState`) and would otherwise
	 * leak into the public conversion module interface.
	 *
	 * @warning The atomic-counter pointers and `outPathsMutex` must outlive
	 *          this context. The batch driver guarantees that by holding them
	 *          as caller-supplied parameters for the duration of the run.
	 */
	struct ConversionBatchContext {
		// Configuration
		ConversionThemeStrings themes; ///< Localized label/path strings used in log messages.
		ConversionMode mode;           ///< Which backend to invoke for each job.

		// File ownership
		uid_t real_uid = 0; ///< Real UID applied to produced ISO files via `chown`.
		gid_t real_gid = 0; ///< Real GID applied to produced ISO files via `chown`.

		// Atomic counters
		std::atomic<std::size_t>* completedBytes = nullptr; ///< Incremented by bytes processed (may be null).
		std::atomic<std::size_t>* completedTasks = nullptr; ///< Incremented per success (must be non-null).
		std::atomic<std::size_t>* failedTasks    = nullptr; ///< Incremented per failure (must be non-null).
		std::atomic<std::size_t>* skippedTasks   = nullptr; ///< Incremented per skip (must be non-null).

		// Optional output collection
		std::vector<std::string>* successfulOutputPaths = nullptr; ///< Optional sink for produced ISO paths.
		std::mutex* outPathsMutex = nullptr;                       ///< Guards @ref successfulOutputPaths when set.

		// Message batching
		std::vector<std::string> localSuccessMsgs; ///< Buffered success lines awaiting flush.
		std::vector<std::string> localFailedMsgs;  ///< Buffered failure lines awaiting flush.
		std::vector<std::string> localSkippedMsgs; ///< Buffered skip lines awaiting flush.
		std::size_t batchSize = 50;                     ///< Flush threshold per message class.

		/**
		 * @brief Flushes all buffered messages into the shared verbose sets.
		 *
		 * Takes `GlobalMutexes::globalSetsMutex` once and inserts every
		 * pending message, then clears the local buffers. Safe to call at
		 * any time; a no-op when all buffers are empty.
		 */
		void flushMessages() {

			std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
			if (!localSuccessMsgs.empty()) {
				verboseSets.operationCompleted.insert(localSuccessMsgs.begin(), localSuccessMsgs.end());
				localSuccessMsgs.clear();
			}
			if (!localFailedMsgs.empty()) {
				verboseSets.operationFailed.insert(localFailedMsgs.begin(), localFailedMsgs.end());
				localFailedMsgs.clear();
			}
			if (!localSkippedMsgs.empty()) {
				verboseSets.operationSkipped.insert(localSkippedMsgs.begin(), localSkippedMsgs.end());
				localSkippedMsgs.clear();
			}

		}

		/**
		 * @brief Flushes all buffers when the supplied class has reached the batch threshold.
		 *
		 * @param msgs The just-appended message class (must be one of the
		 *             @c local*Msgs members).
		 */
		void flushIfFull(const std::vector<std::string>& msgs) {

			if (msgs.size() >= batchSize) flushMessages();

		}
	};

	/**
	 * @brief Per-file scratch data: source/target paths plus display strings.
	 *
	 * Lifetime is exactly one loop iteration. `fileNameOnly` is stored as an
	 * owned `std::string` so views taken from `inputPath` remain valid for the
	 * duration of the iteration.
	 */
	struct ConversionFileContext {
		std::string inputPath;    ///< Source image path (as supplied by the caller).
		std::string outputPath;   ///< Derived or supplied destination ISO path.
		std::string displayPath;  ///< Formatted path used in user-facing messages.
		std::string fileNameOnly; ///< Basename including extension; used for type detection.
	};

	/**
	 * @brief Validates that a source image exists and is readable.
	 *
	 * @param inputPath Absolute or relative path to the candidate image file.
	 * @return `ValidationResult::Valid` when the file exists and can be opened
	 *         for reading; `ValidationResult::Missing` when no entry exists;
	 *         `ValidationResult::NoAccess` when the stream cannot be opened.
	 */
	ValidationResult validateImageFile(const std::string& inputPath) {

		if (!std::filesystem::exists(inputPath)) {
			return ValidationResult::Missing;
		}
		if (!std::ifstream(inputPath)) {
			return ValidationResult::NoAccess;
		}
		return ValidationResult::Valid;

	}

	/**
	 * @brief Removes an invalid or missing image path from the active format cache.
	 *
	 * Selects the cache matching the batch's active mode flag and erases all
	 * occurrences of @p inputPath. Locking is handled internally via
	 * `GlobalMutexes::globalSetsMutex`.
	 *
	 * @param inputPath Path to erase from the cache.
	 * @param ctx       Batch context providing the active `ConversionMode`.
	 *
	 * @note When multiple mode flags are set (which callers should not do),
	 *       the MDF cache wins, then NRG, CHD, DAA, and finally BIN/IMG
	 *       (same precedence as `runConversion`).
	 */
	void removeFromCache(const std::string& inputPath, const ConversionBatchContext& ctx) {

		std::lock_guard<std::mutex> lock(GlobalMutexes::globalSetsMutex);
		auto& cache = ctx.mode.mdf ? GlobalState::mdfMdsFilesCache :
		              (ctx.mode.nrg ? GlobalState::nrgFilesCache :
		               (ctx.mode.chd ? GlobalState::chdFilesCache :
		                (ctx.mode.daa ? GlobalState::daaGbiFilesCache : GlobalState::binImgFilesCache)));
		cache.erase(std::remove(cache.begin(), cache.end(), inputPath), cache.end());

	}

	/**
	 * @brief Dispatches a single conversion to the backend selected by the batch mode.
	 *
	 * @param file Per-file paths (input and output) to convert.
	 * @param ctx  Batch context supplying mode flags and the completed-bytes counter.
	 * @return @c true when the selected backend reports success; @c false otherwise.
	 *
	 * @note Falls through to the CCD backend when no mode flag is set, matching
	 *       the historical behavior of the pre-refactor dispatcher.
	 */
	bool runConversion(const ConversionFileContext& file, const ConversionBatchContext& ctx) {

		const auto& mode = ctx.mode;
		if (mode.mdf) return convertMdfToIso(file.inputPath, file.outputPath, ctx.completedBytes);
		if (mode.nrg) return convertNrgToIso(file.inputPath, file.outputPath, ctx.completedBytes);
		if (mode.chd) return convertChdToIso(file.inputPath, file.outputPath, ctx.completedBytes);
		if (mode.daa) return convertDaaToIso(file.inputPath, file.outputPath, ctx.completedBytes);
		return convertCcdToIso(file.inputPath, file.outputPath, ctx.completedBytes);

	}

	/**
	 * @brief Maps a filename's extension to a normalized upper-case image type.
	 *
	 * The comparison is ASCII case-insensitive and uses fixed-length checks so
	 * the caller does not pay for temporary string construction.
	 *
	 * @param fileNameOnly Filename (including extension) to inspect.
	 * @return A `std::string_view` naming the type (`"BIN"`, `"IMG"`, `"MDF"`,
	 *         `"NRG"`, `"CHD"`, `"DAA"`, `"GBI"`), or `"Image"` when the
	 *         extension is absent or unrecognized.
	 *
	 * @note The returned view aliases static storage or @p fileNameOnly; it is
	 *       valid only while @p fileNameOnly is alive.
	 */
	std::string_view getFileType(std::string_view fileNameOnly) {

		const std::size_t dotPos = fileNameOnly.find_last_of('.');
		if (dotPos == std::string_view::npos) return "Image";

		std::string_view ext = fileNameOnly.substr(dotPos + 1);
		auto iequal = [](std::string_view a, const char* b) {

			if (a.size() != std::strlen(b)) return false;
			for (std::size_t i = 0; i < a.size(); ++i)
			    if (std::tolower(static_cast<unsigned char>(a[i])) != b[i]) return false;
			return true;

		};

		if (iequal(ext, "bin")) return "BIN";
		if (iequal(ext, "img")) return "IMG";
		if (iequal(ext, "mdf")) return "MDF";
		if (iequal(ext, "nrg")) return "NRG";
		if (iequal(ext, "chd")) return "CHD";
		if (iequal(ext, "daa")) return "DAA";
		if (iequal(ext, "gbi")) return "GBI";
		return "Image";

	}

} // namespace

/**
 * @file convert.cpp
 * @brief Batch conversion driver for disk-image → ISO workflows.
 */

namespace {

	/**
	 * @brief Formats the path shown in log and console messages.
	 *
	 * When `displayConfig::toggleNamesOnly` is set the basename alone is used;
	 * otherwise the joined `"directory/filename"` form is produced.
	 *
	 * @param directory    Directory component of the image path.
	 * @param fileNameOnly Basename component of the image path.
	 * @return A formatted display path suitable for direct interpolation into
	 *         log messages.
	 */
	std::string formatDisplayPath(std::string_view directory, std::string_view fileNameOnly) {

		if (!displayConfig::toggleNamesOnly) {
			std::string displayPath;
			displayPath.reserve(directory.size() + 1 + fileNameOnly.size());
			displayPath.append(directory).append("/").append(fileNameOnly);
			return displayPath;
		}
		return std::string(fileNameOnly);

	}

	/**
	 * @brief Derives the destination ISO path from a source image path.
	 *
	 * Replaces the final `.`-suffix with `.iso`. Paths without an extension
	 * receive the suffix appended verbatim.
	 *
	 * @param inputPath Source image path.
	 * @return The corresponding `.iso` path. The caller is responsible for
	 *         checking that it does not already exist.
	 */
	std::string deriveOutputPath(const std::string& inputPath) {

		const std::size_t dotPos = inputPath.find_last_of('.');
		const std::size_t baseLen = (dotPos != std::string::npos) ? dotPos : inputPath.size();
		std::string outputPath;
		outputPath.reserve(baseLen + 4);
		outputPath.assign(inputPath, 0, baseLen);
		outputPath.append(".iso");
		return outputPath;

	}

	/**
	 * @brief Records a validation failure for a source image.
	 *
	 * Builds the localized failure message, drops the path from the active
	 * format cache when the file is missing, bumps the failed-task counter,
	 * and flushes the buffer when the batch threshold is reached.
	 *
	 * @param file   Per-file context (paths and display strings).
	 * @param valRes Result of `validateImageFile`; must not be `Valid`.
	 * @param ctx    Batch context (mutated to append the message and bump counters).
	 */
	void handleValidationFailure(const ConversionFileContext& file,
	                             ValidationResult valRes,
	                             ConversionBatchContext& ctx) {

		const auto& themes = ctx.themes;
		std::string msg;
		if (valRes == ValidationResult::Missing) {
			msg.reserve(themes.missingLabel.size() * 2 + themes.errPath.size() + file.displayPath.size() + 32);
			msg.append(themes.errLabel).append("✗ ")
			   .append(themes.errPath).append(file.displayPath)
			   .append(themes.missingLabel).append("  MissingFile");
			removeFromCache(file.inputPath, ctx);
		} else {
			msg.reserve(themes.errLabel.size() * 2 + themes.errPath.size() + file.displayPath.size() + 48);
			msg.append(themes.errLabel).append("✗ ")
			   .append(themes.errPath).append(file.displayPath)
			   .append(themes.errLabel).append("  NoAccess");
		}
		msg.append(UI::Palette::BoldReset);
		ctx.localFailedMsgs.push_back(std::move(msg));
		ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
		ctx.flushIfFull(ctx.localFailedMsgs);

	}

	/**
	 * @brief Records a skip for a job whose destination already exists.
	 *
	 * Bumps the skipped-task counter and flushes the buffer when the batch
	 * threshold is reached.
	 *
	 * @param file Per-file context (paths and display strings).
	 * @param ctx  Batch context (mutated to append the message and bump counters).
	 */
	void handleExistingOutput(const ConversionFileContext& file, ConversionBatchContext& ctx) {

		const auto& themes = ctx.themes;
		std::string msg;
		msg.reserve(themes.skipLabel.size() * 2 + themes.skipPath.size() + file.displayPath.size() + 32);
		msg.append(themes.skipLabel).append("– ")
		   .append(themes.skipPath).append(file.displayPath)
		   .append(themes.skipLabel).append("  ISOalrExists");
		msg.append(UI::Palette::BoldReset);
		ctx.localSkippedMsgs.push_back(std::move(msg));
		ctx.skippedTasks->fetch_add(1, std::memory_order_acq_rel);
		ctx.flushIfFull(ctx.localSkippedMsgs);

	}

	/**
	 * @brief Records a successful conversion.
	 *
	 * Applies the real user/group ownership to the produced ISO, appends its
	 * path to the optional output collection, builds the localized success
	 * message, bumps the completed-task counter, and flushes the buffer when
	 * the batch threshold is reached.
	 *
	 * @param file Per-file context (paths and display strings).
	 * @param ctx  Batch context (mutated to append the message and bump counters).
	 *
	 * @note `chown` failures are intentionally non-fatal and ignored — the
	 *       conversion result is still considered successful.
	 */
	void handleSuccessfulConversion(const ConversionFileContext& file, ConversionBatchContext& ctx) {

		const auto& themes = ctx.themes;
		[[maybe_unused]] int ret = chown(file.outputPath.c_str(), ctx.real_uid, ctx.real_gid);

		if (ctx.successfulOutputPaths && ctx.outPathsMutex) {
			std::lock_guard<std::mutex> lock(*ctx.outPathsMutex);
			ctx.successfulOutputPaths->push_back(file.outputPath);
		}

		std::string_view fileType = getFileType(file.fileNameOnly);
		std::string msg;
		msg.reserve(themes.okLabel.size() * 2 + themes.okPath.size() + file.outputPath.size() + fileType.size() + 16);
		msg.append(themes.okPath).append("✓ ")
		   .append(themes.okPath).append(file.outputPath)
		   .append(themes.okLabel).append("  ").append(fileType).append(" → ISO");
		msg.append(UI::Palette::BoldReset);
		ctx.localSuccessMsgs.push_back(std::move(msg));

		ctx.completedTasks->fetch_add(1, std::memory_order_acq_rel);
		ctx.flushIfFull(ctx.localSuccessMsgs);

	}

	/**
	 * @brief Records a failed conversion and cleans up any partial output.
	 *
	 * Removes a partially written ISO when present, distinguishes a
	 * user-requested cancellation from a backend error in the message,
	 * bumps the failed-task counter, and flushes the buffer when the batch
	 * threshold is reached.
	 *
	 * @param file Per-file context (paths and display strings).
	 * @param ctx  Batch context (mutated to append the message and bump counters).
	 */
	void handleFailedConversion(const ConversionFileContext& file, ConversionBatchContext& ctx) {

		namespace fs = std::filesystem;
		if (fs::exists(file.outputPath)) fs::remove(file.outputPath);

		bool isCancelled = GlobalState::g_operationCancelled.load(std::memory_order_relaxed);
		const auto& themes = ctx.themes;
		std::string msg;
		msg.reserve(themes.errLabel.size() * 2 + themes.errPath.size() + file.displayPath.size() + 24);
		msg.append(themes.errLabel).append("✗ ")
		   .append(themes.errPath).append(file.displayPath)
		   .append(themes.errLabel).append("  ").append(isCancelled ? "Cancelled" : "Failed");
		msg.append(UI::Palette::BoldReset);
		ctx.localFailedMsgs.push_back(std::move(msg));

		ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
		ctx.flushIfFull(ctx.localFailedMsgs);

	}

} // namespace

/**
 * @brief Batch-converts disk image files (BIN, IMG, MDF, NRG, CHD, CCD, DAA, GBI) to ISO format.
 *
 * Iterates over @p imageFiles, performing per-file validation (existence and
 * basic readability), skipping jobs whose destination ISO already exists,
 * dispatching to the backend selected by @p mode, and aggregating
 * success/failure/skip messages in a thread-safe manner. Successful output
 * paths are optionally collected for downstream database indexing.
 *
 * The iteration is cooperative with cancellation: when
 * `GlobalState::g_operationCancelled` is set, no further jobs are started and
 * the batch returns promptly after flushing any buffered messages.
 *
 * @param imageFiles              Source image paths to convert.
 * @param mode                    Selects the backend (MDF/MDS, NRG, CHD, DAA/GBI).
 *                                When all flags are @c false, the CCD backend is used.
 * @param batchData               Caller-owned counters and optional output collection:
 *                                - @c completedBytes: optional; may be @c nullptr.
 *                                - @c completedTasks: incremented per success; must
 *                                  be non-null.
 *                                - @c skippedTasks: incremented per skip; must
 *                                  be non-null.
 *                                - @c failedTasks: incremented per failure; must
 *                                  be non-null.
 *                                - @c successfulOutputPaths: optional sink for
 *                                  produced ISO paths; when non-null,
 *                                  @c outPathsMutex must also be non-null.
 *                                - @c outPathsMutex: guards @c successfulOutputPaths.
 *
 * @warning Every pointer in @p batchData must reference objects whose lifetime
 *          exceeds this call. Null pointers for the required atomics are
 *          undefined behavior.
 *
 * @note Safe to invoke concurrently for distinct batches provided the shared
 *       counters/mutex semantics are respected; the global verbose sets are
 *       internally locked.
 */
void convertToISO(
    const std::vector<std::string>& imageFiles,
    const ConversionMode& mode,
    const ConversionBatchData& batchData) {

	ConversionBatchContext ctx;
	ctx.themes = getConversionThemeStrings();
	ctx.mode = mode;

	std::string real_username, real_groupname;
	getRealUserId(ctx.real_uid, ctx.real_gid, real_username, real_groupname);

	ctx.completedBytes        = batchData.completedBytes;
	ctx.completedTasks        = batchData.completedTasks;
	ctx.failedTasks           = batchData.failedTasks;
	ctx.skippedTasks          = batchData.skippedTasks;
	ctx.successfulOutputPaths = batchData.successfulOutputPaths;
	ctx.outPathsMutex         = batchData.outPathsMutex;
	ctx.batchSize             = 50;

	for (const std::string& inputPath : imageFiles) {
		if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) break;

		ConversionFileContext file;
		file.inputPath = inputPath;
		auto [dir, name] = extractDirectoryAndFilename(file.inputPath, "conversions");
		file.fileNameOnly.assign(name);
		file.displayPath = formatDisplayPath(dir, name);

		ValidationResult valRes = validateImageFile(file.inputPath);
		if (valRes != ValidationResult::Valid) {
			handleValidationFailure(file, valRes, ctx);
			continue;
		}

		file.outputPath = deriveOutputPath(file.inputPath);

		if (fileExists(file.outputPath)) {
			handleExistingOutput(file, ctx);
			continue;
		}

		if (GlobalState::g_operationCancelled.load(std::memory_order_relaxed)) break;

		if (runConversion(file, ctx)) {
			handleSuccessfulConversion(file, ctx);
		} else {
			handleFailedConversion(file, ctx);
		}
	}

	ctx.flushMessages();

}
