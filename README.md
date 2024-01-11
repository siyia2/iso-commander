# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![2024-01-09-185254_grim](https://github.com/siyia2/mounter-elite-plus/assets/46220960/43d6bf11-12a4-46a1-bb1b-7c215e40b28a)

State of the art secure and blazing fast terminal image manager written in C++, it allows mounting `ISOs` and/or converting `BIN&IMG&MDF` files to `ISO`. All paths are mounted under `/mnt/iso_*` format and all conversions take place at their local source directories, it requires `ROOT` access to `mount&unmount ISOs`. 

Features:
* Cached ISO management to reduce disk thrashing.
* Ability to delete cached ISO files.
* Utilizes GNU utilities: rm,rmdir,moun,unmount,find.
* Tab completion and history support.
* Sanitized shell commands for improved security.
* Supports all filenames, including special charactes and gaps and ''.
* Multithreaded and asynchronous operations based on unique valid inputs and max vailable system threads.
* Includes a high precision timer to measure performance.
* Support for BIN/IMG conversion to ISO by utilizing ccd2iso
* Support for MDF conversion to ISO by utilizing mdf2iso.
* Clean codebase in case someone decides to contribute in the future.

If you get: `Error: filesystem error: cannot increment recursive directory iterator: Permission denied`.
Make sure your path has `755` and upwards permissions for better cache generation, or alternatively, you can set the executable as an `SUID`, or you can always run it as `ROOT` wit `sudo` from the beginning if need be.

You download the binaries from my releases, compile witk `make`, or if you are on arch or on an archbased distro you can install with `yay -S mounter-elite-plus`.
