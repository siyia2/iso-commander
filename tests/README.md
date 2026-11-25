# ISO Commander regression tests

The tests use production C++ modules and object files. The unit and integration
executables exclude the application's `main.cpp`; the menu test builds a separate
copy of that entry point with a private sandbox lock instead of `/tmp/isocmd.lock`. No Python or external C++ test framework is required. Normal builds do not
compile test sources. The usual GCC 15+ and project dependencies apply.

## Running

```sh
make test-unit             # unit tests and CLI behavioral tests
make test-integration      # unprivileged filesystem, cancellation, writer, import and PTY tests
make test-unit SANITIZE=1       # AddressSanitizer
make test-unit SANITIZE=2       # ThreadSanitizer
make test-integration SANITIZE=1
make test-integration SANITIZE=2
make test-unit FAST=1
make test-unit STATIC=1         # requires the project's static libraries
```

Run these as a regular user. The PTY integration tests use native C++ and
Linux terminal/process APIs (`forkpty`, `poll`, `ioctl`, `waitpid`). The
integration executable links the system `libutil` for PTY support. Runners
use GNU coreutils `timeout`, `realpath` and `mktemp`. Do not run separate make
invocations concurrently in the same build variant.

### Privileged tests (optional)

```sh
make test-privileged
```

Run this command **without sudo**. The target builds the executable and all
compiler caches as your ordinary user, then invokes `sudo` only for the isolated
privileged runner. Test targets reject root invocations before generating any
build or cache files. The runner's temporary root-owned files are cleaned up
on exit; it does not write to the project's build tree or standard-library cache.

To build without executing, or to run the compiled release test manually:

```sh
make test-privileged-build
sudo sh tests/privileged/run.sh build/release/tests/isocmd-privileged-tests
```

If an older Makefile was run under sudo, restore ownership of existing generated
files before compiling or cleaning again. From the project directory, for each
of the following directories that exists:

```sh
sudo chown -hR --from=0 "$(id -u):$(id -g)" -- .std-cache
sudo chown -hR --from=0 "$(id -u):$(id -g)" -- build
```

For a custom `BUILD_DIR`, use that generated directory instead of `build`.
The ownership repair affects only root-owned entries in the specified trees.

The privileged suite requires Linux, util-linux (`losetup`, `unshare`, `mount`),
coreutils, `sudo` for automatic elevation, available loop devices, and root/CAP_SYS_ADMIN. The runner creates a
fresh 16 MiB sparse backing file in a root-owned private temporary directory
and attaches a new loop device. No user-supplied device path is accepted by the
runner. The executable independently verifies that the block device is a whole
loop device and that its kernel-reported backing inode/device and size match
that fresh file before calling the real Linux ISO writer. It checks the written
bytes, alignment padding, completion flag and untouched trailing byte.

If `xorriso` or `genisoimage` is installed, the runner also generates an ISO and
calls the production mount/unmount functions in a separate private mount
namespace, with a private empty directory bind-mounted over `/mnt`. It verifies
the mounted file contents and removal of the mount point. If neither ISO creation
tool is available, this part is explicitly skipped. No physical USB writes or
host mount/unmount targets are used. Loop detach and temporary-file cleanup are
attempted on exit; if detach fails, the backing directory is retained and its
location printed. A forced kill, power loss or kernel failure can prevent cleanup.
Run only in a trusted environment, not alongside other privileged loop-device
management processes.

## Layout

```text
tests/
├── common/
│   ├── testHarness.h       # release-active checks, fixtures, group runner
│   └── sandbox.sh          # private HOME, XDG, TMPDIR and cleanup
├── unit/
│   ├── filtering.cpp
│   ├── selection.cpp
│   ├── sorting.cpp
│   ├── configuration.cpp
│   ├── rendering.cpp
│   ├── converters.cpp
│   ├── database.cpp
│   ├── threadPool.cpp
│   ├── deviceDiscovery.cpp
│   ├── history.cpp
│   ├── writeTelemetry.cpp
│   └── usbUtilities.cpp
├── integration/
│   ├── main.cpp
│   ├── filesystem.cpp
│   ├── cancellation.cpp
│   ├── backgroundImport.cpp
│   ├── terminal.cpp
│   ├── terminalDriver.cpp  # native PTY controller
│   ├── usbWriter.cpp
│   ├── writeUi.cpp         # safe mapping, progress and injected writer child
│   └── run.sh
├── privileged/
│   ├── mount.cpp
│   ├── usbWriter.cpp
│   └── run.sh
├── main.cpp                # unit test registry
├── run.sh                  # unit runner and CLI smoke tests
└── README.md
```

## Coverage

| Suite | Coverage |
| --- | --- |
| Unit: filtering | Smart case, AND/OR, exclusions, escaped delimiters, subset order, caches and fixed-seed scalar oracle for SIMD filtering |
| Unit: selection | Indices, reversed ranges, duplicates, malformed input and overflow |
| Unit: sorting | Natural ordering, path/name modes, parallel sort and list views |
| Unit: configuration | Validators, defaults, self-repair, on-disk order and pagination |
| Unit: rendering | Themes, fallback colors, ANSI/Readline markers, UTF-8 clipping, ISO extension preservation, actual list pagination and filtered pending-index rendering |
| Unit: pagination commands | PageUp/PageDown at boundaries, direct jumps, invalid/overflow jumps, shrinking and disabled pagination |
| Unit: write telemetry | Raw and Windows EMA, 1 s sample floor, exact 60/300 s stall thresholds, recovery, counter regression, 99% cap, speed formatting |
| Unit: USB mappings | Multiple targets per ISO, duplicate canonical paths, malformed and missing mappings, permissions, non-USB rejection and capacity checks |
| Unit: converters | CCD Mode 1/2 and NRG payload bytes, counters, invalid input and pre-cancellation |
| Unit: database | Persistence, duplicate rejection, overlapping concurrent writers, stale-entry removal |
| Unit: thread pool | Exactly-once execution, 1–16 workers, concurrent producers, move-only values, inline/heap callable lifetime and ownership, throwing-move fallback, futures, exceptions, nested tasks and shutdown |
| Unit: USB utilities | ISO 9660/UDF and Windows signature detection, progress moves and aligned buffers |
| Unit: device discovery | Synthetic sysfs/dev/proc fixtures, USB ancestry, removable flag, ignored device names, symlink aliases, model and partition mount detection |
| Unit: history | Independent path/filter histories, newest-entry deduplication, limits, disabled history, missing/empty files |
| CLI | Version/parser errors, successful IMG conversion with exact output bytes, depth, silent mode, existing output preservation, mixed valid/invalid batch and unprivileged mount/unmount/write rejection |
| Integration: filesystem | Actual copy/move/delete, overwrite at the low-level copy helper, counters, callbacks, missing source, failed rename cleanup, scan depth and directory symlinks |
| Integration: cancellation | Real SIGINT handler, pre-cancelled copy/move/delete/scan, active FIFO-fed copy cancellation, partial-file cleanup and recovery |
| Integration: USB writer | Invalid ISO, rejection of regular-file target, Windows copy engine (file bytes, directories, empty files, early EOF, missing file, cancellation) |
| Integration: background import | History parsing, nested discovery, duplicate suppression, completion and worker accounting, stopped import, restart and missing history |
| Integration: PTY | Four terminal resizes during typed input, repaint, preservation of input and Readline/signal state restoration |
| Integration: menus | Real application main menu, invalid choice, resize with typed input, ManageISO, Convert2ISO and image search, ImportISO, Settings, Esc/EOF return and normal exit |
| Integration: interactive workflows | Real PageUp/PageDown and page jumps, live filter scroll, query replacement, resize, no-match retry, cancellation, staged copy, rejected/confirmed delete and move with filesystem checks |
| Integration: USB UI | Injected discovery/validation using harmless device names; invalid mapping retry, confirmation resize, decline and prefill, multi-device task ordering, completed/partial/failed summaries |
| Integration: progress | Real WriteScreen rendering at 10/50/99/100%, speed and bar glyphs, filename clipping, active/result resize, DONE/FAIL/CXL and interrupted summary; copy result resize at verbose prompt |
| Privileged | Linux writer on verified disposable loop device; optional private-namespace ISO mount/read/unmount |

## Safety and limitations

Each ordinary runner starts a new process with isolated HOME, XDG directories,
TMPDIR, and current directory, and uses timeouts to make hangs fail. Unit and
integration fixtures are temporary regular files, directories and a FIFO. USB discovery
uses explicit synthetic roots; normal production entry points still use the system roots.
The CLI tests exercise conversion in the sandbox, and only dispatch mount, unmount
and USB commands when running unprivileged to check permission rejection. No real
mount or USB write is attempted. The menu test uses a separately compiled app with
a private lock. The workflow test seeds an isolated database with temporary regular files and checks real copy, move and delete operations only on those fixtures. Run unprivileged.
The USB UI tests inject only discovery/validation and a function-pointer writer;
production entry points still use the real discovery, validation and writer.
The fake writer never opens or writes any device. `/dev/null`, `/dev/zero` and
`/dev/full` are used only as existing canonical path fixtures for mapping and
read-only metadata checks. Telemetry tests inject timestamps, not wall-clock waits.
The database test additionally refuses to modify a database outside the runner's
sandbox or an already-existing database file. A failing group exits nonzero and
reports the failed assertion and source line.

The suite does not yet simulate short device writes, general I/O fault injection, active
USB-write cancellation, the full Windows partition/format workflow, actual
FAT32/NTFS writes, all converters, successful physical-device flashing through the UI, every interactive operation or all possible thread
interleavings. The Windows copy-engine tests use regular filesystem paths, not
actual FAT32/NTFS volumes. The privileged Linux writer fixture has a synthetic
ISO signature, not a bootable image. Passing sanitizers do not prove the absence
of all races or undefined behavior.

## Adding tests

Add a test function in the appropriate source file and register it in
`tests/main.cpp` or `tests/integration/main.cpp`. Use `CHECK(...)` from the
shared harness (unlike `assert`, it remains enabled in release builds).
Use `TempDir` and ordinary files for fixtures. Unit and integration `.cpp`
files are automatically discovered by the Makefile. Keep tests outside `src/`
and keep physical devices, real user data and system mount targets out of
all suites. Only the explicitly invoked privileged runner may create loop devices.
