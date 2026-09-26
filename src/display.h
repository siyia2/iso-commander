// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DISPLAY_H
#define DISPLAY_H

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Namespace containing global toggles that control the verbosity and
 * detail level of various list-based operations within the UI.
 */
inline namespace displayConfig {
    /** @brief Toggle to show the full list during mount operations. */
    inline bool toggleFullListMount = false;

    /** @brief Toggle to show the compact list during unmount operations. */
    inline bool toggleFullListUmount = true;

    /** @brief Toggle to show the compact list during file operations (Cp/Mv/Rm). */
    inline bool toggleFullListCpMvRm = true;

    /** @brief Toggle to show the full list during write operations. */
    inline bool toggleFullListWrite2usb = false;

    /** @brief Toggle to show the full list during image convert2isos. */
    inline bool toggleFullListConvert2iso = false;

    /** @brief Toggle to display only filenames, hiding full directory paths. */
    inline bool toggleNamesOnly = false;
}

#endif // DISPLAY_H
