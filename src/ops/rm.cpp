// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>

import std;
import isocmd.input.lineEditor;
import isocmd.ui.interface.pagination;
import isocmd.core.cancellation;
import isocmd.input.promptCompletion;
import isocmd.ops.cpMvRm;
import isocmd.ui.display.themes;
import isocmd.util.sort;
import isocmd.util.stringManipulation;

namespace fs = std::filesystem;

/**
 * @brief Orchestrates user confirmation for permanent file deletion.
 *
 * Displays the selected ISO files and captures a Y/N confirmation. Line editor
 * bindings are restored automatically through EditorKeyBindGuard.
 *
 * @param isoFiles Full list of ISO paths.
 * @param indexChunks Chunks used to format the displayed selection.
 * @param umountMvRmBreak Set according to whether deletion was confirmed.
 * @param abortDel Set when the user cancels or sends EOF.
 * @return True if deletion was confirmed.
 */
bool handleDeleteOperation(
    const std::vector<std::string>& isoFiles,
    const std::vector<std::vector<int>>& indexChunks,
    bool& umountMvRmBreak,
    bool& abortDel
)
{

	PendingLineEditorTextClearGuard pendingTextGuard;

	editorCompletionHook = nullptr;

	reset_custom_keybindingsForRm();
	editorBindKeySequence("\033", exit_handler);

	const CpMvRmColors colors = getCpMvRmColors();

	std::string green = "\001" + std::string(colors.prompt_green) + "\002";
	std::string blue  = "\001" + std::string(colors.prompt_blue) + "\002";
	std::string red   = "\001" + std::string(UI::Palette::Red) + "\002";
	std::string reset = "\001" + std::string(UI::Palette::BoldReset) + "\002";

	std::vector<std::string> entries =
	    generateIsoEntries(indexChunks, isoFiles);

	sortFilesCaseInsensitive(entries);

	const std::string promptPrefix = "\n";

	std::string promptSuffix(blue);
	promptSuffix.append("The selected ").append(green).append("ISO").append(blue)
	            .append(" will be ").append(red).append("*PERMANENTLY DELETED*")
	            .append(blue).append(". Proceed? (y/n): ").append(reset);

	bool isPageTurn = false;

	auto setupEnv = [&]() {

		RestoreLineEditorBuffer();

		editorBindKey('\f', [](int count, int key) -> int {

			clear_screen_and_buffer(count, key);

			editorReplaceLine("");
			editorDone = 1;
			return 0;

		});

	};

	std::size_t currentPage = 0;

	while (true) {
		std::string userInput;

		{
			EditorKeyBindGuard keyGuard;

			userInput = handlePaginatedDisplay(
			    entries,
			    promptPrefix,
			    promptSuffix,
			    setupEnv,
			    isPageTurn,
			    currentPage,
			    true
			);
		}

		if (userInput == "EOF_SIGNAL") {
			umountMvRmBreak = false;
			abortDel = true;
			return false;
		}

		if (!isPageTurn && !userInput.empty()) {
			const std::string choice = trimWhitespace(userInput);

			if (choice == "Y" || choice == "y") {
				umountMvRmBreak = true;
				return true;
			}

			umountMvRmBreak = false;
			abortDel = true;
			return false;
		}
	}

}

/**
 * @brief Deletes one file and logs the outcome.
 *
 * Respects the global cancellation flag. On success, the file size is added
 * to completedBytes and a success message is emitted. On failure, an error
 * message is reported.
 *
 * @param ctx Operation context for reporting and counters.
 * @param srcPath Full path to the file to delete.
 * @param srcDir Source directory for display.
 * @param srcFile Source filename for display.
 * @param fileSize File size in bytes.
 */
void performDeleteOperation(
    OperationContext& ctx,
    const fs::path& srcPath,
    std::string_view srcDir,
    std::string_view srcFile,
    std::size_t fileSize
)
{

	const CpMvRmColors colors = getCpMvRmColors();
	const std::string displaySrc = buildDisplaySrc(srcDir, srcFile);

	if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
		std::string msg;
		msg.reserve(128 + displaySrc.size());

		msg.append(colors.error_label).append("✗ ")
		   .append(colors.error_path).append(displaySrc)
		   .append(colors.error_label).append(": ")
		   .append("Cancelled")
		   .append(UI::Palette::BoldReset);

		ctx.reporter.addError(std::move(msg));

		if (ctx.failedTasks) {
			ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
		}

		ctx.operationSuccessful.store(false, std::memory_order_release);
		return;
	}

	std::error_code ec;

	if (fs::remove(srcPath, ec)) {
		if (ctx.completedBytes) {
			ctx.completedBytes->fetch_add(
			    fileSize,
			    std::memory_order_relaxed
			);
		}

		std::string msg;
		msg.reserve(
		    2 * colors.success_label.size() +
		    colors.success_path.size() +
		    displaySrc.size() +
		    2 * UI::Palette::BoldReset.size() +
		    16
		);

		msg.append(colors.success_path).append("✓ ").append(displaySrc)
		   .append(UI::Palette::BoldReset);

		ctx.reporter.addSuccess(std::move(msg));

		if (ctx.completedTasks) {
			ctx.completedTasks->fetch_add(1, std::memory_order_acq_rel);
		}
	} else {
		std::string msg;
		msg.reserve(
		    128 + displaySrc.size() +
		    (ec ? ec.message().size() : 20)
		);

		msg.append(colors.error_label).append("✗ ")
		   .append(colors.error_path).append(displaySrc)
		   .append(colors.error_label).append(": ")
		   .append(ec ? ec.message() : "File not found")
		   .append(UI::Palette::BoldReset);

		ctx.reporter.addError(std::move(msg));

		if (ctx.failedTasks) {
			ctx.failedTasks->fetch_add(1, std::memory_order_acq_rel);
		}

		ctx.operationSuccessful.store(false, std::memory_order_release);
	}

}
