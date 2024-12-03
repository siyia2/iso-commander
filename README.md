# iso-commander
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![isocmd-preview](https://github.com/user-attachments/assets/e3a56f5e-cbb4-42b0-840d-eb5a7e8d6435)


State of the art secure and blazing fast terminal `ISO` manager written in C++. All paths are mounted under `/mnt/iso_*` format and conversions are stored in their respective source directories. `ROOT` access is essential for `mount&umount` operations.

For best experience execute with: `sudo isocmd`.

Features:
* Cached ISO management for reduced disk thrashing.
* Sanitized shell commands, tab completion and history support.
* Native C++ and libmount calls.
* Ultra lightweight with no reliance on external libraries for terminal control.
* Multithreaded asynchronous tasks leveraging state-of-the-art concurrent practices.
* Capable of seamlessly handling anywhere from 1 to an astonishing 100,000 ISO files.
* Supports most ISO filesystem types: iso9660, UDF, HFSPlus, Rock Ridge, Joliet, and ISOFs.
* Reimplemented ccd2iso and mdf2iso internally for converting BIN/IMG/MDF to ISO.
* Clean codebase in case someone decides to contribute in the future.

Make dependencies:
- Archlinux: readline, util-linux.
- Debian: libreadline-dev, libmount-dev.

Ways to Install:
1) Download the binary executable from latest release.
2) Download and compile from source with `make`.
3) If on arch or on an arch based distro, install with `yay -S iso-commander`.
