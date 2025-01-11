# iso-commander
Port of bash shell program mounter_elite to C++ 
https://github.com/siyia2/mounter_elite




https://github.com/user-attachments/assets/2d247581-bf84-437b-b872-800111986d42




Secure and blazing fast cmd `ISO` manager written in pure C++. All paths are mounted under `/mnt/iso_*` format and conversions are stored in their respective source directories. `ROOT` access is essential for `mount&umount` operations.

For best experience execute with: `sudo isocmd`.

## ✨ Features

💾 Intelligent ISO Caching:

* Minimizes disk thrashing.
* Optimizes file access performance.


🖥️ Advanced Terminal Interface:

* Robust tab completion.
* Comprehensive command history.
* Advanced progress tracking.


⚡ High-Performance Architecture:

* Native C++ implementation.
* Direct libmount and umount2 system calls.
* Zero external library dependencies for terminal control.


🔀 Concurrent Processing:

* Multithreaded asynchronous task handling.
* Lock-free operations.
* Scalable from 1 to 100,000 ISO files.
* Implements cutting-edge concurrent programming practices.


📂 Supports multiple ISO filesystem types:

- iso9660
- UDF
- HFSPlus
- Rock Ridge
- Joliet
- ISOFs


🔄 Powerful Conversion Toolkit Integrating:

* ccd2iso
* mdf2iso
* nrg2iso

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

 Note: Their original code has been modernized and ported to C++.
