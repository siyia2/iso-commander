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

### Asynchronous USB cancellation regression coverage

The unit and integration suites check that cancellation remains latched after
resetting the global UI flag, including direct and buffered Windows copy paths,
CRC-64 readback, and the lifetime of moved progress records. The PTY test holds
an injected writer in flight while the UI returns, checks that its device stays
reserved, edits the buffered-write setting, then releases the writer and checks
that cancellation and its original settings snapshot are preserved. The
privileged loop-device suite also checks the raw writer after a short write with
latched cancellation and a reset UI flag. No additional dependencies are needed.

Unprivileged unit tests use synthetic sysfs and mount-table fixtures to exercise
the production busy check, device validation in both modes, and status labels.
They cover pending writes, read-only I/O, missing or malformed counters, mounted
status priority, drain reservations and acceptance after the device becomes idle.
No actual block device is opened or modified.

### Privileged tests (optional)

```sh
make test-privileged
```

Run this command **without sudo**. The target builds the application, privileged test executable and all
compiler caches as your ordinary user, then invokes `sudo` only for the isolated
privileged runner. Test targets reject root invocations before generating any
build or cache files. The application and helper scripts are staged in a root-owned temporary directory. The runner's temporary root-owned files are cleaned up
on exit; it does not write to the project's build tree or standard-library cache.

To rerun the already-built release test manually:

```sh
sudo sh tests/privileged/run.sh build/release/tests/isocmd-privileged-tests ./isocmd
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

The privileged suite requires Linux, util-linux (`losetup`, `unshare`, `mount`, `umount`, `script`),
coreutils, findutils, `sudo` for automatic elevation, two available loop devices,
a filesystem supporting loop-device direct I/O, at least two writer threads,
and root privileges with CAP_SYS_ADMIN and CAP_MKNOD. The runner creates a
fresh 16 MiB sparse backing file in a root-owned private temporary directory
and attaches a new loop device. No user-supplied device path is accepted by the
runner. The executable independently verifies that the block device is a whole
loop device and that its kernel-reported backing inode/device and size match
that fresh file before calling the real Linux ISO writer. It checks the written
bytes, alignment padding, completion flag and untouched trailing byte.

The end-to-end `w2usb` CLI suite creates a second 16 MiB backing file and
runs 26 independent scenarios against the two disposable loops. A private
mount namespace replaces `/dev` with a fresh tmpfs (only two verified block
nodes, `/dev/testa` and `/dev/testb`, plus minimal character devices and a
private devpts instance). A synthetic `/sys/block` supplies removable USB
metadata, capacity and in-flight counters. Both aliases are independently
checked with `LOOP_GET_STATUS64` against their root-owned backing files;
original loop descriptors stay open throughout the suite. No production
validation bypass or production source modification is needed. Before each
case the two verified loops are reset; full-device byte comparisons verify
successful writes, positional mapping, zeroed tails and no changes on
validation failure. Verbose cases use a 120x24 PTY with separate application
stderr; silent cases require empty raw stdout and stderr on dispatched jobs.
The 12 verified scenarios cover single, broadcast and paired writes, invalid
images and mixed success/failure batches in both modes, plus a multi-chunk
image with a partial final sector (nonzero padding sentinel and untouched
tail marker). Successful verified verbose rows must
report PASS rather than DONE, and all cases check exact exit status and both
full devices. Verbose verified runs assert separate Successful and Verified
counts, and ordinary writes must omit Verified. Checksum mismatch and readback
cancellation are injected in the separate privileged writer executable, which
asserts that a flushed write remains successful even if readback fails or is
cancelled. The PTY tests also check mixed verification results and cancellation
with distinct write/verification counts, including after a resize.
Mounted rejection uses a private tmpfs mount with the test device name as
its source, rather than mounting or formatting the loop. Busy rejection
uses a synthetic in-flight counter. Each case runs in its own shell so
regressions do not prevent later cases from executing. All-success verbose
cases require exit 0, even if the current implementation returns 1 due to
its separate UI and CLI progress vectors. This is an intentional regression
assertion, not an expected-failure exemption.

### Windows end-to-end verification

With `xorriso` or `genisoimage`, `parted`, and `mkfs.fat` (dosfstools),
the privileged runner generates a synthetic Windows PE-style ISO and tests the
real FAT-only writer. With `mkfs.ntfs` (ntfs-3g) it also tests the FAT32 + NTFS
installation layout. The kernel must support ISO9660, VFAT, loop partition
scanning and (for the full layout) the production writer's writable NTFS driver
(e.g. ntfs3). Missing ISO/format tools produce explicit SKIP messages;
a write, mount or verification failure with the tools present fails the suite.
Allow two additional available loop devices (one writable target and one
read-only source ISO loop) and sufficient free disk space for the filesystems.
Each layout is tested with `buffered_writes=off` (direct I/O) and
`buffered_writes=on` (buffered I/O). Each of the four cases uses a fresh sparse
1280 MiB backing file, needed for the 1 GiB ESP plus NTFS data partition.
Fixtures are not bootable Windows images.

The actual `writeIsoToDevice` dispatcher runs with `verifyRequested=true` in a
private mount namespace. A root-owned alias directory links only to the pinned,
independently checked disposable target and its kernel-created partition names;
normal host `/dev` remains visible for libmount's read-only ISO loop allocation.
No external target path is accepted by the runner. The test checks GPT, exact
partition count, filesystem types and labels, per-partition file routing,
empty files/directories, distinct multi-chunk payloads, exact written/verified
byte counts, completion state, and absence of leaked mounts. It independently
remounts the resulting filesystems read-only and compares fixture file bytes,
without using the production checksum algorithm. All four cases run separately
with mode-specific PASS/FAIL messages; a failed case propagates a nonzero exit
status. These are production-writer integration tests, not Windows
CLI/bootability tests.

### ISO mounting

If `xorriso` or `genisoimage` is installed, the runner also generates an ISO and
calls the production mount/unmount functions in a separate private mount
namespace, with a private empty directory bind-mounted over `/mnt`. It verifies
the mounted file contents and removal of the mount point. It also invokes the real CLI in the same namespace and checks mount/unmount success, failure, skip, mixed results, summaries and silent mode. If neither ISO creation
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
│   ├── sandbox.sh          # private HOME, XDG, TMPDIR and cleanup
│   ├── cli.sh              # stdout/stderr, exit code and result assertions
│   ├── ioFaults.h          # scoped inode-specific fault scripts
│   └── ioFaults.cpp        # test-only linker wrappers
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
│   ├── ioRecovery.cpp       # Windows copy engine syscall faults
│   ├── writeUi.cpp         # safe mapping, progress and injected writer child
│   └── run.sh
├── privileged/
│   ├── mount.cpp
│   ├── usbWriter.cpp
│   ├── windowsWriter.cpp   # real Windows verification in both write modes
│   ├── cli.sh              # root validation and isolated mount/unmount CLI
│   ├── writeCli.sh         # isolated real w2usb CLI, PTY and data checks
│   └── run.sh
├── main.cpp                # unit test registry
├── run.sh                  # unit runner and CLI regression tests
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
| CLI | Version/parser errors on stderr; conversion discovery, depth, deduplication, symlink exclusion, success, existing output skip, backend failure and cleanup, mixed validation/backend batches, final progress status, per-file messages and summary counters; raw silent output; exact ISO bytes; unprivileged permission errors for mount/unmount/write even in silent mode |
| Integration: filesystem | Actual copy/move/delete, overwrite at the low-level copy helper, counters, callbacks, missing source, failed rename cleanup, scan depth and directory symlinks |
| Integration: cancellation | Real SIGINT handler, pre-cancelled copy/move/delete/scan, active FIFO-fed copy cancellation, partial-file cleanup and recovery |
| Integration: USB writer | Invalid ISO, rejection of regular-file target, Windows copy engine (file bytes, directories, empty files, early EOF, missing file, cancellation) |
| Integration: I/O recovery | Windows copy engine: EINTR, short reads and positional writes, premature EOF, EIO, ENOSPC, zero writes, aligned/unaligned direct-write recovery, unsupported preallocation, active cancellation, exact partial-byte counters, truncation and clean retry (regular files); transient and permanent cleanup truncation faults, close EINTR/EIO after failed copies, FAT32 and NTFS O_DIRECT flags, configured FAT and NTFS chunk sizes, padding, exact CRC/progress, restart exhaustion and tail truncation failure; deterministic double-buffered read/write overlap in both write modes on both filesystem paths, worker cleanup and retry; buffered mode flags, unaligned short writes, exact CRC/progress, ENOSPC, cancellation and empty files |
| Integration: background import | History parsing, nested discovery, duplicate suppression, completion and worker accounting, stopped import, restart and missing history |
| Integration: PTY | Four terminal resizes during typed input, repaint, preservation of input and Readline/signal state restoration |
| Integration: menus | Real application main menu, invalid choice, resize with typed input, ManageISO, Convert2ISO and image search, ImportISO, Settings, Esc/EOF return and normal exit |
| Integration: interactive workflows | Real PageUp/PageDown and page jumps, live filter scroll, query replacement, resize, no-match retry, cancellation, staged copy, rejected/confirmed delete and move with filesystem checks; verbose copy, delete and move reports, resize repaint, Enter/Esc/Ctrl+D dismissal and no stale report text |
| Integration: interactive settings | Real settings editor help, multi-setting PageUp/PageDown and direct jump, invalid numeric value, typed input through resize, persisted update across process restart, skip, Escape and declined/confirmed reset |
| Integration: interactive appearance | Switch original → dracula → ocean through Settings, change base and skin, assert raw ANSI palette on menu borders, prompts, settings headers and ISO rows, row reset and width after resize, restart persistence and restoration of defaults |
| Integration: interactive import | Invalid and mixed directory input, overlapping directory scans, exact database contents, repeat import, report resize, statistics, independent history clearing and database clearing without deleting source files |
| Integration: interactive conversion | BIN/IMG discovery and empty cache, staged selection clearing, filtering, executing a staged source index from a different filtered view, exact ISO bytes, invalid-sector failure and cleanup, existing output preservation, database registration, cache listing and clearing; accept/decline verbose results, assert success/failure/skip details and dismiss with Enter/Esc/Ctrl+D |
| Integration: USB UI | Injected discovery/validation using harmless device names; invalid mapping retry, confirmation resize, decline and prefill, multi-device task ordering, completed/partial/failed summaries |
| Integration: progress | Real WriteScreen rendering at 10/50/99/100%, speed and bar glyphs, filename clipping, active/result resize, DONE/FAIL/CXL and interrupted summary; copy result resize at verbose prompt |
| Privileged | Linux writer on verified disposable loop device; 16 raw I/O scenarios (EINTR, short reads, aligned and unaligned short writes with bounded restart, premature EOF, EIO, ENOSPC, zero writes, final fsync EINTR retries and hard failure, close EIO during failure cleanup, active cancellation and clean retry), exact device prefix/padding and untouched tail; CLI USB argument and device validation (including multiple errors and duplicate targets); 26 real raw-writer CLI cases (14 ordinary and 12 with `--verify`): single/broadcast/paired success, invalid image, mixed batch, capacity/duplicate/mounted/busy rejection, verbose per-job status and summary, silent output, exit codes and full-device data checks; optional full Windows FAT-only and FAT32 + NTFS verified writes in direct and buffered modes on disposable partitioned loops, independent read-only remount and byte comparisons; optional private-namespace ISO mount/read/unmount and CLI success, skip, failure, mixed results, status/summary and silent mode |

## Safety and limitations

Each ordinary runner starts a new process with isolated HOME, XDG directories,
TMPDIR, and current directory, and uses timeouts to make hangs fail. Unit and
integration fixtures are temporary regular files, directories and a FIFO. USB discovery
uses explicit synthetic roots; normal production entry points still use the system roots.
The unprivileged CLI tests exercise conversion in the sandbox, and only dispatch mount, unmount
and USB commands when running unprivileged to check permission rejection. No real
mount or USB write is attempted by those tests. CLI assertions capture stdout and stderr
separately, remove ANSI CSI sequences and Readline nonprinting delimiters for text comparisons, check raw silence, and verify
exit codes and filesystem effects. They do not depend on timing, theme colors or progress
speed. Skips count as successful jobs, and validation errors affect the exit code but
are not counted as dispatched-job failures. The privileged CLI USB validation-only tests use missing arguments or `/dev/null`,
guarded as a character device without a sysfs block entry. End-to-end CLI writes
are restricted to independently verified disposable loop devices in the private
`/dev` and `/sys/block` namespace described above; no host block nodes are visible there. Real mount CLI tests run only inside the
private mount namespace with the sandbox bound over `/mnt`. The menu test uses a separately compiled app with
a private lock. The workflow test seeds an isolated database with temporary regular files and checks real copy, move and delete operations only on those fixtures. Run unprivileged.
The extended interactive settings, import and conversion cases each run in a fresh nested sandbox with an independent HOME, configuration, history, database and private menu lock. They launch the real menu binary and assert persisted configuration and exact file contents, not only rendered messages. No physical devices or privileged operations are used.
The USB UI tests inject only discovery/validation and a function-pointer writer;
production entry points still use the real discovery, validation and writer.
The fake writer never opens or writes any device. `/dev/null`, `/dev/zero` and
`/dev/full` are used only as existing canonical path fixtures for mapping and
read-only metadata checks. Telemetry tests inject timestamps, not wall-clock waits.
The database test additionally refuses to modify a database outside the runner's
sandbox or an already-existing database file. A failing group exits nonzero and
reports the failed assertion and source line.

The fault-injection wrappers are linked only into the integration and privileged
test executables. They match an explicitly registered file/device identity and
are inactive outside a scoped scenario. Short transfers perform real reduced-size
I/O; injected failures return the requested errno without touching the target.
The raw writer uses the existing verified loop device, and the Windows copy
engine uses temporary regular files. No injection is compiled into or linked
with the normal application. Raw progress is sampled periodically; tests check its upper bound on failure
and exact unpadded byte count on successful completion. Unaligned direct-write results are simulated with one actual aligned sector
write followed by a one-byte rewind and a reported count of sector size minus
one; the final byte may already be on disk. Tests require a successful restart,
correct final bytes/padding, unchanged trailing data and a successful flush.
Injected close failures release the actual descriptor first, as on Linux.
Permanent truncate failure is expected to leave a partial file and a failed
operation; a fresh copy must replace it. Transient truncate and final fsync
EINTR must be retried, but permanent sync errors must not report completion.
The raw writer restarts an unaligned write chunk at most eight times and
counts replayed bytes only once. Tests also verify restart exhaustion, final
fsync EINTR retries, and Windows cleanup ftruncate EINTR retries. By default, Windows FAT32 and NTFS use aligned O_DIRECT payload writes with bounded chunk replay
and exact final truncation. NTFS retains best-effort preallocation; FAT32 initializes
one chunk at a time and drains any dirty final page after truncation. No asynchronous
range pacing remains in the Windows copy engine;
the final syncfs still covers filesystem metadata and device-cache flushing. Optional buffered mode writes exact logical bytes without direct-I/O padding, preallocation or chunk initialization; final syncfs still flushes the data before successful completion.
The suite does not yet simulate all cleanup syscalls (e.g. unmount/rmdir),
full Windows filesystem syncfs/remount fault injection, Windows end-to-end CLI or corruption/cancellation cases on real FAT32/NTFS volumes, all converters, successful physical-device flashing through either CLI or UI, every interactive operation or all possible thread
interleavings. The unprivileged Windows copy-engine tests use regular filesystem paths; the optional privileged Windows tests exercise actual FAT32/NTFS volumes. The privileged Linux writer fixture has a synthetic
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

### USB checksum verification

The unit suite checks the CRC-64/ECMA-182 reference vector, chunked updates, file sizes, corruption, cancellation and progress-state moves. Integration tests exercise checksum capture in the Windows copy engine and the interactive USB flow with the persistent checksum setting enabled and disabled, without a second confirmation prompt. Settings tests cover migration, invalid values, persistence and reset. The privileged suite tests verified raw writes on its disposable loop fixture, including readback EINTR, short reads, corruption, EIO and cancellation. The real CLI suite also exercises `--verify` in verbose and silent modes (including multiple devices, partial batches, invalid images and sector padding). The optional privileged Windows tests now exercise the complete FAT-only and FAT32 + NTFS partition/format/copy/flush/unmount/read-only-remount/checksum workflow in both direct and buffered modes and independently compare the resulting file contents. Windows checksum mismatch and cancellation remain covered at the file/copy-engine level, not by the full partitioned workflow.
