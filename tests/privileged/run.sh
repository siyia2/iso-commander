#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[ "$#" -eq 2 ] || { echo "Usage: $0 PRIVILEGED_BINARY ISOCMD_BINARY" >&2; exit 2; }
binary=$(realpath "$1")
app=$(realpath "$2")
test_dir=$(realpath "$(dirname "$0")")
if [ "$(id -u)" -ne 0 ]; then
    printf 'Privilege required. Build as an ordinary user, then explicitly run:\n  sudo sh %s %s %s\n' "$(realpath "$0")" "$binary" "$app" >&2
    exit 2
fi
for cmd in losetup truncate timeout stat install; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 2; }; done
# Ignore caller-controlled TMPDIR when privileged; create a new root-owned directory.
export TMPDIR=/tmp
. "$test_dir/../common/sandbox.sh"
setup_sandbox
loop=''
loop2=''
cleanup() {
    cleanup_failed=0
    # Detach only devices still associated with their own backing files.
    for slot in 1 2; do
        case "$slot" in
            1) target=$loop; backing="$sandbox/backing.img" ;;
            2) target=$loop2; backing="$sandbox/backing2.img" ;;
        esac
        [ -n "$target" ] || continue
        association=$(losetup -n -O BACK-FILE -- "$target" 2>/dev/null || :)
        if [ "$association" = "$backing" ]; then
            if ! losetup -d -- "$target"; then
                echo "Failed to detach $target; keeping $sandbox" >&2
                cleanup_failed=1
            fi
        fi
    done
    [ "$cleanup_failed" -eq 0 ] || return 1
    rm -rf -- "$sandbox"
}
trap cleanup EXIT
# Stage outside /mnt: the private mount namespace hides the original /mnt.
install -m 700 -- "$binary" "$sandbox/isocmd-privileged-tests"
binary="$sandbox/isocmd-privileged-tests"
install -m 700 -- "$app" "$sandbox/isocmd-cli"
install -m 600 -- "$test_dir/cli.sh" "$sandbox/cli-tests.sh"
install -m 600 -- "$test_dir/../common/cli.sh" "$sandbox/cli-helpers.sh"
sh "$sandbox/cli-tests.sh" validation
truncate -s 16M backing.img
loop=$(losetup --find --show -- "$sandbox/backing.img")
case "$loop" in /dev/loop[0-9]*) ;; *) echo 'Not a loop device' >&2; exit 1;; esac
[ -b "$loop" ] && [ ! -L "$loop" ]
[ "$(losetup -n -O BACK-FILE -- "$loop")" = "$sandbox/backing.img" ]
raw_rc=0
timeout -k 5s 120s "$binary" "$loop" || raw_rc=$?
# Only continue using a fixture after a normal test return (0 or 1).
# A timeout or signal could leave an unfinished writer.
[ "$raw_rc" -le 1 ] || exit "$raw_rc"

# End-to-end raw Linux writer CLI tests, independent of ISO creation tools.
# Capture the status so the existing mount suite still runs if a CLI regression fails.
for cmd in unshare mount umount mknod cmp dd find script stty; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 2; }; done
truncate -s 16M backing2.img
loop2=$(losetup --find --show -- "$sandbox/backing2.img")
[ "$loop" != "$loop2" ] || exit 2
for slot in 1 2; do
    case "$slot" in 1) target=$loop; backing="$sandbox/backing.img" ;; 2) target=$loop2; backing="$sandbox/backing2.img" ;; esac
    case "$target" in /dev/loop[0-9]*) ;; *) echo 'Not a loop device' >&2; exit 2;; esac
    [ -b "$target" ] && [ ! -L "$target" ] && [ "$(stat -c %t "$target")" = 7 ] || exit 2
    [ "$(losetup -n -O BACK-FILE -- "$target")" = "$backing" ] || exit 2
done
install -m 600 -- "$test_dir/writeCli.sh" "$sandbox/write-cli-tests.sh"
parent_ns=$(stat -Lc '%i' /proc/self/ns/mnt)
write_cli_rc=0
timeout -k 5s 600s unshare --mount --propagation private sh "$sandbox/write-cli-tests.sh" setup "$parent_ns" "$loop" "$loop2" || write_cli_rc=$?


# Optional real ISO mounting test. The private mount namespace hides all host
# /mnt contents, and only the generated ISO is handed to the application.
if command -v xorriso >/dev/null; then
    iso_tool=xorriso
elif command -v genisoimage >/dev/null; then
    iso_tool=genisoimage
else
    echo 'SKIP: mount/unmount test (install xorriso or genisoimage)'
    [ "$raw_rc" -eq 0 ] || exit "$raw_rc"
    exit "$write_cli_rc"
fi
for cmd in unshare mount; do command -v "$cmd" >/dev/null || { echo "Missing $cmd" >&2; exit 2; }; done
mkdir "$sandbox/iso-tree" "$sandbox/mnt"
printf 'isocmd mount test\n' > "$sandbox/iso-tree/marker.txt"
if [ "$iso_tool" = xorriso ]; then
    xorriso -as mkisofs -quiet -R -o "$sandbox/mount.iso" "$sandbox/iso-tree"
else
    genisoimage -quiet -R -o "$sandbox/mount.iso" "$sandbox/iso-tree"
fi
timeout -k 5s 300s unshare --mount --propagation private sh -eu -c '
    mount --bind "$ISOCMD_TEST_SANDBOX/mnt" /mnt
    "$1" --mount-test
    exec sh "$ISOCMD_TEST_SANDBOX/cli-tests.sh" mount
' sh "$binary"

[ "$raw_rc" -eq 0 ] || exit "$raw_rc"
exit "$write_cli_rc"
