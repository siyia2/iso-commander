// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef RENDERING_H
#define RENDERING_H

/**
 * @file rendering.h
 * @brief Umbrella header for the terminal rendering helpers.
 *
 * @details Kept for convenience. Prefer including only the headers you use,
 * so editing one does not rebuild unrelated files.
 *
 * | Header                              | Provides                                                  |
 * |-------------------------------------|-----------------------------------------------------------|
 * | isocmd/ui/syncOutput.h              | SyncOutput, kSync*                                        |
 * | isocmd/ui/termSize.h                | terminalSize(), visibleWidthFrom(), utf8Cols(), ...       |
 * | isocmd/ui/progressBarSizeWatcher.h  | ProgressBarSizeWatcher                                    |
 * | isocmd/ui/readlineResize.h          | ListResizeWatcher, readlineWithResize(), RlLine           |
 */

// Project Headers
#include "isocmd/ui/progressBarSizeWatcher.h"
#include "isocmd/ui/readlineResize.h"
#include "isocmd/ui/syncOutput.h"
#include "isocmd/ui/termSize.h"

#endif // RENDERING_H
