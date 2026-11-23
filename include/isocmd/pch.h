// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef ISOCMD_PCH_H
#define ISOCMD_PCH_H

/**
 * @file pch.h
 * @brief Precompiled header holding the standard headers most translation units use.
 *
 * @details The Makefile compiles this file once per build variant and
 * force-includes the result into every source file (see the @c PCH variable
 * in the Makefile; disable with `make PCH=0`). GCC only; it is skipped
 * automatically for clang.
 *
 * @par Rules for editing
 * - Add only standard or third-party headers that most translation units use.
 * - Never add project headers (`isocmd/...`): editing one of them would
 *   invalidate the PCH and rebuild every source file.
 *
 * @note Source files keep their own explicit @c \#include directives, so each
 *       one remains valid and compiles without the PCH.
 */

// C++ Standard Library Headers
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#endif // ISOCMD_PCH_H
