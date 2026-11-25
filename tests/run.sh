#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 UNIT_BINARY ISOCMD_BINARY" >&2
    exit 2
fi
unit=$(realpath "$1")
app=$(realpath "$2")
. "$(dirname "$0")/common/sandbox.sh"
setup_sandbox
timeout -k 5s 180s "$unit"

# These invocations must exit before dispatching to an operation handler.
# Do not add valid mount/unmount/write commands to this suite.
expect() {
    expected_rc=$1 expected_text=$2
    shift 2
    rc=0
    timeout -k 2s 10s "$app" "$@" </dev/null >cli-output 2>&1 || rc=$?
    if [ "$rc" -ne "$expected_rc" ] || ! grep -Fq -- "$expected_text" cli-output; then
        printf 'FAIL: isocmd %s (exit %s, expected %s)\n' "$*" "$rc" "$expected_rc" >&2
        cat cli-output >&2
        exit 1
    fi
    printf 'PASS: isocmd %s\n' "$*"
}
expect 0 'Iso Commander v' --version
expect 0 'Iso Commander v' -v
expect 1 "Unknown action: 'not-an-action'" not-an-action dummy
expect 1 "must follow the action keyword" --silent mount
expect 1 "Invalid depth option" mount -dinvalid dummy
expect 1 "Invalid depth option" c2iso -d9999999999999999999 dummy
expect 1 "must precede all paths" mount dummy --silent
expect 1 "must precede all paths" c2iso dummy -d0
expect 1 "ISO paths must precede device paths" w2usb /dev/isocmd-test-does-not-exist dummy.iso
expect 1 "must precede all paths" w2usb dummy.iso --silent
printf '%s\n' '10 CLI smoke cases passed'

# Real CLI dispatch with an exact two-sector CCD/IMG fixture; no device writes.
# These cases use only files beneath the private sandbox.
mkdir -p images/nested empty
# Build two Mode 1 raw sectors, each containing a deterministic 2048-byte payload.
dd if=/dev/zero of=images/nested/sample.img bs=2352 count=2 status=none
printf '\001' | dd of=images/nested/sample.img bs=1 seek=15 conv=notrunc status=none
printf '\001' | dd of=images/nested/sample.img bs=1 seek=2367 conv=notrunc status=none
head -c 2048 /dev/zero | tr '\000' A > sector-a
head -c 2048 /dev/zero | tr '\000' B > sector-b
dd if=sector-a of=images/nested/sample.img bs=1 seek=16 conv=notrunc status=none
dd if=sector-b of=images/nested/sample.img bs=1 seek=2368 conv=notrunc status=none
cat sector-a sector-b > expected.iso
run_silent() {
    wanted=$1; shift
    rc=0
    timeout -k 2s 20s "$app" "$@" </dev/null >cli-output 2>&1 || rc=$?
    if [ "$rc" -ne "$wanted" ]; then
        printf 'FAIL: isocmd %s (exit %s, expected %s)\n' "$*" "$rc" "$wanted" >&2
        cat cli-output >&2; exit 1
    fi
}
expect 0 'No convertible image files found.' c2iso "$PWD/empty"
run_silent 0 c2iso --silent -d0 "$PWD/images"
[ ! -e images/nested/sample.iso ]
[ ! -s cli-output ]
run_silent 0 c2iso --silent -d1 "$PWD/images"
cmp expected.iso images/nested/sample.iso
[ ! -s cli-output ]
# Existing outputs must be preserved (successful no-op).
printf 'keep-existing-output' > images/nested/sample.iso
run_silent 0 c2iso --silent "$PWD/images/nested/sample.img"
[ "$(cat images/nested/sample.iso)" = 'keep-existing-output' ]
# Invalid inputs return failure; a mixed batch still processes valid files.
rm images/nested/sample.iso
run_silent 1 c2iso --silent "$PWD/missing.img" "$PWD/images/nested/sample.img"
cmp expected.iso images/nested/sample.iso
printf '%s\n' 'PASS: CLI conversion, depth, silent output, existing output and mixed batch'
# These operations must reject an unprivileged caller before inspecting host
# mount points or block devices. Refuse to run them as root.
if [ "$(id -u)" -ne 0 ]; then
    expect 1 'Root privileges required for mounting ISOs.' mount "$PWD/empty"
    expect 1 'Root privileges required for unmounting ISOs.' umount "$PWD/empty"
    expect 1 'Root privileges required for unmounting ISOs.' unmount "$PWD/empty"
    expect 1 'Root privileges are required' w2usb "$PWD/expected.iso" /dev/isocmd-test-does-not-exist
else
    printf '%s\n' 'SKIP: unprivileged CLI permission checks (root)'
fi
