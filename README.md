# iso-commander
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![prog](https://github.com/siyia2/iso-commander/assets/46220960/9f81b15e-5922-4d7f-b54f-bd887f3b1be2)

State of the art secure and blazing fast terminal `ISO` manager written in C++. All paths are mounted under `/mnt/iso_*` format and conversions are stored in their respective source directories. `ROOT` access is essential for `mount&umount` operations.

For best experience execute with: `sudo isocmd`.

Features:
* Cached ISO management for reduced disk thrashing.
* Utilizes GNU/Linux utilities: rm,rmdir,cp,mv,libmount,umount.
* Tab completion and history support.
* Sanitized shell commands for improved security.
* Ultra lightweight with no reliance on ncurses or any other external libraries for terminal control.
* Multithreaded asynchronous operations based on unique valid indices and max available system cores.
* Capable of seamlessly handling anywhere from 1 to an astonishing 100,000 ISO files.
* Support for BIN/IMG/MDF conversion to ISO by utilizing ccd2iso and mdf2iso.
* Clean codebase in case someone decides to contribute in the future.

Ways to Install:
1) Download the binary executable from latest release.
2) Download and compile from source with `make`.
3) If on arch or on an arch based distro, install with `yay -S iso-commander`.

`Make` dependencies for Debian based distros: `libmount-dev` `libreadline-dev`.
