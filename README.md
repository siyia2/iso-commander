# iso-commander
![iso-commander-preview](https://github.com/user-attachments/assets/47298b0c-0dc0-4c26-a6cb-e6e6b68c2d92)

The most capable ISO manager on Linux. Mount, Unmount, Delete, Copy, Move, convert, and Write ISO images with zero compromise.

The only Linux tool that writes Windows 10/11 bootable USB drives correctly — native GPT + FAT32/NTFS layout, no wimlib, no file splitting, no workarounds. Faster and more correct than WoeUSB.

Root access is required for `mount`, `umount`, and `write` operations. For best experience execute with `sudo isocmd`.

ISO files are mounted under `/mnt/iso_{name}` format. Conversions are stored in their respective source directories.

Configuration: `/root/.config/isocmd/config` or `~/.config/isocmd/config`  
Database: `/root/.local/share/isocmd/database/`

## ✨ Features

💾 **Smart ISO Storage & Retrieval**
- Minimizes disk thrashing
- Optimizes file access performance
- Optional automatic imports to database
- Resilient atomic database writes

🖥️ **Advanced Terminal Interface**
- Robust tab completion and pagination
- Comprehensive command history
- Advanced progress tracking
- Optional non-interactive mode
- Full RGB color support

⚡ **High-Performance Architecture**
- Native C++ implementation — no Python runtime, no shell wrappers
- Direct `libmount` and `umount2` system calls
- Single self-contained executable binary
- `O_DIRECT` unbuffered non-ntfs writes — bypasses page cache entirely
- `sync_file_range` dirty-page bounding on NTFS — instant cancellation

🔀 **Concurrent Processing**
- Multithreaded asynchronous task handling
- Lock-free operations
- Scalable from 1 to 10,000 ISO files

📂 **Supported ISO Filesystem Types**
- ISO 9660, UDF, Rock Ridge, Joliet, ISOFs

🖊️ **Best-in-Class USB Write Engine**
- Native GPT + FAT32/NTFS dual partition layout for Windows 10/11
- Automatic Windows vs Linux/BSD ISO detection via Aho-Corasick signature scan
- No wimlib, no file splitting — `install.wim` routed to NTFS natively
- Two-pass write ordering — bootloader written last for integrity
- Byte-perfect writes verified by md5sum against source ISO
- Kernel-aware NTFS driver selection — `ntfs` on 7.1+, `ntfs3` as a safe fallback
- Best Windows USB writer on Linux, period

⚙️ **Supported Operations**
- Mount/Unmount ISO
- Copy/Move ISO
- Delete ISO
- Write ISO (Linux, BSD, Windows 10/11) to USB drives
- Convert `.bin` `.img` `.chd` `.daa` `.gbi` `.mdf` `.nrg` to ISO

## Dependencies
- **Arch Linux:** `readline util-linux xz zstd`
- **Debian/Ubuntu:** `libreadline-dev libmount-dev liblzma-dev libzstd-dev`
- **Agnostic:** `libchdr` (statically built and linked)
- **Optional — required for Windows live USB creation:** `ntfs-3g` `parted` `dosfstools`

## Ways to Install
- Download the binary from the latest release
- Compile from source with `make`
- Arch Linux: available from the AUR — `yay -S iso-commander`

## 🏆 Credits

Special thanks to Aaron Giles for libchd and Romain TISSERAND for the libchdr fork.
- libchd license: BSD-3-Clause
- libchdr license: BSD-3-Clause

Special thanks to the original authors of the bundled conversion tools:
- Salvatore Santagati (mdf2iso) — GPL-2.0-or-later
- Grégory Kokanosky (nrg2iso) — GPL-2.0-or-later
- Danny Kurniawan & Kerry Harris (ccd2iso) — GPL-2.0-or-later
- Luigi Auriemma (daa2iso) — GPL-2.0-or-later
- nrg2iso — GPL-3.0-or-later (as of 2021)
