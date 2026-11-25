// SPDX-License-Identifier: GPL-3.0-or-later

module;

#include <stddef.h>
#include <stdint.h>

export module isocmd.write2usb.ui.write2usbUI;

import std;
import isocmd.write2usb.util.writeTypes;

// extern "C++" keeps these declarations attached to the global module, so the
// existing .cpp files keep defining them unchanged and symbol names stay the same.
export extern "C++" {

// ---- write2usb/ui/write2usbUI ----

/**
 * Performs the actual write2usb.
 */
bool writeIsoToDevice(const std::string& isoPath, const std::string& device, size_t progressIndex, std::vector<ProgressInfo>& progressData);


/**
 * Returns true if the block device or any of its partitions is currently mounted.
 */
bool isDeviceMounted(const std::string&);

/**
 * Returns the drive's model name from sysfs, or "Unknown Drive" if unavailable.
 */
std::string getDriveName(const std::string&);

// --- Write theme forward declaration (used by WriteScreen + WriteValidation) ---

namespace UI { struct WriteTheme; }

/**
 * @brief Live progress/result screen for a write2usb operation.
 *
 * Opaque handle returned by @ref performWriteOperation. It owns the keeper
 * thread that keeps the screen laid out correctly on terminal resize, and it
 * must stay alive until the caller has finished waiting for the user. The
 * destructor stops and joins that thread. Defined in
 * write2usbUIRendering.cpp.
 */
class WriteScreen;

// --- Opaque WriteScreen interface (defined in write2usbUIRendering.cpp) ---
//
// WriteScreen is forward-declared above; these free functions are the only
// way to drive it. All are no-ops / safe to call on a stopped screen.

/**
 * @brief Constructs a WriteScreen from the shared per-task progress vector,
 *        the shared WriteSync state and the active write theme.
 *
 * The screen captures shared_ptr copies of both shared objects, so it never
 * refers to the stack of the function that created it.
 */
std::shared_ptr<WriteScreen> createWriteScreen(
        std::shared_ptr<std::vector<ProgressInfo>> progressData,
        std::shared_ptr<WriteSync> sync,
        UI::WriteTheme theme);

/** @brief Prints the header and starts the keeper / relay threads. */
void startWriteScreen(WriteScreen& screen);

/** @brief Switches to the result screen and paints it. Call once. */
void finishWriteScreen(WriteScreen& screen);

/**
 * @brief True if Ctrl+C is relayed into the shared condition variable, so a
 *        waiter on it is woken immediately on cancellation (no timed recheck).
 */
bool writeScreenCancelWakeable(const WriteScreen& screen);

/**
 * @brief Registers the program's "press Enter" prompt so that a terminal
 *        resize triggers repaintWriteScreen() instead of the keeper's own
 *        repaint path.
 */
void setWriteScreenPrompt(WriteScreen& screen, std::string prompt);

/**
 * @brief Re-emits the result screen at the current terminal width. Called by
 *        the pressEnterTo*WithRepaint() machinery on resize.
 */
void repaintWriteScreen(WriteScreen& screen);

/** @brief Stops and joins the keeper and relay threads. Safe to call twice. */
void stopWriteScreen(WriteScreen& screen);

/**
 * @brief Watches terminal resizes while the write confirmation prompt is active.
 *
 * The implementation and its signal/self-pipe state are kept in
 * write2usbUIRendering.cpp. The painter callback redraws the prompt for the
 * supplied terminal width and returns the widths of the rendered lines.
 */
class ConfirmResizeWatcher {
public:
    using Painter = std::function<std::vector<size_t>(size_t cols)>;

    ConfirmResizeWatcher(Painter paint, size_t drawnCols,
                         std::vector<size_t> drawnWidths,
                         const std::string& promptText);

    ~ConfirmResizeWatcher();

    ConfirmResizeWatcher(const ConfirmResizeWatcher&) = delete;
    ConfirmResizeWatcher& operator=(const ConfirmResizeWatcher&) = delete;
    ConfirmResizeWatcher(ConfirmResizeWatcher&&) = delete;
    ConfirmResizeWatcher& operator=(ConfirmResizeWatcher&&) = delete;

    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/**
 * Writes the given (ISO, device) pairs concurrently on the thread pool,
 * with a live progress display and a final status summary.
 */
std::shared_ptr<WriteScreen> performWriteOperation(
    const std::vector<std::pair<IsoInfo, std::string>>&);


// --- Device discovery and mapping APIs (defined in write2usbUI.cpp) ---

/** @brief Enumerates removable USB block devices visible to the system. */
std::vector<std::string> getRemovableDevices();

/**
 * @brief Validates parsed ISO-to-device mappings and returns viable pairs.
 */
std::vector<std::pair<IsoInfo, std::string>> validateDevices(
    const std::vector<std::pair<size_t, std::string>>& deviceMap,
    const std::vector<IsoInfo>& selectedIsos,
    bool noPermissions,
    std::vector<std::string>& validationErrors);

/**
 * @brief Parses semicolon-delimited INDEX>DEVICE mappings.
 */
std::vector<std::pair<size_t, std::string>> parseDeviceMappings(
    const std::string& pairString,
    const std::vector<std::string>& selectedIsos,
    std::vector<std::string>& errors);

/** @brief Runs the interactive ISO-to-device mapping and confirmation flow. */
std::vector<std::pair<IsoInfo, std::string>> collectDeviceMappings(
    const std::vector<IsoInfo>& selectedIsos);

/** @brief Entry point for the write-to-USB workflow. */
void writeToUsb(const std::string& input, const std::vector<std::string>& isoFiles);

// --- Filesystem & Formatting Utilities ---

/**
 * Converts a raw byte count into a human-readable string (KB, MB, GB).
 */
std::string formatFileSize(uint64_t size);

/**
 * Formats a speed value into a readable string (e.g., "15.4 MB/s").
 */
std::string formatSpeed(double mbPerSec);

/**
 * Queries the kernel for the total capacity of a block device.
 * @param device Path to the device (e.g., "/dev/sdb").
 * @return Size in bytes.
 */
uint64_t getBlockDeviceSize(const std::string& device);

// --- Shared validation helpers (defined in the same .cpp as validateDevices) ---

/**
 * @namespace WriteValidation
 * @brief Shared validation helpers used by the interactive write2usb UI and
 *        the non-interactive CLI writer.
 *
 * Defined in write2usbUI.cpp alongside validateDevices().
 */
namespace WriteValidation {

/**
 * @brief Builds a themed error message that has no associated path.
 *
 * @param wt  Active write theme supplying the label and reset colours.
 * @param msg Message text (e.g. "Root privileges are required").
 * @return The message prefixed with the error label colour and terminated
 *         with a colour reset.
 */
std::string plainErr(const UI::WriteTheme& wt, const std::string& msg);

/**
 * @brief Builds a themed error message of the form "'<path>' <message>".
 *
 * @param wt   Active write theme.
 * @param path Path to display, quoted, at the start of the message.
 * @param msg  Message text; include a leading space (e.g. " is not an ISO file").
 * @return The fully coloured message string.
 */
std::string pathErr(const UI::WriteTheme& wt, const std::string& path, const std::string& msg);

/**
 * @brief Prints a themed "Validation errors:" block to stderr.
 *
 * @param wt   Active write theme.
 * @param errs Pre-formatted error messages, printed as indented bullets.
 */
void printValidationErrors(const UI::WriteTheme& wt, const std::vector<std::string>& errs);

/**
 * @brief Runs the device-level checks, independent of which ISO is paired with it.
 *
 * Verifies that the device is a removable USB flash drive, is not mounted,
 * is not in the draining set (pending kernel-level tasks), has no write I/O
 * still in flight (non-interactive mode only), and has a determinable size.
 *
 * @param wt     Active write theme.
 * @param device Path used for the checks (canonical /dev/sdX).
 * @param shown  Path displayed in error messages (what the user typed).
 * @param[out] size Device size in bytes; only valid when no error is returned.
 * @return An error message for the first failed check, or std::nullopt if
 *         the device is usable.
 */
std::optional<std::string> checkDevice(const UI::WriteTheme& wt, const std::string& device,
                                       const std::string& shown, uint64_t& size);

/**
 * @brief Checks that an ISO fits on a device.
 *
 * @param wt         Active write theme.
 * @param iso        ISO to be written.
 * @param device     Device path, used for the message and the drive-name lookup.
 * @param deviceSize Device capacity in bytes (as returned by checkDevice()).
 * @return A themed "is too large for" message including both sizes and the
 *         drive name, or std::nullopt if the ISO fits.
 */
std::optional<std::string> checkCapacity(const UI::WriteTheme& wt, const IsoInfo& iso,
                                         const std::string& device, uint64_t deviceSize);

} // namespace WriteValidation

} // extern "C++"
