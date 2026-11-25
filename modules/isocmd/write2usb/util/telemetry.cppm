// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <stdint.h>

export module isocmd.write2usb.util.telemetry;
import std;

export namespace write_telemetry {
	using Clock = std::chrono::high_resolution_clock;
	inline constexpr int rawStallTimeoutSeconds = 60;
	inline constexpr int windowsStallTimeoutSeconds = 300;

	/**
	 * @brief Exponentially smoothed write-speed estimator with stall handling.
	 *
	 * Fed periodically with the cumulative byte count. Samples closer than one
	 * second apart are ignored. While bytes keep arriving it publishes a smoothed
	 * MB/s figure (heavier smoothing below 10 MB/s). If nothing arrives it holds
	 * the last figure, and reports 0 after the configured timeout without progress.
	 *
	 * @par Utility
	 * USB drives write in bursts, so the raw speed jumps up and down and would
	 * flicker on screen. This evens it out, keeps showing the last speed during short
	 * pauses, and shows 0 after the configured stall timeout.
	 */
	class SpeedEstimator {
	public:
		/**
		 * @brief Creates an estimator whose baseline is the transfer start.
		 *
		 * @par Utility
		 * Starting the clock at the real start time makes the first measurement
		 * accurate.
		 * @param start Time the transfer started (initial baseline).
		 */
		explicit SpeedEstimator(Clock::time_point start, int stallTimeoutSeconds)
		    : stallTimeoutSeconds_(stallTimeoutSeconds), lastUpdate_(start), stallStart_(start) {}

		/**
		 * @brief Feeds one observation.
		 *
		 * @par Utility
		 * It is the single place where measurements are fed in. It ignores updates that
		 * arrive too quickly and picks the right handling for "data was written" or
		 * "nothing was written".
		 *
		 * @param currentWritten Cumulative bytes written so far.
		 * @param now            Time of the observation.
		 * @return Speed in MB/s to publish, or std::nullopt if nothing should change.
		 */
		std::optional<double> sample(uint64_t currentWritten, Clock::time_point now) {

			const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			                           now - lastUpdate_).count();
			if (elapsedMs < kMinUpdateIntervalMs)
			    return std::nullopt;

			const uint64_t delta = (currentWritten >= lastWritten_) ? (currentWritten - lastWritten_) : 0;
			return delta > 0 ? onProgress(currentWritten, delta, elapsedMs, now)
			                 : onNoProgress(now);

		}

	private:
		/// Smoothing factor for normal speeds (0.2 is used below 10 MB/s).
		static constexpr double kAlpha = 0.3;
		/// Minimum time between estimates.
		static constexpr int kMinUpdateIntervalMs = 1000;
		/// Time without progress after which the speed is reported as 0.
		const int stallTimeoutSeconds_;

		/**
		 * @brief Handles a sample in which bytes arrived.
		 *
		 * Computes the instantaneous speed over the window since the last real
		 * data point, folds it into the smoothed value and resets the baseline.
		 *
		 * @par Utility
		 * Works out the speed from what was written since the last measurement, and
		 * blends it with earlier values so the number changes smoothly.
		 *
		 * @param currentWritten Cumulative bytes written.
		 * @param delta          Bytes written since the last baseline (> 0).
		 * @param elapsedMs      Milliseconds since the last baseline.
		 * @param now            Time of the observation.
		 * @return The new smoothed speed in MB/s.
		 */
		double onProgress(uint64_t currentWritten, uint64_t delta, long long elapsedMs,
		                  Clock::time_point now) {

			stalled_ = false;

			const double instant = (static_cast<double>(delta) / (1024.0 * 1024.0)) / (elapsedMs / 1000.0);
			// Heavier smoothing for slow drives (< 10 MB/s) to reduce display
			// jitter from bursty O_DIRECT writes at low throughput.
			const double factor = (instant < 10.0) ? 0.2 : kAlpha;

			smoothed_ = haveEstimate_ ? (factor * instant + (1.0 - factor) * smoothed_) : instant;
			haveEstimate_ = true;

			// Only reset the baseline when bytes actually arrived — this keeps
			// `elapsed` spanning back to the last real data point instead of an
			// arbitrary polling tick, which would truncate the window to ~1s and
			// produce a spurious floor (one write chunk per interval) on
			// slow/bursty O_DIRECT writers.
			lastWritten_ = currentWritten;
			lastUpdate_  = now;
			return smoothed_;

		}

		/**
		 * @brief Handles a sample in which no bytes arrived.
		 *
		 * Starts (or continues) stall tracking and leaves the baseline untouched
		 * so the next real sample spans the full transfer duration.
		 *
		 * @par Utility
		 * Handles the case where nothing new was written. It waits, keeps the last
		 * speed on screen, and only reports 0 after the configured timeout. The
		 * measuring window keeps growing meanwhile, so a slow write is judged fairly.
		 *
		 * @param now Time of the observation.
		 * @return 0.0 once the stall timeout is reached; otherwise std::nullopt
		 *         (keep showing the last speed).
		 */
		std::optional<double> onNoProgress(Clock::time_point now) {

			if (!stalled_) {
				stalled_    = true;
				stallStart_ = now;
			}
			const auto stallSeconds = std::chrono::duration_cast<std::chrono::seconds>(
			                              now - stallStart_).count();
			// Deliberately do NOT touch lastWritten_/lastUpdate_ here — let the
			// elapsed window keep growing while a slow write is still in flight,
			// so the eventual speed sample reflects the true transfer duration.
			if (stallSeconds >= stallTimeoutSeconds_)
			    return 0.0;
			return std::nullopt;

		}

		Clock::time_point lastUpdate_;
		Clock::time_point stallStart_;
		uint64_t lastWritten_  = 0;
		double   smoothed_     = 0.0;
		bool     haveEstimate_ = false;
		bool     stalled_      = false;
	};

	// The writers reserve 100% for successful completion after flushing.
	inline int transferPercent(uint64_t written, uint64_t total) {

		return static_cast<int>(std::min(99.0, (static_cast<double>(written) / total) * 100.0));

	}
} // namespace write_telemetry
