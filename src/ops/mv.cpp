
// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

// Project Headers
#include "isocmd/ops/cpMvRm.h"
#include "isocmd/core/cancellation.h"
#include "isocmd/ui/themes.h"

/**
 * @brief Executes a file move operation to a single destination.
 *
 * Tries a fast filesystem rename first. If that fails, typically because the
 * destination is on another device, it falls back to a buffered copy followed
 * by source deletion. The source is removed only after a successful copy.
 *
 * Ownership of the destination file is changed to the real user after a
 * successful operation.
 *
 * @param ctx Operation context for reporting, counters, and ownership.
 * @param paths Source/destination paths and display strings.
 * @param fileSize Source file size in bytes.
 * @return True if the move succeeded, false otherwise.
 */
bool performMoveOperation(
    OperationContext& ctx,
    const TransferPaths& paths,
    std::size_t fileSize
)
{
    const bool cancelled =
        GlobalState::g_operationCancelled.load(std::memory_order_acquire);

    if (cancelled) {
        logOperationResult(
            ctx, false, true, {}, "moving",
            paths.srcDir, paths.srcFile,
            paths.destDirProcessed, paths.destFile
        );
        return false;
    }

    std::error_code ec;
    std::filesystem::rename(paths.srcPath, paths.destPath, ec);

    if (!ec) {
        if (ctx.completedBytes) {
            ctx.completedBytes->fetch_add(
                fileSize,
                std::memory_order_relaxed
            );
        }

        if (ctx.changeOwnership) {
            ctx.changeOwnership(paths.destPath);
        }

        logOperationResult(
            ctx, true, false, {}, "moving",
            paths.srcDir, paths.srcFile,
            paths.destDirProcessed, paths.destFile
        );
        return true;
    }

    // Cross-device fallback.
    ec.clear();

    const bool success = bufferedCopyWithProgress(
        paths.srcPath,
        paths.destPath,
        ctx.completedBytes,
        ec
    );

    if (success) {
        std::error_code deleteEc;

        if (!std::filesystem::remove(paths.srcPath, deleteEc)) {
            const MainTheme* theme = getActiveTheme();
            const bool isOriginal = (globalTheme == "original");

            const std::string_view errLabel =
                isOriginal ? UI::Palette::Red : theme->secondary;

            const std::string_view errPath =
                isOriginal ? UI::Palette::Yellow : theme->warning;

            const std::string displaySrc =
                buildDisplaySrc(paths.srcDir, paths.srcFile);

            std::string msg;
            msg.reserve(160 + displaySrc.size() + deleteEc.message().size());

            msg.append(errLabel)
               .append("Move completed but failed to remove source file: ")
               .append(errPath)
               .append("'")
               .append(displaySrc)
               .append("'")
               .append(UI::Palette::BoldReset)
               .append(errLabel)
               .append(" - ")
               .append(deleteEc.message())
               .append(UI::Palette::BoldReset);

            ctx.reporter.addError(std::move(msg));

            if (ctx.completedTasks) {
                ctx.completedTasks->fetch_add(
                    1,
                    std::memory_order_acq_rel
                );
            }

            return true; // The destination copy succeeded.
        }

        if (ctx.changeOwnership) {
            ctx.changeOwnership(paths.destPath);
        }

        logOperationResult(
            ctx, true, false, {}, "moving",
            paths.srcDir, paths.srcFile,
            paths.destDirProcessed, paths.destFile
        );

        return true;
    }

    logOperationResult(
        ctx,
        false,
        GlobalState::g_operationCancelled.load(std::memory_order_acquire),
        ec,
        "moving",
        paths.srcDir,
        paths.srcFile,
        paths.destDirProcessed,
        paths.destFile
    );

    return false;
}
