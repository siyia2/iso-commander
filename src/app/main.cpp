// SPDX-License-Identifier: GPL-3.0-or-later

#include <stdio.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

import std;
import isocmd.input.lineEditor;
import isocmd.app.main;
import isocmd.settings.settings;
import isocmd.core.cancellation;
import isocmd.core.lists;
import isocmd.core.paths;
import isocmd.core.sharedRefreshState;
import isocmd.core.uiState;
import isocmd.database.databaseOps;
import isocmd.input.inputHandling;
import isocmd.input.terminalControl;
import isocmd.input.promptCompletion;
import isocmd.threadpool.threadpool;
import isocmd.ui.display.lineEditorResize;
import isocmd.ui.cli.syncOutput;
import isocmd.ui.display.themes;
import isocmd.write2usb.util.drainingState;

namespace fs = std::filesystem;

/**
 * @brief Prints the program version to stdout, bolded.
 *
 * @param version The version string to display.
 */
static void printVersionNumber(const std::string& version) {

	std::cout << UI::Palette::BoldReset << "Iso Commander v" << version << UI::Palette::Reset << "\n";

}

namespace {

	/**
	 * @brief Handles version queries and non-interactive commands.
	 * @return The command's exit code, or std::nullopt to start the interactive menu.
	 */
	std::optional<int> tryExecuteCliMode(int argc, char *argv[]) {

		const std::string arg1 = (argc >= 2) ? argv[1] : "";

		if (argc == 2 && (arg1 == "--version" || arg1 == "-v")) {
			printVersionNumber("8.0.4");
			return 0;
		}
		if (argc >= 2) {
			return handleMountUmountConvertWriteCommands(argc, argv);
		}
		return std::nullopt; // interactive mode

	}

	/**
	 * @brief Prepares application state, caches, line editor and signal handling.
	 * Ctrl+C is ignored at the main prompt; SIGTERM is handled by signalHandler.
	 */
	void initializeEnvironment() {

		GlobalState::isNonInteractive.store(false);

		// Generous reserves for future lists.
		GlobalState::globalIsoFileList.reserve(1000);
		GlobalState::binImgFilesCache.reserve(1000);
		GlobalState::mdfMdsFilesCache.reserve(1000);
		GlobalState::nrgFilesCache.reserve(1000);
		GlobalState::chdFilesCache.reserve(1000);
		GlobalState::daaGbiFilesCache.reserve(1000);

		setupLineEditorToIgnoreCtrlC();

		// Line editor completion behavior.
		editorCompletionDisplayHook = customListingsFunction;

		// Esc as universal exit handler; PgUp/PgDn as history navigation.
		editorBindKeySequence("\033", exit_handler);
		editorBindKeySequence("\033[5~", editorPreviousHistory);
		editorBindKeySequence("\033[6~", editorNextHistory);

		// Signal Handling
		signal(SIGINT, SIG_IGN);
		signal(SIGTERM, signalHandler);

		paginationSet(GlobalState::configPath);

		// Line editor refresh hook for pending UI refreshes.
		editorEventHook = checkPendingRefresh;

	}

	/**
	 * @brief Locks the supplied lock file so only one interactive instance can run.
	 * On failure, print a diagnostic and close the descriptor. The caller keeps
	 * the open descriptor on success; closing it releases the advisory lock.
	 */
	bool acquireSingleInstanceLock(int& lockFd, const PrintListTheme& c, const char* lockFile) {

		lockFd = open(lockFile, O_CREAT | O_RDWR, 0666);
		struct flock fl = { F_WRLCK, SEEK_SET, 0, 0, 0 };
		if (fcntl(lockFd, F_SETLK, &fl) == -1) {
			std::cerr << c.indexA << "error: " << c.num
			          << "failed to setup transaction (unable to lock database)\n  "
			          << UI::Palette::BoldReset << "if you're sure isocmd isn't already running, you can remove '/tmp/isocmd.lock'\n"
			          << UI::Palette::Reset;
			close(lockFd);
			return false;
		}
		return true;

	}

	/**
	 * @brief Conditionally launches the background database import thread.
	 *
	 * Starts background database synchronization if auto-updates are enabled and valid history paths exist.
	 *
	 * @param search Atomic boolean tracking whether user config updates require searching.
	 * @param importState Shared pointer managing background refresh state coordination.
	 * @param backgroundThreads Vector tracking active background worker threads.
	 */
	void initializeBackgroundImport(std::atomic<bool>& search,
	                                std::shared_ptr<RefreshState>& importState,
	                                std::vector<std::thread>& backgroundThreads) {

		if (search.load() && (!(isHistoryFileEmpty(GlobalState::historyFilePath) || !fs::is_regular_file(GlobalState::historyFilePath)))) {
			importState->isImportRunning.store(true);
			backgroundThreads.emplace_back([importState, &search] {

				backgroundDatabaseImport(importState);
				search.store(false);

			});
		}

	}

	/**
	 * @brief Updates the main-menu status for background database imports.
	 * Starts a monitor only once for a running import, or a timed message when
	 * auto-update is enabled but no stored scan folders are available.
	 */
	void updateStatusLine(std::atomic<bool>& search,
	                      std::shared_ptr<RefreshState>& importState,
	                      std::atomic<bool>& messageActive,
	                      std::atomic<bool>& stopMessage,
	                      std::atomic<bool>& monitorThreadSpawned,
	                      std::atomic<bool>& isAtMain,
	                      std::vector<std::thread>& backgroundThreads,
	                      bool& messagePrinted,
	                      std::string& statusLine) {

		if (search.load() && !isHistoryFileEmpty(GlobalState::historyFilePath) &&
		    importState && importState->isImportRunning.load()) {
			statusLine = std::string(UI::Palette::Dim) +
			             "[Auto-Update: running in the background...]\n" +
			             std::string(UI::Palette::Reset);
			messageActive = true;
			if (!monitorThreadSpawned.exchange(true))
			    backgroundThreads.emplace_back(monitorAndClearMessage,
			                                   importState, std::ref(messageActive),
			                                   std::ref(stopMessage), std::ref(isAtMain));
		} else if ((search.load() && !messagePrinted) &&
		           (isHistoryFileEmpty(GlobalState::historyFilePath) || !fs::is_regular_file(GlobalState::historyFilePath))) {
			statusLine = std::string(UI::Palette::Dim) +
			             "[Auto-Update: no stored FolderPaths to scan...]\n" +
			             std::string(UI::Palette::Reset);
			messagePrinted = true;
			messageActive = true;
			backgroundThreads.emplace_back(clearMessageAfterTimeoutInMain, 4, std::ref(isAtMain),
			                               importState, std::ref(messageActive),
			                               std::ref(stopMessage));
		}

	}

	/**
	 * @brief Routes the main-menu choice to browsing, conversion, database refresh,
	 * settings or exit.
	 */
	void dispatchMenuChoice(const std::string& choice,
	                        bool& exitProgram,
	                        std::atomic<bool>& isAtMain,
	                        std::atomic<bool>& isAtISOList,
	                        std::shared_ptr<RefreshState>& importState,
	                        std::vector<std::thread>& backgroundThreads) {

		if (choice == "1") {
			isAtMain.store(false);
			isAtISOList.store(false);
			submenu1(isAtISOList, importState, backgroundThreads);
		} else if (choice.length() == 1) {
			switch (choice[0]) {
				case '2':
				    isAtMain.store(false);
					isAtISOList.store(false);
					submenu2(importState);
					break;
				case '3': {
					isAtMain.store(false);
					isAtISOList.store(false);
					bool newISOFound = false;
					refreshForDatabase(true, -1, false, newISOFound);
					break;
				}
				case '4':
				    isAtMain.store(false);
					isAtISOList.store(false);
					interactiveConfigEditor(GlobalState::configPath);
					break;
				case '5':
				    exitProgram = true;
					clearScrollBuffer();
					break;
				default: break;
			}
		}

	}

	/**
	 * @brief Performs graceful cleanup and resource release upon application shutdown.
	 *
	 * Shuts down thread pools, signals background tasks to stop, notifies worker condition variables,
	 * joins all active threads, flushes terminal formatting, and releases/unlinks the single-instance lock file.
	 *
	 * @param backgroundThreads Vector of background threads to join.
	 * @param importState Shared pointer managing refresh state flags.
	 * @param stopMessage Atomic flag signaling workers to terminate.
	 * @param lockFd File descriptor handle for the advisory lock.
	 */
	void cleanupAndExit(std::vector<std::thread>& backgroundThreads,
	                    std::shared_ptr<RefreshState>& importState,
	                    std::atomic<bool>& stopMessage,
	                    int lockFd,
	                    const char* lockFile) {

		getStaticThreadPool().shutdown();

		importState->stopImport = stopMessage = true;
		GlobalState::g_operationCancelled.store(true, std::memory_order_release);

		if (importState) {
			importState->isImportRunning.store(false, std::memory_order_release);
			{
				std::lock_guard<std::mutex> lk(importState->workerMutex);
				importState->workerCV.notify_all();
			}
		}

		for (auto& t : backgroundThreads) {
			if (t.joinable()) {
				t.join();
			}
		}

		g_drainingManager.joinAll();

		std::cout << UI::Palette::Reset << std::flush;
		close(lockFd);
		unlink(lockFile);

	}

} // namespace

/**
 * @brief Entry point for isocmd, an ISO management and mounting utility.
 *
 * Reads the user config, dispatches command-line modes, acquires a
 * single-instance lock, configures signal handling, launches background
 * tasks (auto-update / database import), and enters the interactive menu
 * loop.
 *
 * Command-line modes:
 * - `-v` / `--version` : print version and exit.
 * - `mount` / `umount` / `unmount` : delegate to the mount/unmount handler.
 * - `c2iso` : non-interactive image -> ISO conversion
 *   (e.g. `isocmd c2iso -d2 ~/Games`).
 * - `w2usb` : non-interactive ISO-to-USB writer
 *   (e.g. `isocmd w2usb file.iso /dev/sdd`).
 * - No arguments : interactive TUI.
 *
 * The main menu prompt is wrapped in a @c ListResizeWatcher (via
 * @c editLineWithResize), so the menu (banner, status message, menu box) is
 * repainted when the terminal width changes while the prompt waits.
 *
 * @param argc Number of command-line arguments.
 * @param argv Array of null-terminated argument strings.
 * @return 0 on success, 1 on failure (e.g. lock acquisition failure).
 */
int main(int argc, char *argv[]) {

	editorInitializeLocale();

	// User-defined folder paths and settings.
	std::map<std::string, std::string> config = readUserConfigLists(GlobalState::configPath);

	// --- Version & Utility Command Dispatch ---
	if (auto cliExitCode = tryExecuteCliMode(argc, argv)) {
		return *cliExitCode;
	}

	/// @name Application State Flags
	/// Atomic booleans coordinating concurrency and UI state between the main
	/// thread and background workers.
	/// @{
	std::atomic<bool> messageActive{false}, stopMessage{false}, monitorThreadSpawned{false};
	std::atomic<bool>& isAtMain    = GlobalState::isAtMain;
	std::atomic<bool>& isAtISOList = GlobalState::isAtISOList;
	/// @}

	initializeEnvironment();

	/**
	 * @brief Single-instance lock.
	 *
	 * Acquires an exclusive advisory lock on /tmp/isocmd.lock; on failure,
	 * prints a hint about the stale lock file and exits.
	 */
	const PrintListTheme c = getListColors();
#ifdef ISOCMD_TEST_PRIVATE_LOCK
	const char* sandbox = std::getenv("ISOCMD_TEST_SANDBOX");
	const char* home = std::getenv("HOME");
	if (!sandbox || !home || fs::path(home) != fs::path(sandbox) / "home") return 1;
	const std::string privateLock = (fs::path(sandbox) / "isocmd.lock").string();
	const char* lockFile = privateLock.c_str();
#else
	const char* lockFile = "/tmp/isocmd.lock";
#endif
	if (!acquireSingleInstanceLock(GlobalState::lockFileDescriptor, c, lockFile)) {
		return 1;
	}

	bool exitProgram = false;
	std::atomic<bool> search{readUserConfigUpdates(GlobalState::configPath)};

	std::vector<std::thread> backgroundThreads;

	// Shared state for background import coordination (isImportRunning,
	// stopImport, worker sync).
	std::shared_ptr<RefreshState> importState = std::make_shared<RefreshState>();

	initializeBackgroundImport(search, importState, backgroundThreads);

	bool messagePrinted = false;

	/**
	 * @brief Main interactive loop.
	 *
	 * Displays the ASCII banner, background-task status, and menu, then
	 * processes user input (menu options 1-5).
	 */
	while (!exitProgram) {
		// Prevent default line editor keybindings from interfering.
		editorBindKey('\f', ignoreEditorKeybinding);
		editorBindKey('\t', ignoreEditorKeybinding);

		GlobalState::g_operationCancelled.store(false);
		GlobalState::g_pendingRefreshKind.store(PendingRefreshKind::None);
		isAtMain.store(true);
		isAtISOList.store(false);

		/**
		 * @brief Status-line logic (runs once per iteration, not on resize
		 * repaints).
		 *
		 * Picks the background-task status to show and starts the helper
		 * threads that later clear it:
		 * 1. Auto-update running in the background (with a monitor thread).
		 * 2. No stored folder paths available for scanning (one-time message).
		 * The text is kept in @c statusLine; drawMain() only prints it.
		 */
		std::string statusLine;
		updateStatusLine(search, importState, messageActive, stopMessage,
		                 monitorThreadSpawned, isAtMain, backgroundThreads,
		                 messagePrinted, statusLine);

		// Draws the main menu screen: clear, banner, status line (while the
		// message is active), menu. Ends just after the menu's last line; the
		// blank line before the prompt comes from the caller / resize watcher.
		// Used for both the normal draw and the resize repaint; runs on the
		// Line editor thread and must not call editLine().
		auto drawMain = [&] {

			SyncOutput sync;
			clearScrollBuffer();
			print_ascii();
			if (!statusLine.empty() && messageActive.load())
			    std::cout << statusLine;
			printMenu();

		};

		drawMain();
		std::cout << "\n";   // blank line before the prompt

		editorClearHistory();

		const LineEditorAndPromptTheme pt = getPromptTheme();
		const std::string prompt = std::string(pt.primary) +
		                           "Enter choice [1-5]: " +
		                           std::string(pt.reset);
		EditorLine input = editLineWithResize(prompt, drawMain);
		if (!input) break; ///< EOF (Ctrl+D)

		/**
		 * @brief Menu dispatch.
		 * - 1: Browse/select ISOs (submenu1)
		 * - 2: Select image files for ISO conversion (submenu2)
		 * - 3: Refresh ISO database from filesystem
		 * - 4: Settings editor
		 * - 5: Exit
		 */
		dispatchMenuChoice(*input, exitProgram, isAtMain, isAtISOList,
		                   importState, backgroundThreads);
	}

	/// @name Cleanup and Resource Release
	/// Shut down the thread pool, signal background tasks to stop, and release
	/// system locks.
	/// @{
	cleanupAndExit(backgroundThreads, importState, stopMessage,
	               GlobalState::lockFileDescriptor, lockFile);
	return 0;
	/// @}

}
