# iso-commander
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

![2024-04-27-212552_grim](https://github.com/siyia2/iso-commander/assets/46220960/c823f745-0231-491d-a86d-6b5610d1f5a1)


State of the art secure and blazing fast terminal `ISO` manager written in C++. All paths are mounted under `/mnt/iso_*` format and conversion results are stored in their respective source directories. `ROOT` access is required to `mount&unmount ISOs`.

For best experience i recommend setting an SUID:  `sudo chown root:root /usr/bin/isocmd;sudo chmod u+s /usr/bin/isocmd`

Features:
* Cached ISO management for reduced disk thrashing.
* Utilizes GNU/Linux utilities: rm,rmdir,cp,mv,mount,umount,find.
* Tab completion and history support.
* Sanitized shell commands for improved security.
* Multithreaded asynchronous operations based on unique valid indices and max available system cores.
* Capable of effortlessly handling anywhere from 1 to an astonishing 100,000 ISO files.
* Includes a high precision timer for performance measurements.
* Support for BIN/IMG/MDF conversion to ISO by utilizing ccd2iso and mdf2iso.
* Clean codebase in case someone decides to contribute in the future.
  
Ways to Install:
1) Download the binary executable from latest release.
2) Download and compile from source with `make`.
3) If on arch or on an archbased distro, install with `yay -S iso-commander`.
