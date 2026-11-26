// SPDX-License-Identifier: GPL-3.0-or-later

export module isocmd.write2usb.util.telemetry;
import std;

export namespace write_telemetry {
	using Clock = std::chrono::high_resolution_clock;
	inline constexpr int rawStallTimeoutSeconds = 60;
	inline constexpr int windowsStallTimeoutSeconds = 300;

	/**
	 * @brief Smooths transfer-speed readings so bursty USB I/O does not make the display flicker.
	 *
	 * Samples less than one second apart are ignored. New measurements are blended
	 * with earlier ones, with stronger smoothing below 10 MiB/s. During a pause,
	 * retain the last estimate until the stall timeout, then report zero.
	 */
	class SpeedEstimator {
	public:
		/// Starts measuring from the beginning of the transfer.
		explicit SpeedEstimator(Clock::time_point start, int stallTimeoutSeconds)
		    : stallTimeoutSeconds_(stallTimeoutSeconds), lastUpdate_(start), stallStart_(start) {}

		/**
		 * @brief Updates the speed estimate from a cumulative byte count and timestamp.
		 * @return Speed in MiB/s, or std::nullopt to retain the previous display.
		 */
		std::optional<double> sample(std::uint64_t currentWritten, Clock::time_point now) {

			const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
			                           now - lastUpdate_).count();
			if (elapsedMs < kMinUpdateIntervalMs)
			    return std::nullopt;

			const std::uint64_t delta = (currentWritten >= lastWritten_) ? (currentWritten - lastWritten_) : 0;
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
		 * @brief Blends the measured speed with the previous estimate and resets the baseline.
		 * @param delta Bytes written since the previous baseline (> 0).
		 * @param elapsedMs Milliseconds since that baseline.
		 */
		double onProgress(std::uint64_t currentWritten, std::uint64_t delta, long long elapsedMs,
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
		 * @brief Tracks stalls without resetting the measurement baseline.
		 * @return Zero after the configured timeout; otherwise std::nullopt.
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
		std::uint64_t lastWritten_  = 0;
		double   smoothed_     = 0.0;
		bool     haveEstimate_ = false;
		bool     stalled_      = false;
	};

	// The writers reserve 100% for successful completion after flushing.
	inline int transferPercent(std::uint64_t written, std::uint64_t total) {

		return static_cast<int>(std::min(99.0, (static_cast<double>(written) / total) * 100.0));

	}
} // namespace write_telemetry
