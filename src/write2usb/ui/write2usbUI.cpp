// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include <readline/history.h>
#include <readline/readline.h>

import std;
import isocmd.core.cancellation;
import isocmd.input.inputHandling;
import isocmd.input.readline;
import isocmd.threadpool.threadpool;
import isocmd.ui.display.display;
import isocmd.ui.interface.pausePrompts;
import isocmd.ui.display.readlineResize;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.cli.termSize;
import isocmd.ui.display.themes;
import isocmd.util.stringManipulation;
import isocmd.write2usb.util.terminalGuards;
import isocmd.write2usb.ui.textWidth;
import isocmd.write2usb.ui.write2usbUI;

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Global scope: external declarations
// ---------------------------------------------------------------------------

void helpMappings();
void displayErrors();

// --------------------------------------------------------------------------- //
// namespace MappingPrompt  (interactive prompt and confirmation UI)           //
// --------------------------------------------------------------------------- //

namespace MappingPrompt {

	/**
	 * @brief Represents summary information for a physical removable storage device.
	 */
	struct DeviceInfo {
		std::string path;     ///< Device node path (e.g., /dev/sdb).
		std::uint64_t size;        ///< Total storage capacity in bytes.
		std::string driveName;///< Vendor or model drive name.
		std::string sizeStr;  ///< Human-readable formatted file size string.
		bool mounted;         ///< True if the device currently has active mounts.
		bool error;           ///< True if an error occurred while probing the device.
	};

	/**
	 * @brief Represents a single row in the pre-burn warning confirmation table.
	 */
	struct WarnRow {
		std::string device;       ///< Target device path.
		std::string driveName;    ///< Drive model/name description.
		std::string deviceSizeStr;///< Formatted storage device size.
		std::string filename;     ///< Source ISO filename.
		std::string isoSizeStr;   ///< Formatted ISO image size.
	};

	/**
	 * @brief Constructs the interactive index prompt string for device mapping input.
	 *
	 * @param wt The active write theme configuration.
	 * @return std::string Formatted ANSI-colored prompt string.
	 */
	std::string buildIdxPrompt(const WriteTheme& wt) {

		std::string prompt("\n");
		prompt
		    .append(UI::Palette::RL_idxGreen)
		    .append("IDX>DEVICE")
		    .append(wt.rl_primaryCol)
		    .append(" ↵ for ")
		    .append(UI::Palette::RL_Yellow)
		    .append("write2usb")
		    .append(wt.rl_primaryCol)
		    .append(", ? for help: ")
		    .append(wt.rl_resetCol);
		return prompt;

	}

	/**
	 * @brief Validates whether the number of selected ISOs exceeds the thread pool capacity.
	 *
	 * @param pool Reference to the application thread pool.
	 * @param selectedIsos Vector of ISO files chosen by the user.
	 * @param wt The active write theme configuration.
	 * @return true If the selection limit has been exceeded.
	 * @return false If within safe execution limits.
	 */
	bool checkThreadPoolLimit(ThreadPool& pool, const std::vector<IsoInfo>& selectedIsos, const WriteTheme& wt) {

		if (selectedIsos.size() <= pool.threadCount()) return false;
		auto render = [&] {

			SyncOutput sync;
			clearScrollBuffer();
			std::cout << "\n"
			          << wt.colorFailure << "Error: "
			          << UI::Palette::Yellow << "write2usb"
			          << wt.colorFailure << " selection limit" << wt.colorFailure << " is "
			          << wt.colorWarning << pool.threadCount()
			          << wt.colorFailure << " (thread pool size)."
			          << wt.speedCol << "\n";

		};
		pressEnterToTryWithRepaint(render);
		return true;

	}

	/**
	 * @brief Collects probe information and sizes for a list of USB device paths.
	 *
	 * @param usbDevices Vector of detected USB device node paths.
	 * @return std::vector<DeviceInfo> Sorted and populated device details.
	 */
	std::vector<DeviceInfo> collectAndSortDeviceInfos(const std::vector<std::string>& usbDevices) {

		std::vector<DeviceInfo> deviceInfos;
		deviceInfos.reserve(usbDevices.size());
		for (const auto& device : usbDevices) {
			try {
				std::string driveName = getDriveName(device);
				std::uint64_t deviceSize = getBlockDeviceSize(device);
				std::string sizeStr = formatFileSize(deviceSize);
				bool mounted = isDeviceMounted(device);
				deviceInfos.push_back({device, deviceSize, driveName, sizeStr, mounted, false});
			} catch (...) {
				deviceInfos.push_back({device, 0, "", "", false, true});
			}
		}

		std::sort(deviceInfos.begin(), deviceInfos.end(), [](const DeviceInfo& a, const DeviceInfo& b) {

			if (a.error != b.error) return !a.error;
			return a.size > b.size;

		});
		return deviceInfos;

	}

	/**
	 * @brief Builds the comprehensive device selection prompt string listing ISOs and targets.
	 *
	 * @param selectedIsos Original collection of selected ISO inputs.
	 * @param[out] sortedIsos Output parameter containing size-sorted ISO metadata.
	 * @param[out] usbDevices Output parameter containing detected removable block devices.
	 * @param wt The active write theme configuration.
	 * @return std::string The fully formatted multi-line console prompt.
	 */
	std::string buildDevicePromptString(
	    const std::vector<IsoInfo>& selectedIsos,
	    std::vector<IsoInfo>& sortedIsos,
	    std::vector<std::string>& usbDevices,
	    const WriteTheme& wt, WriteMapping::Discover discover
	) {

		sortedIsos = selectedIsos;
		std::sort(sortedIsos.begin(), sortedIsos.end(), [](const IsoInfo& a, const IsoInfo& b) {

			return a.size > b.size;

		});

		std::ostringstream stream;
		stream << "\n" << color << "Selected " << wt.fileCol << "ISO" << color << ":\n\n";

		for (std::size_t i = 0; i < sortedIsos.size(); ++i) {
			auto [isoDir, filename] = extractDirectoryAndFilename(sortedIsos[i].path, "write2usb");
			stream << wt.indexCol << " " << (i + 1) << ">" << " ";
			if (!displayConfig::toggleNamesOnly) {
				stream << wt.pathCol << isoDir << "/";
			}
			stream << wt.fileCol << filename;
			stream << " " << UI::Palette::DimGray << "(" << wt.sizeCol << sortedIsos[i].sizeStr
			       << UI::Palette::DimGray << ")\n";
		}

		stream << "\n" << color << "Removable USB Flash Devices:" << wt.rl_resetCol << "\n\n";
		usbDevices = discover();
		auto deviceInfos = collectAndSortDeviceInfos(usbDevices);

		if (deviceInfos.empty()) {
			stream << " " << wt.colorFailure << "No USB flash drives detected!" << wt.rl_resetCol << "\n";
		} else {
			for (const auto& dev : deviceInfos) {
				if (dev.error) {
					stream << " " << wt.colorFailure << dev.path << " (error)" << wt.rl_resetCol << "\n";
				} else {
					stream << wt.deviceCol << " " << dev.path
					       << wt.infoLabel << " <" << dev.driveName
					       << "> " << UI::Palette::DimGray << "(" << wt.sizeCol << dev.sizeStr
					       << UI::Palette::DimGray << ")"
					       << ([&]() -> std::string {

							   if (dev.mounted) return wt.colorFailure + " [MOUNTED]" + wt.rl_resetCol;
							   return "";

						   }())
					       << "\n";
				}
			}
		}

		g_completerData.sortedIsos = &sortedIsos;
		g_completerData.usbDevices = &usbDevices;
		reset_custom_keybindingsForCpMvWrite2Usb();

		stream << buildIdxPrompt(wt);
		return stream.str();

	}

	/**
	 * @brief Generates warning row dataset structures from validated ISO-to-device mapping pairs.
	 *
	 * @param validPairs Vector of validated ISO and target device mappings.
	 * @return std::vector<WarnRow> Compact structural rows ready for confirmation formatting.
	 */
	std::vector<WarnRow> buildWarnRows(const std::vector<std::pair<IsoInfo, std::string>>& validPairs) {

		std::vector<WarnRow> warnRows;
		warnRows.reserve(validPairs.size());
		for (const auto& [iso, device] : validPairs) {
			warnRows.push_back({ device,
				                 getDriveName(device),
				                 formatFileSize(getBlockDeviceSize(device)),
				                 iso.filename,
				                 iso.sizeStr });
		}
		return warnRows;

	}

	/**
	 * @brief Renders the destructive action confirmation table layout for terminal UI.
	 *
	 * @param cols Current terminal column width constraint.
	 * @param clearFirst If true, issues screen reset sequence codes before painting.
	 * @param devicePrompt Original prompt content block string.
	 * @param mainInputString User-supplied input string to echo back.
	 * @param warnRows Structural warning metadata row items.
	 * @param wt The active write theme configuration.
	 * @return std::vector<size_t> Calculated character widths per rendered line for window resizing hooks.
	 */
	std::vector<std::size_t> renderConfirmRows(
	    std::size_t cols,
	    bool clearFirst,
	    const std::string& devicePrompt,
	    const std::string& mainInputString,
	    const std::vector<WarnRow>& warnRows,
	    const WriteTheme& wt
	) {

		const std::size_t usableCols = cols > 1 ? cols - 1 : 1;
		std::vector<std::size_t> widths;

		if (clearFirst) {
			std::cout << "\033[H\033[2J\033[3J";
			std::cout << promptHeaderForRepaint(devicePrompt);
			std::cout << buildIdxPrompt(wt) << mainInputString << "\n";
			std::cout << "\n";
		} else {
			std::cout << "\n";
			widths.push_back(0);
		}

		std::cout << wt.colorWarning << "WARNING: This will "
		          << UI::Palette::Red << "*ERASE ALL DATA*"
		          << wt.colorWarning << " on:" << wt.rl_resetCol << "\n\n";
		widths.push_back(0);
		widths.push_back(utf8Width("WARNING: This will *ERASE ALL DATA* on:"));
		widths.push_back(0);
		rl_bind_keyseq("?", rl_insert);
		for (const auto& r : warnRows) {
			const std::size_t fixedWidth = 17 + r.device.size() + utf8Width(r.driveName)
			                        + utf8Width(r.deviceSizeStr) + utf8Width(r.isoSizeStr);
			const std::size_t availForName = usableCols > fixedWidth ? usableCols - fixedWidth : 0;
			const std::string shownName = truncateIsoName(r.filename, availForName);

			std::ostringstream row;
			row << UI::Palette::DimGray << " {" << wt.deviceCol << r.device
			    << " " << wt.infoLabel << "<" << r.driveName << ">" << UI::Palette::DimGray << " ("
			    << wt.sizeCol << r.deviceSizeStr
			    << UI::Palette::DimGray << ")} " << color << "←" << UI::Palette::DimGray << " {" << wt.fileCol
			    << shownName << UI::Palette::DimGray << " (" << wt.sizeCol
			    << r.isoSizeStr << UI::Palette::DimGray << ")}";

			const std::string clipped = clipToWidth(row.str(), usableCols);
			std::cout << clipped << "\n";
			widths.push_back(std::min(visibleWidth(clipped), usableCols));
		}

		std::cout << "\n";
		widths.push_back(0);
		return widths;

	}

	/**
	 * @brief Renders error messages with the specified prompt, main input string, and theme styling.
	 *
	 * @param devicePrompt The current device input prompt string.
	 * @param mainInputString The raw input string entered by the user.
	 * @param errors Vector of error messages to display.
	 * @param wt The current write theme configuration.
	 * @param color Color formatting string for error bullets.
	 */
	void renderErrorMessage(const std::string& devicePrompt,
	                               const std::string& mainInputString,
	                               const std::vector<std::string>& errors,
	                               const WriteTheme& wt,
	                               std::string_view color)
	{

		SyncOutput sync;
		clearScrollBuffer();
		std::cout << promptHeaderForRepaint(devicePrompt);
		std::cout << buildIdxPrompt(wt) << mainInputString << "\n";
		std::cerr << "\n" << wt.rl_errorCol << "Errors:" << wt.rl_resetCol << "\n";
		for (const auto& e : errors) {
			std::cerr << color << " • " << e << "\n";
		}

	}

	/**
	 * @brief Confirms valid ISO-to-device mappings.
	 *
	 * @param devicePrompt    The current device input prompt string.
	 * @param mainInputString The raw input string entered by the user.
	 * @param warnRows        Warning rows generated for the valid pairs.
	 * @param wt              The current write theme configuration.
	 * @param color           Color formatting string for the prompts.
	 * @return true when confirmed; false when declined or aborted.
	 */
	bool confirmDeviceMappings(
	    const std::string& devicePrompt,
	    const std::string& mainInputString,
	    const std::vector<WarnRow>& warnRows,
	    const WriteTheme& wt,
	    std::string_view color)
	{

		const std::size_t drawnCols = terminalSize().cols;
		std::vector<std::size_t> drawnWidths;

		{
			SyncOutput sync;
			drawnWidths = renderConfirmRows(
			    drawnCols, false,
			    devicePrompt, mainInputString, warnRows, wt);
		}

		const std::string promptText = "Proceed? (y/n): ";

		std::string confirmPrompt("\001");
		confirmPrompt.append(color)
		             .append("\002")
		             .append(promptText)
		             .append(wt.rl_resetCol);

		std::unique_ptr<char, decltype(&std::free)> confirmation(
		    nullptr, &std::free);

		{
			ConfirmationModeGuard confirmGuard;
			ConfirmResizeWatcher watcher(
			    [&](std::size_t cols) {

					return renderConfirmRows(
					    cols, true,
					    devicePrompt, mainInputString, warnRows, wt);

				},
			    drawnCols, drawnWidths, promptText);

			confirmation.reset(readline(confirmPrompt.c_str()));
			watcher.stop();
		}

		std::string ans;

		if (confirmation) {
			ans = confirmation.get();

			while (!ans.empty() &&
			       std::isspace(static_cast<unsigned char>(ans.back())))
			    ans.pop_back();
		}

		return ans == "y" || ans == "Y";

	}

} // namespace MappingPrompt

// --------------------------------------------------------------------------- //
// Device mapping collection (public API)                                      //
// --------------------------------------------------------------------------- //

namespace {

	/**
	 * @brief Installs a temporary GNU Readline startup hook to prefill the next input line.
	 *
	 * Restores any previously registered `rl_startup_hook` before inserting the prefilled text.
	 * If `prefill` is empty, no hook is installed.
	 *
	 * @param prefill Text to insert into the Readline buffer when Readline starts.
	 */
	void installPrefillStartupHook(const std::string& prefill)
	{

		static std::string activePrefillText;
		static rl_hook_func_t* previousStartupHook = nullptr;

		rl_startup_hook = nullptr;
		previousStartupHook = nullptr;
		activePrefillText.clear();

		if (prefill.empty()) {
			return;
		}

		activePrefillText = prefill;
		previousStartupHook = rl_startup_hook;
		rl_startup_hook = +[]() -> int {
			if (previousStartupHook) {
				previousStartupHook();
			}
			if (!activePrefillText.empty()) {
				rl_insert_text(activePrefillText.c_str());
			}
			return 0;
		};

	}

	/**
	 * @brief Clears any temporary Readline startup hook installed for input prefilling.
	 */
	void clearPrefillStartupHook()
	{

		rl_startup_hook = nullptr;

	}

	/**
	 * @brief Extracts file paths from sorted ISO descriptors for mapping parsing.
	 *
	 * @param sortedIsos Ordered list of available ISO descriptors.
	 * @return std::vector<std::string> Vector of ISO file paths in matching order.
	 */
	std::vector<std::string> extractIsoFilenames(const std::vector<IsoInfo>& sortedIsos)
	{

		std::vector<std::string> isoFilenames;
		isoFilenames.reserve(sortedIsos.size());
		for (const auto& iso : sortedIsos) {
			isoFilenames.push_back(iso.path);
		}
		return isoFilenames;

	}

	/**
	 * @brief Prompts the user to acknowledge parse or validation errors with window-resize repaint support.
	 *
	 * Disables `SIGINT` and `Ctrl+D` before waiting for acknowledgement when validation fails.
	 *
	 * @param devicePrompt Rendered Readline prompt string.
	 * @param rawInput Raw user input string to re-display alongside the error message.
	 * @param messages List of parse or validation error messages to render.
	 * @param wt Active write theme configuration.
	 * @param pauseForContinue True to wait to continue (e.g., missing root permissions); false to retry.
	 * @param ignoreSignals True to ignore `SIGINT` and disable `Ctrl+D` prior to prompting.
	 */
	void handlePromptErrors(
	    const std::string& devicePrompt,
	    const std::string& rawInput,
	    const std::vector<std::string>& messages,
	    const WriteTheme& wt,
	    bool pauseForContinue,
	    bool ignoreSignals)
	{

		if (ignoreSignals) {
			signal(SIGINT, SIG_IGN);
			disable_ctrl_d();
		}

		const auto renderError = [&] {

			MappingPrompt::renderErrorMessage(devicePrompt, rawInput, messages, wt, color);

		};

		if (pauseForContinue) {
			pressEnterToContinueWithRepaint(renderError);
		} else {
			pressEnterToTryWithRepaint(renderError);
		}

	}

	/**
	 * @brief Finalizes confirmed ISO-to-device mappings and prepares signal state for writing.
	 */
	void finalizeConfirmedMappings()
	{

		setupSignalHandlerCancellations();
		GlobalState::g_operationCancelled.store(false);

	}

} // namespace

/**
 * @brief Interactive loop to collect, parse, validate, and confirm mappings between ISO images and USB storage targets.
 *
 * Manages the interactive terminal session using GNU Readline, displaying available ISOs and USB devices,
 * parsing user mapping inputs, validating device permissions and constraints, displaying warnings, and prompting
 * for final user confirmation before returning confirmed association pairs.
 *
 * @param selectedIsos Vector of selected ISO files designated for writing.
 * @return std::vector<std::pair<IsoInfo, std::string>> Vector of confirmed (ISO, Target Device) association pairs,
 *         or an empty vector if cancelled or stopped by thread pool limits.
 */
std::vector<std::pair<IsoInfo, std::string>> WriteMapping::collectWithDevices(
    const std::vector<IsoInfo>& selectedIsos, Discover discover, Validate validate, bool noPermissions)
{

	PendingReadlineTextClearGuard pendingTextGuard;
	const WriteTheme wt = getWriteTheme();
	std::string pendingInput;

	while (true) {
		TerminalStateGuard termGuard;
		ThreadPool& pool = getStaticThreadPool();
		if (MappingPrompt::checkThreadPoolLimit(pool, selectedIsos, wt)) {
			return {};
		}

		displayErrors();

		std::vector<IsoInfo> sortedIsos;
		std::vector<std::string> usbDevices;
		const std::string devicePrompt =
		    MappingPrompt::buildDevicePromptString(selectedIsos, sortedIsos, usbDevices, wt, discover);

		const auto repaintDevices = [&] {

			SyncOutput sync;
			clearScrollBuffer();
			std::cout << promptHeaderForRepaint(devicePrompt);

		};

		armPendingInputRestore();
		installPrefillStartupHook(pendingInput);
		pendingInput.clear();

		RlLine deviceInput = readlineWithResize(devicePrompt, repaintDevices);
		clearPrefillStartupHook();

		if (!deviceInput) {
			return {};
		}

		std::string mainInputString(deviceInput.get());
		if (mainInputString.empty()) {
			continue;
		}
		if (mainInputString == "\x1b") {
			return {};
		}
		if (mainInputString == "?") {
			helpMappings();
			continue;
		}

		add_history(deviceInput.get());

		std::vector<std::string> parseErrors;
		auto deviceMap = parseDeviceMappings(deviceInput.get(), extractIsoFilenames(sortedIsos), parseErrors);
		if (!parseErrors.empty()) {
			handlePromptErrors(devicePrompt, mainInputString, parseErrors, wt, false, false);
			continue;
		}

		std::vector<std::string> validationErrors;
		auto validPairs = validate(deviceMap, sortedIsos, noPermissions, validationErrors);
		if (!validationErrors.empty()) {
			handlePromptErrors(devicePrompt, mainInputString, validationErrors, wt, noPermissions, true);
			continue;
		}

		if (validPairs.empty()) {
			pendingInput = mainInputString;
			continue;
		}

		auto warnRows = MappingPrompt::buildWarnRows(validPairs);
		if (MappingPrompt::confirmDeviceMappings(devicePrompt, mainInputString, warnRows, wt, color)) {
			finalizeConfirmedMappings();
			return validPairs;
		}

		rl_bind_keyseq("?", help_handler);
		pendingInput = mainInputString;
	}

}

std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(
    const std::vector<IsoInfo>& selectedIsos) {

	return WriteMapping::collectWithDevices(selectedIsos, getRemovableDevices, validateDevices, geteuid() != 0);

}
