#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[ "$#" -eq 1 ] || { echo "Usage: $0 PRIVILEGED_BINARY" >&2; exit 2; }
binary=$(realpath "$1")
if [ "$(id -u)" -ne 0 ]; then
    printf 'Privilege required. Build as an ordinary user, then explicitly run:\n  sudo sh %s %s\n' "$(realpath "$0")" "$binary" >&2
    exit 2
fi
for cmd in losetup truncate timeout stat install; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 2; }; done
# Ignore caller-controlled TMPDIR when privileged; create a new root-owned directory.
export TMPDIR=/tmp
. "$(dirname "$0")/../common/sandbox.sh"
setup_sandbox
loop=''
cleanup() {
    if [ -n "$loop" ]; then
        # Detach only if the association still points to our own backing file.
        if [ "$(losetup -n -O BACK-FILE -- "$loop" 2>/dev/null || :)" = "$sandbox/backing.img" ]; then
            if ! losetup -d -- "$loop"; then echo "Failed to detach $loop; keeping $sandbox" >&2; return 1; fi
        fi
    fi
    rm -rf -- "$sandbox"
}
trap cleanup EXIT
# Stage outside /mnt: the private mount namespace hides the original /mnt.
install -m 700 -- "$binary" "$sandbox/isocmd-privileged-tests"
binary="$sandbox/isocmd-privileged-tests"
truncate -s 16M backing.img
loop=$(losetup --find --show -- "$sandbox/backing.img")
case "$loop" in /dev/loop[0-9]*) ;; *) echo 'Not a loop device' >&2; exit 1;; esac
[ -b "$loop" ] && [ ! -L "$loop" ]
[ "$(losetup -n -O BACK-FILE -- "$loop")" = "$sandbox/backing.img" ]
timeout -k 5s 120s "$binary" "$loop"

# Optional real ISO mounting test. The private mount namespace hides all host
# /mnt contents, and only the generated ISO is handed to the application.
if command -v xorriso >/dev/null; then
    iso_tool=xorriso
elif command -v genisoimage >/dev/null; then
    iso_tool=genisoimage
else
    echo 'SKIP: mount/unmount test (install xorriso or genisoimage)'
    exit 0
fi
for cmd in unshare mount; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 2; }; done
mkdir "$sandbox/iso-tree" "$sandbox/mnt"
printf 'isocmd mount test\n' > "$sandbox/iso-tree/marker.txt"
if [ "$iso_tool" = xorriso ]; then
    xorriso -as mkisofs -quiet -R -o "$sandbox/mount.iso" "$sandbox/iso-tree"
else
    genisoimage -quiet -R -o "$sandbox/mount.iso" "$sandbox/iso-tree"
fi
timeout -k 5s 120s unshare --mount --propagation private sh -eu -c '
    mount --bind "$ISOCMD_TEST_SANDBOX/mnt" /mnt
    exec "$1" --mount-test
' sh "$binary"
