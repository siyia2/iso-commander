// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef DISPLAY_H
#define DISPLAY_H

/**
 * @brief Canonical list of all supported configuration settings with validation.
 * @details Namespace containing global toggles that control the verbosity and 
 * detail level of various list-based operations within the UI.
 */
namespace displayConfig {
    /** @brief Toggle to show the full list during mount operations. */
    extern bool toggleFullListMount;
    
    /** @brief Toggle to show the full list during unmount operations. */
    extern bool toggleFullListUmount;
    
    /** @brief Toggle to show the full list during file operations (Cp/Mv/Rm). */
    extern bool toggleFullListCpMvRm;
    
    /** @brief Toggle to show the full list during write operations. */
    extern bool toggleFullListWrite2usb;
    
    /** @brief Toggle to show the full list during image convert2isos. */
    extern bool toggleFullListConvert2iso;
    
    /** @brief Toggle to display only filenames, hiding full directory paths. */
    extern bool toggleNamesOnly;
}

#endif // DISPLAY_H
