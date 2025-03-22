# iso-commander

![iso-commander-preview](https://github.com/user-attachments/assets/47298b0c-0dc0-4c26-a6cb-e6e6b68c2d92)


Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite

High performance `ISO` manager written in pure C++. `ISOs` are mounted under `/mnt/iso_{name}` format and conversions are stored in their respective source directories. `ROOT` access is required for `mount` `umount` and `write` operations.

For best experience execute with: `sudo isocmd`.

## ✨ Features

💾 Smart ISO Storage & Retrieval:

* Minimizes disk thrashing.
* Optimizes file access performance.
* Optional automatic imports to database.


🖥️ Advanced Terminal Interface:

* Robust tab completion and pagination.
* Comprehensive command history.
* Advanced progress tracking.


⚡ High-Performance Architecture:

* Native C++ implementation.
* Direct libmount and umount2 system calls.
* Zero external library dependencies for terminal control.


🔀 Concurrent Processing:

* Multithreaded asynchronous task handling.
* Lock-free operations.
* Scalable from 1 to 10,000 ISO files.
* Implements cutting-edge concurrent programming practices.


📂 Supports multiple ISO filesystem types:

- iso9660
- UDF
- HFSPlus
- Rock Ridge
- Joliet
- ISOFs


⚙️ Supported Operations:

* Mount/Unmount ISO
* Copy/Move ISO
* Delete ISO
* Write ISO to Removable USB Drives
* Convert (.bin/.img/.mdf/.nrg) to ISO

## Make dependencies
- Archlinux: `readline util-linux`.
- Debian: `libreadline-dev libmount-dev`.

## Ways to Install
* Download the binary from latest release.
* Download and compile from source with `make`.
* On ArchLinux `iso-commander` is available from the `AUR`.

## 🏆 Credits
Special thanks to the original authors of the conversion tools:

* Salvatore Santagati (mdf2iso).
* Grégory Kokanosky (nrg2iso).
* Danny Kurniawan and Kerry Harris (ccd2iso).

Original Licenses:
- ccd2iso -> GPL2 or later
- mdf2iso -> GPL2 or later
- nrg2iso -> GPL3 or later (as of 2021)

 Note: Their original code has been modernized and ported to C++.
