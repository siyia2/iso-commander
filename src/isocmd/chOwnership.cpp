// SPDX-License-Identifier: GPL-3.0-or-later

// C++ Standard Library Headers
#include <string>
#include <cstdlib>

// C / System Headers
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>

/**
 * @brief Retrieves the identity of the user who invoked the process via sudo.
 * * This function is designed to be thread-safe. It attempts to extract the 
 * original user's UID and GID from the `SUDO_UID` and `SUDO_GID` environment 
 * variables. If these are unavailable or invalid, it falls back to the 
 * current effective user/group IDs. It also resolves these IDs into 
 * human-readable usernames and group names using reentrant system calls.
 * * @param[out] real_uid The resolved numeric User ID.
 * @param[out] real_gid The resolved numeric Group ID.
 * @param[out] real_username The string representation of the resolved username.
 * @param[out] real_groupname The string representation of the resolved group name.
 */
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname) {
    real_uid = static_cast<uid_t>(-1);
    real_gid = static_cast<gid_t>(-1);
    real_username.clear();
    real_groupname.clear();

    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        char* endptr;
        errno = 0;
        unsigned long uid_val = std::strtoul(sudo_uid, &endptr, 10);
        
        if (errno != 0 || *endptr != '\0' || endptr == sudo_uid) {
            real_uid = geteuid();
            real_gid = getegid();
        } else {
            real_uid = static_cast<uid_t>(uid_val);
            
            errno = 0;
            unsigned long gid_val = std::strtoul(sudo_gid, &endptr, 10);
            
            if (errno != 0 || *endptr != '\0' || endptr == sudo_gid) {
                real_gid = getegid();
            } else {
                real_gid = static_cast<gid_t>(gid_val);
            }
        }
    } else {
        real_uid = geteuid();
        real_gid = getegid();
    }

    struct passwd pwd_result;
    struct passwd *pwd = nullptr;
    char buffer[1024];

    int ret = getpwuid_r(real_uid, &pwd_result, buffer, sizeof(buffer), &pwd);
    if (ret == 0 && pwd != nullptr) {
        real_username = pwd->pw_name ? pwd->pw_name : "";
    } else {
        real_username = "unknown";
    }

    struct group grp_result;
    struct group *grp = nullptr;
    char grp_buffer[1024];

    ret = getgrgid_r(real_gid, &grp_result, grp_buffer, sizeof(grp_buffer), &grp);
    if (ret == 0 && grp != nullptr) {
        real_groupname = grp->gr_name ? grp->gr_name : "";
    } else {
        real_groupname = "unknown";
    }
}
