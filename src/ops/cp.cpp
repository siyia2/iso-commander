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
#include "isocmd/core/cancellation.h"
#include "isocmd/ops/cpMvRm.h"

/**
 * @brief Checks whether the global operation cancellation flag is set.
 *
 * @return True if cancellation has been requested, false otherwise.
 */
static bool isCopyCancelled()
{
    return GlobalState::g_operationCancelled.load(std::memory_order_acquire);
}

/**
 * @brief Closes the temporary output stream and removes the partial file.
 *
 * @param output Output file stream associated with the temporary file.
 * @param tmp Path to the temporary ".part" file to remove.
 */
static void discardTemporaryFile(std::ofstream& output, const fs::path& tmp)
{
    output.close();

    std::error_code rmEc;
    fs::remove(tmp, rmEc);
}

/**
 * @brief Opens a binary file stream using a caller-supplied buffer.
 *
 * @tparam StreamT Stream type (`std::ifstream` or `std::ofstream`).
 * @param stream Stream instance to configure and open.
 * @param path Filesystem path to open.
 * @param buffer Backing buffer used via `pubsetbuf`.
 * @return True if the stream was opened successfully, false otherwise.
 */
template <typename StreamT>
static bool openBufferedBinaryStream(
    StreamT& stream,
    const fs::path& path,
    std::vector<char>& buffer
)
{
    if (auto* sb = stream.rdbuf()) {
        sb->pubsetbuf(
            buffer.data(),
            static_cast<std::streamsize>(buffer.size())
        );
    }

    stream.open(path, std::ios::binary);
    return static_cast<bool>(stream);
}

/**
 * @brief Streams chunks from input to output while tracking progress and cancellation.
 *
 * @param input Open source input stream.
 * @param output Open destination output stream.
 * @param inBuf Read buffer for source chunks.
 * @param completedBytes Optional atomic progress counter updated per chunk.
 * @param totalCopied Receives the total number of bytes written to @p output.
 * @param ec Receives the failure reason if reading or writing fails.
 * @return True if the loop exited without a read or write error, false otherwise.
 */
static bool copyStreamChunks(
    std::ifstream& input,
    std::ofstream& output,
    std::vector<char>& inBuf,
    std::atomic<std::size_t>* completedBytes,
    std::uintmax_t& totalCopied,
    std::error_code& ec
)
{
    totalCopied = 0;

    while (!isCopyCancelled()) {
        input.read(
            inBuf.data(),
            static_cast<std::streamsize>(inBuf.size())
        );

        const std::streamsize bytesRead = input.gcount();

        // badbit means a real read error, not a normal end of file. Any bytes
        // already read are not trusted: the copy must fail.
        if (input.bad()) {
            ec = std::make_error_code(std::errc::io_error);
            return false;
        }

        if (bytesRead == 0) {
            break;
        }

        output.write(inBuf.data(), bytesRead);

        if (!output) {
            ec = std::make_error_code(std::errc::io_error);
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

    return true;
}

/**
 * @brief Finalizes the temporary file by flushing, verifying size, and renaming.
 *
 * @param output Temporary output stream to close and verify.
 * @param tmp Path to the temporary ".part" file.
 * @param dst Final destination path.
 * @param totalCopied Total bytes written to @p tmp.
 * @param expectedSize Expected source size in bytes.
 * @param hasKnownSize True if @p expectedSize was determined without error.
 * @param ec Receives the failure reason if finalization fails.
 * @return True if finalized and renamed cleanly, false otherwise.
 */
static bool finalizeTemporaryFile(
    std::ofstream& output,
    const fs::path& tmp,
    const fs::path& dst,
    std::uintmax_t totalCopied,
    std::uintmax_t expectedSize,
    bool hasKnownSize,
    std::error_code& ec
)
{
    output.close();

    if (!output) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    // Never publish a short copy: a cross-device move deletes the source
    // after this function reports success.
    if (hasKnownSize && totalCopied != expectedSize) {
        ec = std::make_error_code(std::errc::io_error);
        return false;
    }

    std::error_code renameEc;
    fs::rename(tmp, dst, renameEc);

    if (renameEc) {
        ec = renameEc;
        return false;
    }

    return true;
}

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

    if (isCopyCancelled()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        return false;
    }

    std::ifstream input;
    if (!openBufferedBinaryStream(input, src, inBuf)) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return false;
    }

    fs::path tmp = dst;
    tmp += ".part";

    std::ofstream output;
    if (!openBufferedBinaryStream(output, tmp, outBuf)) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    }

    // Expected size, used to detect a source that ended early without the
    // stream reporting an error. If it cannot be determined, skip that check.
    std::error_code sizeEc;
    const std::uintmax_t expectedSize = fs::file_size(src, sizeEc);

    std::uintmax_t totalCopied = 0;
    if (!copyStreamChunks(input, output, inBuf, completedBytes, totalCopied, ec)) {
        discardTemporaryFile(output, tmp);
        return false;
    }

    if (isCopyCancelled()) {
        ec = std::make_error_code(std::errc::operation_canceled);
        discardTemporaryFile(output, tmp);
        return false;
    }

    if (!finalizeTemporaryFile(
            output,
            tmp,
            dst,
            totalCopied,
            expectedSize,
            !sizeEc,
            ec)) {
        discardTemporaryFile(output, tmp);
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
