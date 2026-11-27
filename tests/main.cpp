// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/testHarness.h"

namespace isocmd_tests {
	void filterGrammar();
	void filterRandom();
	void selection();
	void sortingAndViews();
	void configuration();
	void themeAndWidth();
	void listRendering();
	void paginationCommands();
	void isoDetection();
	void conversion();
	void progressAndBuffer();
	void checksumVerification();
	void latchedReadbackCancellation();
	void threadPool();
	void taskLifetime();
	void database();
	void deviceDiscovery();
	void usbBusyIndicators();
	void usbBusyValidation();
	void usbInflightParsing();
	void historyPersistence();
	void writeTelemetry();
	void atomicScopeGuards();
	void selectionScopeGuards();
	void usbMappingValidation();
	void usbMappingCompletion();
}

int main() {

	using namespace isocmd_tests;
	return runTests("unit", {
		{"filter grammar", filterGrammar}, {"randomized filter oracle", filterRandom},
		{"selection parsing", selection}, {"natural sorting and views", sortingAndViews},
		{"configuration and self-repair", configuration}, {"themes and text width", themeAndWidth},
		{"ISO signature detection", isoDetection}, {"CCD and NRG conversion", conversion},
		{"progress state and aligned buffers", progressAndBuffer},
		{"CRC-64 and readback verification", checksumVerification},
		{"latched readback cancellation and progress moves", latchedReadbackCancellation},
		{"thread pool stress and shutdown", threadPool},
		{"move-only task ownership and lifetime", taskLifetime},
		{"database persistence and concurrent updates", database},
		{"USB discovery and mount classification", deviceDiscovery},
		{"USB mounted and busy indicators", usbBusyIndicators},
		{"USB busy rejection in interactive and CLI modes", usbBusyValidation},
		{"USB in-flight counter parsing", usbInflightParsing},
		{"line-editor history persistence and limits", historyPersistence},
		{"list rendering, pagination and pending indices", listRendering},
		{"pagination command boundaries and jumps", paginationCommands},
		{"USB speed, stall and progress telemetry", writeTelemetry},
		{"USB mapping grammar and validation", usbMappingValidation},
		{"USB mapping assignment-aware completion", usbMappingCompletion},
		{"atomic scope guards, nesting and release", atomicScopeGuards},
		{"selection scope cleanup and unwinding", selectionScopeGuards},
	});

}
