# iso-commander
The most capable ISO manager on Linux. Mount, unmount, delete, copy, move, convert, and write ISO images with zero compromise.

![iso-commander-preview](https://github.com/user-attachments/assets/47298b0c-0dc0-4c26-a6cb-e6e6b68c2d92)

## Requirements
Root access is required for `mount`, `umount`, and `write` operations.
For the best experience, run with `sudo isocmd`.

## Quick Install
| Method | Command |
|--------|---------|
| Arch Linux (AUR) | `yay -S iso-commander` |
| Binary | Download from the [latest release](https://github.com) |
| Source | `make` |

## Getting Started
ISO files are mounted under `/mnt/iso_{name}`.
Converted files are saved in the source ISO's directory.

| Path | Description |
|------|-------------|
| `/root/.config/isocmd/config` | Configuration |
| `/root/.local/share/isocmd/database/` | Database |

> Non-root equivalents use `~/.config/isocmd/` and `~/.local/share/isocmd/` respectively.

## Features

🖊️ **Best-in-Class USB Write Engine**
- Native GPT + FAT32/NTFS dual partition layout for Windows 10/11
- Automatic Windows vs Linux/BSD ISO detection via Aho-Corasick signature scan
- No wimlib, no file splitting — `install.wim` routed to NTFS natively
- Two-pass write ordering — bootloader written last for integrity
- Byte-perfect writes verified by md5sum against source ISO
- Dynamic NTFS driver selection — `ntfs3` preferred for best performance

💡 **USB Boot Mode Support**

| ISO Type      | Write Method            | BIOS | UEFI |
|---------------|-------------------------|------|------|
| Windows < 10  | Unsupported             | ✗    | ✗    |
| Windows 10/11 | GPT + FAT32/NTFS layout | ✗    | ✓    |
| Linux / BSD   | Raw sector copy         | ✓ *  | ✓ *  |

> \* Linux/BSD boot support mirrors the source ISO — if the ISO supports BIOS, UEFI, or both, the written USB will too.

⚡ **High Performance**
- Native C++ — no Python runtime, no shell wrappers
- `O_DIRECT` unbuffered writes — bypasses page cache entirely
- Multithreaded processing — scales from 1 to 10,000 ISO files
- Lock-free operations

💾 **Smart ISO Management**
- Persistent database with atomic writes
- Minimized disk thrashing and optimized file access
- Optional automatic imports

🖥️ **Terminal Interface**
- Tab completion and pagination
- Command history
- Real-time progress tracking
- Non-interactive mode support for mount/umount
- In-built theme engine with full RGB color support

📂 **Supported Convert To ISO Formats**

| Category | Formats |
|----------|---------|
| Conversions | `.bin` `.img` `.chd` `.daa` `.gbi` `.mdf` `.nrg` |

⚙️ **Supported ISO Operations**
Mount · Unmount · Copy · Move · Delete · Write to USB · Convert to ISO

## Dependencies

| Distro | Packages |
|--------|----------|
| Arch Linux | `readline util-linux xz zstd` |
| Debian / Ubuntu | `libreadline-dev libmount-dev liblzma-dev libzstd-dev` |
| All distros | `libchdr` (statically built and linked) |
| Windows USB writing | `ntfs-3g parted dosfstools` |

## Credits
Special thanks to Aaron Giles for libchd and Romain TISSERAND for the libchdr fork *(BSD-3-Clause)*.

Bundled conversion tools:
| Tool | Author | License |
|------|--------|---------|
| mdf2iso | Salvatore Santagati | GPL-2.0-or-later |
| nrg2iso | Grégory Kokanosky | GPL-3.0-or-later (as of 2021) |
| ccd2iso | Danny Kurniawan & Kerry Harris | GPL-2.0-or-later |
| daa2iso | Luigi Auriemma | GPL-2.0-or-later |
