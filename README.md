# mounter-elite-plus
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![2024-04-22-135512_grim](https://github.com/siyia2/mounter-elite-plus/assets/46220960/d642aae7-b21d-4178-b90a-727922517c1d)

State of the art secure and blazing fast terminal `ISO` manager written in C++, it manages `ISOs` and/or converts `BIN&IMG&MDF` files to `ISO`. All paths are mounted under `/mnt/iso_*` format and the conversion results are stored in their respective source directories. `ROOT` access is required to `mount&unmount&delete ISOs`. 

Features:
* Cached ISO management for reduced disk thrashing.
* Utilizes GNU/Linux utilities: rm,rmdir,mount,umount,find.
* Tab completion and history support.
* Sanitized shell commands for improved security.
* Multithreaded asynchronous operations based on unique valid indices and max available system cores.
* Highly scalable handling anywhere from 10 ISO files to a staggering 100,000 ISO files .
* Includes a high precision timer for performance measurements.
* Support for BIN/IMG conversion to ISO by utilizing ccd2iso.
* Support for MDF conversion to ISO by utilizing mdf2iso.
* Clean codebase in case someone decides to contribute in the future.

Ways to Install:
1) Download the binary executable from latest release.
2) Download and compile from source with `make`.
3) If on arch or on an archbased distro, install with `yay -S mounter-elite-plus`.
