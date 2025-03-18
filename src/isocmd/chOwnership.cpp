// SPDX-License-Identifier: GNU General Public License v2.0

#include "../headers.h"


// Function to get the sudo invoker ID in a thread safe manner
void getRealUserId(uid_t& real_uid, gid_t& real_gid, std::string& real_username, std::string& real_groupname) {
    // Reset output parameters to prevent any uninitialized memory issues
    real_uid = static_cast<uid_t>(-1);
    real_gid = static_cast<gid_t>(-1);
    real_username.clear();
    real_groupname.clear();

    // Get the real user ID and group ID (of the user who invoked sudo)
    const char* sudo_uid = std::getenv("SUDO_UID");
    const char* sudo_gid = std::getenv("SUDO_GID");

    if (sudo_uid && sudo_gid) {
        char* endptr;
        errno = 0;
        unsigned long uid_val = std::strtoul(sudo_uid, &endptr, 10);
        
        // Check for conversion errors
        if (errno != 0 || *endptr != '\0' || endptr == sudo_uid) {
            // Fallback to current effective user
            real_uid = geteuid();
            real_gid = getegid();
        } else {
            real_uid = static_cast<uid_t>(uid_val);
            
            errno = 0;
            unsigned long gid_val = std::strtoul(sudo_gid, &endptr, 10);
            
            // Check for conversion errors
            if (errno != 0 || *endptr != '\0' || endptr == sudo_gid) {
                // Fallback to current effective group
                real_gid = getegid();
            } else {
                real_gid = static_cast<gid_t>(gid_val);
            }
        }
    } else {
        // Fallback to current effective user if not running with sudo
        real_uid = geteuid();
        real_gid = getegid();
    }

    // Get real user's name (with more robust error handling)
    struct passwd pwd_result;
    struct passwd *pwd = nullptr;
    char buffer[1024];  // Recommended buffer size for getpwuid_r

    int ret = getpwuid_r(real_uid, &pwd_result, buffer, sizeof(buffer), &pwd);
    if (ret == 0 && pwd != nullptr) {
        real_username = pwd->pw_name ? pwd->pw_name : "";
    } else {
        // Fallback if unable to retrieve username
        real_username = "unknown";
    }

    // Get real group name (with more robust error handling)
    struct group grp_result;
    struct group *grp = nullptr;
    char grp_buffer[1024];  // Recommended buffer size for getgrgid_r

    ret = getgrgid_r(real_gid, &grp_result, grp_buffer, sizeof(grp_buffer), &grp);
    if (ret == 0 && grp != nullptr) {
        real_groupname = grp->gr_name ? grp->gr_name : "";
    } else {
        // Fallback if unable to retrieve group name
        real_groupname = "unknown";
    }
}
