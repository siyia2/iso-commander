// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

namespace isocmd_tests {
	void backgroundImport(); void filesystemOperations(); void directoryTraversal(); void cancellation(); void activeCancellation(); void usbWriter(); void windowsIoRecovery(); void windowsDoubleBuffering(); void windowsLatchedCancellation(); void readbackLatchedDuringIo(); void windowsIoFailures(); void windowsCleanupFailures(); void windowsDirectRecovery(); void windowsFilesystemWriteModes(); void windowsBufferedWrites(); int terminalChild(); int terminalTest(const char* binary); int menuTest(const char* binary); int workflowTest(const char* binary); int settingsWorkflowTest(const char* binary); int themeWorkflowTest(const char* binary); int importWorkflowTest(const char* binary); int conversionWorkflowTest(const char* binary); int writeUiChild(); int progressScreenChild(); int writeUiTest(const char* binary); int progressScreenTest(const char* binary);
}

int main(int argc,char** argv) {

	using namespace isocmd_tests;
	if (argc == 2) {
		const std::pair<std::string_view, int(*)()> children[] = {
			{"--terminal-child", terminalChild},
			{"--write-ui-child", writeUiChild},
			{"--progress-child", progressScreenChild},
		};
		for (const auto& [option, run] : children)
			if (argv[1] == option) return run();
	}
	const std::pair<std::string_view, int(*)(const char*)> selfTests[] = {
		{"--terminal-test", terminalTest},
		{"--write-ui-test", writeUiTest},
		{"--progress-test", progressScreenTest},
	};
	const std::pair<std::string_view, int(*)(const char*)> menuTests[] = {
		{"--menu-test", menuTest},
		{"--workflow-test", workflowTest},
		{"--settings-workflow-test", settingsWorkflowTest},
		{"--theme-workflow-test", themeWorkflowTest},
		{"--import-workflow-test", importWorkflowTest},
		{"--conversion-workflow-test", conversionWorkflowTest},
	};
	if (argc == 2) {
		for (const auto& [option, run] : selfTests)
			if (argv[1] == option) return run(argv[0]);
	} else if (argc == 3) {
		for (const auto& [option, run] : menuTests)
			if (argv[1] == option) return run(argv[2]);
	}
	if (argc!=1) return 2;
	return runTests("integration", {{"copy, move, delete and failure cleanup",filesystemOperations},
		{"directory traversal and depth",directoryTraversal},{"SIGINT cancellation and recovery",cancellation}, {"active copy cancellation and recovery",activeCancellation},
		{"USB writer validation and Windows copy engine",usbWriter},
		{"Windows copy I/O retries and short transfers",windowsIoRecovery},
		{"Windows double-buffered read/write overlap",windowsDoubleBuffering},
		{"Windows cancellation survives UI reset",windowsLatchedCancellation},
		{"readback cancellation survives UI reset during I/O",readbackLatchedDuringIo},
		{"Windows copy I/O failures, cancellation and recovery",windowsIoFailures},
		{"Windows copy cleanup syscall failures",windowsCleanupFailures},
		{"Windows FAT32 and NTFS direct writes",windowsFilesystemWriteModes},
		{"Windows FAT32 and NTFS buffered writes",windowsBufferedWrites},
		{"Windows FAT32 and NTFS direct I/O recovery",windowsDirectRecovery}, {"background import, deduplication and restart",backgroundImport}});

}
