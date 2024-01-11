# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![2024-01-09-185254_grim](https://github.com/siyia2/mounter-elite-plus/assets/46220960/43d6bf11-12a4-46a1-bb1b-7c215e40b28a)

State of the art secure and blazing fast terminal image manager written in C++, it manages `ISOs` and/or converts `BIN&IMG&MDF` files to `ISO`. All paths are mounted under `/mnt/iso_*` format and conversion results are stored in their respective source directories. `ROOT` access is required to `mount&unmount&delete ISOs`. 

Features:
* Cached ISO management to reduce disk thrashing.
* Utilizes GNU/Linux utilities: rm,rmdir,mount,unmount,find.
* Tab completion and history support.
* Sanitized shell commands for improved security.
* Multithreaded asynchronous operations based on unique valid inputs and max available system threads.
* Includes a high precision timer for performance measurements.
* Support for BIN/IMG conversion to ISO by utilizing ccd2iso
* Support for MDF conversion to ISO by utilizing mdf2iso.
* Clean codebase in case someone decides to contribute in the future.

Ways to Install:
1) Download the binary executable from latest release.
2) Download and compile from source with `make`.
3) If on arch or on an archbased distro install with `yay -S mounter-elite-plus`.
