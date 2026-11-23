// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef STATE_H
#define STATE_H

/**
 * @file state.h
 * @brief Umbrella header for the global application state.
 *
 * @details Prefer including only the part you use, so editing one concern
 * does not rebuild unrelated files.
 *
 * | Header                        | Provides                                                      |
 * |-------------------------------|---------------------------------------------------------------|
 * | isocmd/core/cancellation.h    | GlobalState::g_operationCancelled                             |
 * | isocmd/core/paths.h           | database/config paths, history limits                         |
 * | isocmd/core/uiState.h         | pending-refresh flags, page size, "where am I" flags          |
 * | isocmd/core/lists.h        | global ISO list and per-format image caches                   |
 */

// Project Headers
#include "isocmd/core/cancellation.h"
#include "isocmd/core/lists.h"
#include "isocmd/core/paths.h"
#include "isocmd/core/uiState.h"

#endif // STATE_H
