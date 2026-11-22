// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

// Project Headers
#include "isocmd/core/state.h"
#include "isocmd/ops/cpMvRm.h"

/**
 * @brief Performs a buffered file copy with progress and cancellation support.
 *
 * Writes to a temporary ".part" file and renames it to the destination only
 * after the copy has completed successfully. The existing destination is
 * therefore left intact if the copy fails or is canceled.
 *
 * @param src Source file.
 * @param dst Destination file.
 * @param completedBytes Optional atomic progress counter.
 * @param ec Receives the failure reason.
 * @return True if the copy succeeded.
 */
bool bufferedCopyWithProgress(
    const fs::path& src,
    const fs::path& dst,
    std::atomic<std::size_t>* completedBytes,
    std::error_code& ec
)
{
    constexpr std::size_t kBufferSize = 4 * 1024 * 1024;

    std::vector<char> inBuf(kBufferSize);
    std::vector<char> outBuf(kBufferSize);

    if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        ec = std::make_error_code(std::errc::operation_canceled);
        return false;
    }

    std::ifstream input;

    if (auto* sb = input.rdbuf()) {
        sb->pubsetbuf(
            inBuf.data(),
            static_cast<std::streamsize>(inBuf.size())
        );
    }

    input.open(src, std::ios::binary);

    if (!input) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }

    fs::path tmp = dst;
    tmp += ".part";

    std::ofstream output;

    if (auto* sb = output.rdbuf()) {
        sb->pubsetbuf(
            outBuf.data(),
            static_cast<std::streamsize>(outBuf.size())
        );
    }

    output.open(tmp, std::ios::binary);

    if (!output) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    auto discardTemp = [&]() {
        output.close();

        std::error_code rmEc;
        fs::remove(tmp, rmEc);
    };

    // Expected size, used to detect a source that ended early without the
    // stream reporting an error. If it cannot be determined, skip that check.
    std::error_code sizeEc;
    const std::uintmax_t expectedSize = fs::file_size(src, sizeEc);

    std::uintmax_t totalCopied = 0;

    while (!GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        input.read(
            inBuf.data(),
            static_cast<std::streamsize>(inBuf.size())
        );

        const std::streamsize bytesRead = input.gcount();

        // badbit means a real read error, not a normal end of file. Any bytes
        // already read are not trusted: the copy must fail.
        if (input.bad()) {
            ec = std::make_error_code(std::errc::io_error);
            discardTemp();
            return false;
        }

        if (bytesRead == 0) {
            break;
        }

        output.write(inBuf.data(), bytesRead);

        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
            discardTemp();
            return false;
        }

        totalCopied += static_cast<std::uintmax_t>(bytesRead);

        if (completedBytes) {
            completedBytes->fetch_add(
                static_cast<std::size_t>(bytesRead),
                std::memory_order_relaxed
            );
        }
    }

    if (GlobalState::g_operationCancelled.load(std::memory_order_acquire)) {
        ec = std::make_error_code(std::errc::operation_canceled);
        discardTemp();
        return false;
    }

    output.close();

    if (!output) {
        ec = std::make_error_code(std::errc::io_error);
        discardTemp();
        return false;
    }

    // Never publish a short copy: a cross-device move deletes the source
    // after this function reports success.
    if (!sizeEc && totalCopied != expectedSize) {
        ec = std::make_error_code(std::errc::io_error);
        discardTemp();
        return false;
    }

    std::error_code renameEc;
    fs::rename(tmp, dst, renameEc);

    if (renameEc) {
        ec = renameEc;
        discardTemp();
        return false;
    }

    return true;
}


/**
 * @brief Buffered copy to a single destination, with reporting and ownership.
 *
 * Performs a buffered copy from @p paths.srcPath to @p paths.destPath. Used
 * for plain copies ("copying") and for each destination of a multi-destination
 * move ("moving"). It does not attempt a fast rename and does not delete the
 * source. Source cleanup for multi-destination moves is handled by the caller
 * once at least one copy succeeds.
 *
 * Ownership of the destination file is changed to the real user on success.
 *
 * @param ctx  Operation context for reporting, counters, and ownership.
 * @param paths Source/destination paths and display strings.
 * @param verb Present-tense verb used in log messages ("copying" or "moving").
 * @return True if the copy succeeded, false otherwise.
 */
bool performBufferedCopyOperation(
    OperationContext& ctx,
    const TransferPaths& paths,
    std::string_view verb
)
{
    std::error_code ec;

    const bool success = bufferedCopyWithProgress(
        paths.srcPath,
        paths.destPath,
        ctx.completedBytes,
        ec
    );

    if (success && ctx.changeOwnership) {
        ctx.changeOwnership(paths.destPath);
    }

    logOperationResult(
        ctx,
        success,
        GlobalState::g_operationCancelled.load(),
        ec,
        verb,
        paths.srcDir,
        paths.srcFile,
        paths.destDirProcessed,
        paths.destFile
    );

    return success;
}
