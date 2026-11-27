// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief USB cancellation checks and progress row construction and moves.
 */

import std;
import isocmd.core.cancellation;
import isocmd.write2usb.util.writeTypes;

bool isUsbWriteCancelled(const std::stop_token& token) noexcept {
	// Read the UI flag first: observing its reset must not be followed by
	// a stale pre-latch token check.
	return GlobalState::g_operationCancelled.load() || token.stop_requested();
}

ProgressInfo::ProgressInfo(std::string filename, std::string device, std::string totalSize,
                           std::string sourcePath)
    : filename(std::move(filename)),
      device(std::move(device)),
      totalSize(std::move(totalSize)),
      sourcePath(std::move(sourcePath)) {}

ProgressInfo::ProgressInfo(ProgressInfo&& other) noexcept
    : filename(std::move(other.filename)),
      device(std::move(other.device)),
      totalSize(std::move(other.totalSize)),
      sourcePath(std::move(other.sourcePath)),
      cancelToken(std::move(other.cancelToken)),
      bufferedWrites(other.bufferedWrites),
      verifyRequested(other.verifyRequested),
      verifying(other.verifying.load()),
      bytesVerified(other.bytesVerified.load()),
      writeCompleted(other.writeCompleted.load()),
      completed(other.completed.load()),
      failed(other.failed.load()),
      bytesWritten(other.bytesWritten.load()),
      progress(other.progress.load()),
      speed(other.speed.load()),
      verificationSpeed(other.verificationSpeed.load()) {}

ProgressInfo& ProgressInfo::operator=(ProgressInfo&& other) noexcept {

	if (this != &other) {
		filename = std::move(other.filename);
		device = std::move(other.device);
		totalSize = std::move(other.totalSize);
		sourcePath = std::move(other.sourcePath);
		cancelToken = std::move(other.cancelToken);
		bufferedWrites = other.bufferedWrites;
		verifyRequested = other.verifyRequested;
		verifying.store(other.verifying.load());
		bytesVerified.store(other.bytesVerified.load());
		writeCompleted.store(other.writeCompleted.load());
		completed.store(other.completed.load());
		failed.store(other.failed.load());
		bytesWritten.store(other.bytesWritten.load());
		progress.store(other.progress.load());
		speed.store(other.speed.load());
		verificationSpeed.store(other.verificationSpeed.load());
	}
	return *this;

}
