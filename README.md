# iso-commander
Mount ISOs, convert images, write bootable USB drives — done right

<img src="preview/iso-commander-preview.gif" width="900" alt="iso-commander demo">

## Getting Started

### Installation

1. **Arch Linux (AUR)** — `yay -S iso-commander` or `yay -S iso-commander-bin`
2. **Static Binary** — Download from the [latest release](https://github.com/siyia2/iso-commander/releases)
3. **Source** — `make`

### Dependencies

| Distro | Packages |
|--------|----------|
| Arch Linux | `readline util-linux xz zstd` |
| Debian | `libreadline-dev libmount-dev liblzma-dev libzstd-dev` |
| Agnostic | `libchdr` (statically built and linked) |
| Windows USB Writing | Arch/Debian: `ntfsprogs`/`ntfs-3g` `parted` `dosfstools` |

<br>

**Supported Operations**
> Mount · Unmount · Copy · Move · Delete · Write to USB · Convert to ISO

**Convert to ISO — Supported Formats (archival only — breaks emulator compatibility)**
> `.bin` · `.img` · `.chd` · `.daa` · `.gbi` · `.mdf` · `.nrg`

### Notes
- Root access is required for `mount`, `umount`, and `write` operations. Run with `sudo isocmd`
- ISO files are mounted under `/mnt/iso_{name}`
- Converted files are saved to the source ISO's directory
- Config: `/root/.config/isocmd/config` — Database: `/root/.local/share/isocmd/database/`

> Non-root equivalents use `~/.config/isocmd/` and `~/.local/share/isocmd/` respectively

## Features

🖊️ **Best-in-Class USB Write Engine**
- No wimlib, no file splitting — `install.wim` routed to NTFS natively
- Two-pass write ordering — bootloader written last for integrity
- Dynamic NTFS driver selection — `NTFSPLUS` is preferred for best performance

⚡ **High Performance**
- Native C++ — no Python runtime, no shell wrappers
- `O_DIRECT` unbuffered writes — bypasses page cache entirely
- Multithreaded, lock-free processing — scales from 1 to 10,000 ISO files

💾 **Smart ISO Management**
- Persistent database with atomic writes
- Minimized disk thrashing and optimized file access
- Optional automatic imports to database

🖥️ **Terminal Interface**
- Tab completion, pagination, and command history
- Real-time progress tracking and non-interactive mode
- Built-in theme engine with full RGB color support

## USB Boot Mode Support

| ISO Type           | Write Method            | BIOS | UEFI |
|--------------------|-------------------------|------|------|
| Windows < 10       | Unsupported             | ✗    | ✗    |
| Windows 10/11      | GPT + FAT32 + NTFS      | ✗    | ✓    |
| Windows 10/11 PE   | GPT + FAT32             | ✗    | ✓    |
| Linux/BSD          | Raw Sector Copy         | ✓ *  | ✓ *  |

> \* Boot support depends on what the original ISO supports

## 🏆 Credits

Special thanks to Aaron Giles for libchdr and Romain TISSERAND for the libchdr fork *(BSD-3-Clause)*

The following conversion tools were ported — special thanks to their original authors:

- **mdf2iso** — Salvatore Santagati *(GPL-2.0-or-later)*
- **nrg2iso** — Grégory Kokanosky *(GPL-3.0-or-later, as of 2021)*
- **ccd2iso** — Danny Kurniawan & Kerry Harris *(GPL-2.0-or-later)*
- **daa2iso** — Luigi Auriemma *(GPL-2.0-or-later)*
