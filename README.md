# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![2024-01-09-185254_grim](https://github.com/siyia2/mounter-elite-plus/assets/46220960/43d6bf11-12a4-46a1-bb1b-7c215e40b28a)

State of the art secure and blazing fast terminal image mounter/converter written in C++, it allows mounting `ISOs` and/or converting `BIN&IMG&MDF` files to `ISO`. Supports `Tab completion` and `on-disk cache` generation. 
You can add multiple mounts and/or conversion paths. All paths are mounted under `/mnt/iso_*` format and all conversions take place at their local source directories, script requires `ROOT` access to `mount&unmount ISOs`. 

If you get: `Error: filesystem error: cannot increment recursive directory iterator: Permission denied`.
Make sure your path has `755` and upwards permissions for better cache generation, or alternatively, you can set the executable as an `SUID`, or you can always run it as `ROOT` from the beginning if need be.

Features:
* Cached ISO management to reduce disk thrashing.
* Sanitized shell commands rm,rmdir,moun,unmount,find for improved security.
* Supports all filenames, including special charactes and gaps and ''.
* Multithreaded and asynchronous operations for maximum performance.
* Includes a high precision timer.
* Support for BIN/IMG conversion to ISO by utilizing ccd2iso
* Support for MDF conversion to ISO by utilizing mdf2iso.
* Ability to delete cached ISO files.
* Clean codebase, just in case someone decides to contribute in the future.
  
Compilation requires the readline library installed from your distro. 
Once dependancies are met, open a terminal inside the source folder and run `make`, this will make a file named `mounter_elite_plus`.
Mark it as executable and it can then be run from anywhere.

You can also download the binaries from my releases, or if you are on arch or on an archbased distro you can install with `yay -S mounter-elite-plus`.
