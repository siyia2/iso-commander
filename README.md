# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite


![2023-12-15-012631_grim](https://github.com/siyia2/mounter-elite-plus/assets/46220960/6c04877a-24a4-4521-8263-9fa40994881a)




State of the art secure and blazing fast terminal image mounter/converter written in C++, it allows mounting `ISOs` and/or converting `BIN&IMG&MDF` files to `ISO`. Supports `Tab completion` and `on-disk cache` generation. 
You can add multiple mounts and/or conversion paths. All paths are mounted under `/mnt/iso_*` format and all conversions take place at their local source directories, script requires `ROOT` access to `mount&unmount ISOs`. 

If you get: `Error: filesystem error: cannot increment recursive directory iterator: Permission denied`.
Make sure your path has `755` and upwards permissions for better cache generation, or alternatively, you can set the executable as an `SUID`, or you can always run it as `ROOT` from the beginning if need be.

Added Features:
* Cached ISO management to reduce disk thrashing.
* Sanitized shell commands for improved security.
* Supports all filenames, including special charactes and gaps and ''.
* Extra checks and error handling so that you don't erase your / or /mnt accidentally.
* More robust menu experience.
* Improved selection procedures.
* Multithreaded and asynchronous operations for maximum performance.
* Includes a high precision timer to measure performance in mount/unmount and list generation operations.
* Dropped manual mode for mounting/converting, since caching is way faster and easier to manage.
* Added support for MDF conversion to ISO by utilizing mdf2iso.
* Clean codebase, just in case someone decides to contribute in the future.

Tested the program on my rig: `mounting` at the same time `125 ISOS` and then `unmounting` them, did this a couple of times and it didn't even break a sweat, Rock Solid!!! :)

Compilation requires the readline library installed from your distro. 
Once dependancies are met, open a terminal inside the source folder and run `make`, this will make a file named `mounter_elite_plus`.
Mark it as executable and it can then be run from anywhere.

You can also download the binaries from my releases, or if you are on arch or on an archbased distro you can install with `yay -S mounter-elite-plus`.
