# iso-commander
The most capable ISO manager on Linux. Mount, unmount, delete, copy, move, convert, and write ISO images with zero compromise.

![iso-commander-preview][([https://github.com/user-attachments/assets/47298b0c-0dc0-4c26-a6cb-e6e6b68c2d92](https://github.com/siyia2/iso-commander/blob/main/preview/iso-commander-preview.gif))](https://github.com/siyia2/iso-commander/blob/main/preview/iso-commander-preview.gif)

## Getting Started

### Installation

1. **Arch Linux (AUR)** — `yay -S iso-commander`
2. **Binary** — Download from the [latest release](https://github.com)
3. **Source** — `make`

### Dependencies

| Distro | Packages |
|--------|----------|
| Arch Linux | `readline util-linux xz zstd` |
| Debian / Ubuntu | `libreadline-dev libmount-dev liblzma-dev libzstd-dev` |
| All distros | `libchdr` (statically built and linked) |
| Windows USB writing | `ntfs-3g parted dosfstools` |


**Supported Operations**
> Mount · Unmount · Copy · Move · Delete · Write to USB · Convert to ISO

**Convert to ISO — Supported Formats**
> `.bin` · `.img` · `.chd` · `.daa` · `.gbi` · `.mdf` · `.nrg`

> Root access is required for `mount`, `umount`, and `write` operations. Run with `sudo isocmd`.

### Notes
- ISO files are mounted under `/mnt/iso_{name}`
- Converted files are saved to the source ISO's directory
- Config: `~/.config/isocmd/config` — Database: `~/.local/share/isocmd/database/`

> Non-root equivalents use `~/.config/isocmd/` and `~/.local/share/isocmd/` respectively.

## Features

🖊️ **Best-in-Class USB Write Engine**
- No wimlib, no file splitting — `install.wim` routed to NTFS natively
- Two-pass write ordering — bootloader written last for integrity
- Byte-perfect writes verified by md5sum against source ISO
- Dynamic NTFS driver selection — `ntfs3` preferred for best performance

⚡ **High Performance**
- Native C++ — no Python runtime, no shell wrappers
- `O_DIRECT` unbuffered writes — bypasses page cache entirely
- Multithreaded, lock-free processing — scales from 1 to 10,000 ISO files

💾 **Smart ISO Management**
- Persistent database with atomic writes and automatic imports
- Minimized disk thrashing and optimized file access
- Optional automatic imports to database

🖥️ **Terminal Interface**
- Tab completion, pagination, and command history
- Real-time progress tracking and non-interactive mode
- Built-in theme engine with full RGB color support

## USB Boot Mode Support

| ISO Type      | Write Method            | BIOS | UEFI |
|---------------|-------------------------|------|------|
| Windows < 10  | Unsupported             | ✗    | ✗    |
| Windows 10/11 | GPT + FAT32/NTFS layout | ✗    | ✓    |
| Linux / BSD   | Raw sector copy         | ✓ *  | ✓ *  |

> \* Linux/BSD boot support mirrors the source ISO — if the ISO supports BIOS, UEFI, or both, the written USB will too.

## 🏆 Credits

Special thanks to Aaron Giles for libchdr and Romain TISSERAND for the libchdr fork *(BSD-3-Clause)*.

The following conversion tools were ported — special thanks to their original authors:

- **mdf2iso** — Salvatore Santagati *(GPL-2.0-or-later)*
- **nrg2iso** — Grégory Kokanosky *(GPL-3.0-or-later, as of 2021)*
- **ccd2iso** — Danny Kurniawan & Kerry Harris *(GPL-2.0-or-later)*
- **daa2iso** — Luigi Auriemma *(GPL-2.0-or-later)*
