// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

namespace isocmd_tests {
    void backgroundImport(); void filesystemOperations(); void directoryTraversal(); void cancellation(); void activeCancellation(); void usbWriter(); void windowsIoRecovery(); void windowsIoFailures(); void windowsCleanupFailures(); void windowsFinalWriteback(); int terminalChild(); int terminalTest(const char* binary); int menuTest(const char* binary); int workflowTest(const char* binary); int writeUiChild(); int progressScreenChild(); int writeUiTest(const char* binary); int progressScreenTest(const char* binary);
}
int main(int argc,char** argv) {
    using namespace isocmd_tests;
    if (argc==2 && std::string_view(argv[1])=="--terminal-child") return terminalChild();
    if (argc==2 && std::string_view(argv[1])=="--terminal-test") return terminalTest(argv[0]);
    if (argc==3 && std::string_view(argv[1])=="--menu-test") return menuTest(argv[2]);
    if (argc==3 && std::string_view(argv[1])=="--workflow-test") return workflowTest(argv[2]);
    if (argc==2 && std::string_view(argv[1])=="--write-ui-child") return writeUiChild();
    if (argc==2 && std::string_view(argv[1])=="--write-ui-test") return writeUiTest(argv[0]);
    if (argc==2 && std::string_view(argv[1])=="--progress-child") return progressScreenChild();
    if (argc==2 && std::string_view(argv[1])=="--progress-test") return progressScreenTest(argv[0]);
    if (argc!=1) return 2;
    return runTests("integration", {{"copy, move, delete and failure cleanup",filesystemOperations},
        {"directory traversal and depth",directoryTraversal},{"SIGINT cancellation and recovery",cancellation}, {"active copy cancellation and recovery",activeCancellation},
        {"USB writer validation and Windows copy engine",usbWriter},
        {"Windows copy I/O retries and short transfers",windowsIoRecovery},
        {"Windows copy I/O failures, cancellation and recovery",windowsIoFailures},
        {"Windows copy cleanup syscall failures",windowsCleanupFailures},
        {"Windows final range writeback retries",windowsFinalWriteback}, {"background import, deduplication and restart",backgroundImport}});
}
