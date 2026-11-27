// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <signal.h>
#include "../common/testHarness.h"
import isocmd.input.lineEditor;
import isocmd.core.cancellation;
import isocmd.input.inputHandling;
import isocmd.input.promptCompletion;
import isocmd.settings.settings;
import isocmd.ui.display.themes;
import isocmd.write2usb.ui.write2usbUI;
import isocmd.write2usb.util.writeTypes;
import isocmd.write2usb.util.drainingState;

namespace isocmd_tests {
	namespace {
		using Pairs=std::vector<std::pair<IsoInfo,std::string>>;
		using namespace std::chrono_literals;

		std::vector<std::string> fakeDevices() {

			return {"/dev/null","/dev/zero","/dev/full"};

		}

		Pairs fakeValidate(const std::vector<std::pair<std::size_t,std::string>>& map,
		                   const std::vector<IsoInfo>& isos, bool denied, std::vector<std::string>& errors) {

			Pairs result;
			const auto devices=fakeDevices();
			for (const auto& [index,device] : map) {
				if (denied || index<1 || index>isos.size() || std::find(devices.begin(),devices.end(),device)==devices.end()) {
					errors.emplace_back("fixture validation rejected mapping"); continue;
				}
				result.emplace_back(isos[index-1],device);
			}
			return result;

		}

		std::mutex recordMutex;
		std::vector<std::tuple<std::size_t,std::string,std::string>> calls;
		std::atomic<int> mode{0};
		std::atomic<bool> expectVerification{false};

		bool fakeWriter(const std::string& source,const std::string& device,std::size_t index,std::vector<ProgressInfo>& progress) {

			// Never open a device. A wrong row association is recorded as a failed call.
			{
				std::lock_guard lock(recordMutex);
				calls.emplace_back(index,source,device);
			}
			auto& row=progress.at(index);
			if (row.sourcePath!=source || row.device!=device || row.verifyRequested != expectVerification.load()) return false;
			row.progress=50; row.speed=12.3; row.bytesWritten=100;
			// The slowest device has distinct write and verification phases.
			std::this_thread::sleep_for(device=="/dev/zero" ? 200ms : 10ms);
			if (mode==2) return false;
			row.progress=100; row.speed=20.0; row.bytesWritten=200;
			row.writeCompleted=true;
			if (row.verifyRequested) {
				row.verificationSpeed=100.0; row.verifying = true; row.progress=0;
				std::this_thread::sleep_for(device=="/dev/zero" ? 400ms : 10ms);
				row.progress=100; row.bytesVerified=200;
			}
			if (mode==1 && device=="/dev/zero") return false;
			return true;

		}

		std::binary_semaphore resumeCancelledWriter{0};
		std::atomic<bool> cancelledAfterReset{false}, bufferedSnapshot{false};

		bool blockedWriter(const std::string&, const std::string&, std::size_t index,
		                   std::vector<ProgressInfo>& progress) {
			auto& row = progress.at(index);
			raise(SIGINT);
			resumeCancelledWriter.acquire();
			cancelledAfterReset = isUsbWriteCancelled(row.cancelToken);
			bufferedSnapshot = row.bufferedWrites == true;
			return false;
		}

		bool freshWriter(const std::string&, const std::string&, std::size_t index,
		                 std::vector<ProgressInfo>& progress) {
			const auto& row = progress.at(index);
			return !isUsbWriteCancelled(row.cancelToken) && row.bufferedWrites == false;
		}

		void checkAsyncCancellation(const Pairs& mappings) {
			setupSignalHandlerCancellations();
			GlobalState::g_operationCancelled = false;
			ConfigCaches::g_configCache["buffered_writes"] = "on";
			bool succeeded = true;
			const Pairs single{mappings.front()};
			auto screen = WriteExecution::performWithWriter(single, blockedWriter, &succeeded, false);
			const bool reserved = g_drainingManager.isDeviceDraining(single.front().second);
			stopWriteScreen(*screen);
			GlobalState::g_operationCancelled = false;
			ConfigCaches::g_configCache["buffered_writes"] = "off";
			// Another device can finish before the old writer is released.
			setupSignalHandlerCancellations();
			bool nextSucceeded = false;
			const Pairs next{mappings.at(1)};
			auto nextScreen = WriteExecution::performWithWriter(next, freshWriter, &nextSucceeded, false);
			stopWriteScreen(*nextScreen);
			const bool stillReserved = g_drainingManager.isDeviceDraining(single.front().second);
			resumeCancelledWriter.release();
			g_drainingManager.joinAll();
			CHECK(!succeeded && reserved && stillReserved && nextSucceeded);
			CHECK(cancelledAfterReset && bufferedSnapshot);
			CHECK(!g_drainingManager.isDeviceDraining(single.front().second));
			CHECK(!g_drainingCancelled.load());
			std::cout << "TEST-ASYNC-CANCELLATION-PASS\n" << std::flush;
		}

		void waitInput() {

			CHECK(std::cin.get()=='\n');

		}

		// A blocking std::cin.get() can defer SIGWINCH under TSan (libc's internal
		// read is not necessarily intercepted). Keep this fixture at intercepted
		// syscall boundaries so the real signal handler/relay can repaint while
		// waiting for the parent. Never request a repaint directly from the test.
		void waitProgressInput() {

			const auto deadline = std::chrono::steady_clock::now() + 90s;
			while (std::chrono::steady_clock::now() < deadline) {
				struct pollfd pfd{STDIN_FILENO, POLLIN, 0};
				const int ready = ::poll(&pfd, 1, 50);
				if (ready < 0 && errno == EINTR) continue;
				CHECK(ready >= 0);
				if (ready == 0) continue;
				CHECK(!(pfd.revents & POLLNVAL));
				if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
					char input{};
					const auto count = ::read(STDIN_FILENO, &input, 1);
					if (count < 0 && errno == EINTR) continue;
					CHECK(count == 1 && input == '\n');
					return;
				}
			}
			throw std::runtime_error("timed out waiting for progress fixture input");

		}
	}

	int writeUiChild() {

		try {
			setupLineEditorToIgnoreCtrlC(); editorBindKeySequence("\033",exit_handler);
			getskin(); UI::Palette::updateDefaultColors();
			const std::vector<IsoInfo> isos{{"/fixture/iso10.iso","iso10.iso",100,"100 B",1},
				                            {"/fixture/iso2.iso","iso2.iso",200,"200 B",2}};
			auto twoDevices=[] { return std::vector<std::string>{"/dev/zero","/dev/null"}; };
			auto cancelled=WriteMapping::collectWithDevices(isos,twoDevices,fakeValidate,false);
			CHECK(cancelled.empty());
			std::cout<<"TEST-MAPPING-CANCELLED\n"<<std::flush;
			ConfigCaches::g_configCache["verify_checksum"] = "on";
			auto mappings=WriteMapping::collectWithDevices(isos,fakeDevices,fakeValidate,false);
			CHECK(write2usbChecksumEnabled());
			CHECK(mappings.size()==3);
			CHECK(mappings[0].first.filename=="iso2.iso" && mappings[0].second=="/dev/zero");
			CHECK(mappings[1].first.filename=="iso10.iso" && mappings[1].second=="/dev/null");
			CHECK(mappings[2].first.filename=="iso2.iso" && mappings[2].second=="/dev/full");
			const std::vector<std::tuple<std::size_t,std::string,std::string>> expected{
				{0,"/fixture/iso2.iso","/dev/full"},{1,"/fixture/iso2.iso","/dev/zero"},{2,"/fixture/iso10.iso","/dev/null"}};
			for (int result=0;result<4;++result) {
				mode=result==3?0:result; calls.clear();
				setupSignalHandlerCancellations(); GlobalState::g_operationCancelled=false;
				const bool expectedSuccess = result==0 || result==3;
				bool allSucceeded = !expectedSuccess; // Start with the opposite expected result.
				if (result==3) ConfigCaches::g_configCache["verify_checksum"] = "off";
				expectVerification = result!=3;
				auto screen=WriteExecution::performWithWriter(mappings,fakeWriter,&allSucceeded,write2usbChecksumEnabled());
				CHECK(allSucceeded == expectedSuccess);
				std::sort(calls.begin(),calls.end()); CHECK(calls==expected);
				stopWriteScreen(*screen);
				std::cout<<"TEST-WRITE-RESULT-"<<result<<'\n'<<std::flush;
				waitInput();
			}
			checkAsyncCancellation(mappings);
			std::cout<<"TEST-VERIFY-OFF\n"<<std::flush;
			auto skipped=WriteMapping::collectWithDevices(isos,fakeDevices,fakeValidate,false);
			CHECK(skipped.size()==2 && !write2usbChecksumEnabled());
			CHECK(skipped[0].first.filename=="iso2.iso" && skipped[0].second=="/dev/zero");
			CHECK(skipped[1].first.filename=="iso10.iso" && skipped[1].second=="/dev/null");
			std::cout<<"TEST-WRITE-UI-PASS\n"<<std::flush;
			return 0;
		} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }

	}

	int progressScreenChild() {

		try {
			getskin(); UI::Palette::updateDefaultColors();
			auto rows=std::make_shared<std::vector<ProgressInfo>>();
			rows->emplace_back("very-long-fixture-image-for-resize-testing.iso","/dev/null","1 MB","fixture1");
			rows->emplace_back("second-fixture.iso","/dev/zero","1 MB","fixture2");
			rows->emplace_back("third-fixture.iso","/dev/full","1 MB","fixture3");
			(*rows)[0].progress=10; (*rows)[0].speed=0.05;
			(*rows)[1].progress=50; (*rows)[1].speed=20.0; (*rows)[1].verificationSpeed=12.3;
			for (auto& row : *rows) row.verifyRequested=true;
			(*rows)[1].writeCompleted=true; (*rows)[1].verifying=true;
			(*rows)[2].progress=99; (*rows)[2].speed=40;
			auto sync=std::make_shared<WriteSync>();
			setupSignalHandlerCancellations(); GlobalState::g_operationCancelled=false;
			auto screen=createWriteScreen(rows,sync,getWriteTheme());
			startWriteScreen(*screen);
			const auto batchStart=std::chrono::steady_clock::now();
			waitProgressInput();
			// Exercise single-digit and 100% progress before the final result.
			(*rows)[0].progress=5;
			(*rows)[1].verificationSpeed=179.8; (*rows)[1].progress=100;
			waitProgressInput();
			(*rows)[0].progress=100; (*rows)[0].writeCompleted=true;
			(*rows)[0].verificationSpeed=100.0; (*rows)[0].verifying=true; (*rows)[0].completed=true;
			(*rows)[1].failed=true;
			// Third device finished writing, but its readback was cancelled.
			(*rows)[2].verificationSpeed=40; (*rows)[2].writeCompleted=true; (*rows)[2].verifying=true;
			sync->completedTasks=1; sync->finishedTasks=2;
			GlobalState::g_operationCancelled=true;
			finishWriteScreen(*screen,std::chrono::steady_clock::now()-batchStart);
			waitProgressInput();
			stopWriteScreen(*screen); stopWriteScreen(*screen);
			GlobalState::g_operationCancelled=false;
			std::cout<<"TEST-PROGRESS-PASS\n"<<std::flush;
			return 0;
		} catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }

	}
} // namespace isocmd_tests
