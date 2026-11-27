#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

# Stages and validates disposable loop and mount-namespace fixtures.

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
windows_loop=''
wait_windows_detach() {
    remaining=50
    while [ "$(losetup -n -O BACK-FILE -- "$1" 2>/dev/null || :)" = "$sandbox/windows-backing.img" ]; do
        [ "$remaining" -gt 0 ] || return 1
        remaining=$((remaining - 1))
        sleep 0.1
    done
}
cleanup() {
    cleanup_failed=0
    # Detach only devices still associated with their own backing files.
    for slot in 1 2 windows; do
        case "$slot" in
            1) target=$loop; backing="$sandbox/backing.img" ;;
            2) target=$loop2; backing="$sandbox/backing2.img" ;;
            windows) target=$windows_loop; backing="$sandbox/windows-backing.img" ;;
        esac
        [ -n "$target" ] || continue
        association=$(losetup -n -O BACK-FILE -- "$target" 2>/dev/null || :)
        if [ "$association" = "$backing" ]; then
            if ! losetup -d -- "$target" || { [ "$slot" = windows ] && ! wait_windows_detach "$target"; }; then
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


# The Windows workflow needs real optical images, partition and format tools.
# Missing optional prerequisites are reported explicitly, not counted as passes.
iso_tool=''
if command -v xorriso >/dev/null; then
    iso_tool=xorriso
elif command -v genisoimage >/dev/null; then
    iso_tool=genisoimage
fi
make_iso() {
    if [ "$iso_tool" = xorriso ]; then
        xorriso -as mkisofs -quiet -R -J -o "$1" "$2"
    else
        genisoimage -quiet -R -J -o "$1" "$2"
    fi
}
windows_rc=0
make_windows_fixture() {
    layout=$1
    tree="$sandbox/windows-$layout-tree"
    mkdir -p "$tree/efi/boot" "$tree/boot" "$tree/sources" "$tree/extras/empty-dir"
    printf 'synthetic Windows boot manager\n' >"$tree/bootmgr"
    printf 'EFI payload\n' >"$tree/efi/boot/bootx64.efi"
    printf 'boot configuration\n' >"$tree/boot/bcd"
    printf 'different payload\n' >"$tree/extras/long file name.txt"
    : >"$tree/extras/empty.txt"
    # Cross the copy engine's 4 MiB chunk boundary with distinct markers.
    truncate -s 5242917 "$tree/sources/boot.wim"
    printf 'boot.wim start' | dd of="$tree/sources/boot.wim" bs=1 conv=notrunc status=none
    printf 'cross-chunk' | dd of="$tree/sources/boot.wim" bs=1 seek=4194300 conv=notrunc status=none
    printf 'last-byte' | dd of="$tree/sources/boot.wim" bs=1 seek=5242908 conv=notrunc status=none
    if [ "$layout" = full ]; then
        truncate -s 5242951 "$tree/sources/install.wim"
        printf 'install.wim distinct content' | dd of="$tree/sources/install.wim" bs=1 conv=notrunc status=none
        printf 'NTFS boundary' | dd of="$tree/sources/install.wim" bs=1 seek=4194300 conv=notrunc status=none
        printf 'NTFS tail' | dd of="$tree/sources/install.wim" bs=1 seek=5242942 conv=notrunc status=none
    fi
    make_iso "$sandbox/windows-$layout.iso" "$tree"
}
windows_case() {
    layout=$1
    mode=$2
    # The Windows writer allocates a 1 GiB ESP for full installation media.
    # Use a fresh sparse 1280 MiB image for each layout and write mode; never use a host disk.
    truncate -s 1280M "$sandbox/windows-backing.img"
    windows_loop=$(losetup --find --show --partscan -- "$sandbox/windows-backing.img")
    case "$windows_loop" in /dev/loop[0-9]*) ;; *) exit 2;; esac
    [ -b "$windows_loop" ] && [ ! -L "$windows_loop" ] && [ "$(stat -c %t "$windows_loop")" = 7 ] || exit 2
    [ "$(losetup -n -O BACK-FILE -- "$windows_loop")" = "$sandbox/windows-backing.img" ] || exit 2
    mkdir -m 700 "$sandbox/windows-dev"
    export ISOCMD_WINDOWS_PARENT_NS=$(stat -Lc '%i' /proc/self/ns/mnt)
    result=0
    # Keep host /dev available for libmount's read-only ISO loop allocation.
    # The only writable target given to the writer is a separately verified
    # private alias; partition aliases refer only to the pinned loop's
    # kernel-generated partition names.
    timeout -k 5s 240s unshare --mount --propagation private sh -eu -c '
        mount -t tmpfs -o mode=0700,dev,nosuid,noexec tmpfs "$ISOCMD_TEST_SANDBOX/windows-dev"
        exec "$1" --windows-test "$2" "$3" "$4"
    ' sh "$binary" "$windows_loop" "$layout" "$mode" || result=$?
    # Do not reuse the fixture after a timeout or signal. EXIT cleanup owns it.
    [ "$result" -le 1 ] || exit "$result"
    [ "$(losetup -n -O BACK-FILE -- "$windows_loop")" = "$sandbox/windows-backing.img" ] || exit 2
    losetup -d -- "$windows_loop"
    wait_windows_detach "$windows_loop" || exit 2
    windows_loop=''
    rm -- "$sandbox/windows-backing.img"
    rmdir "$sandbox/windows-dev"
    if [ "$result" -ne 0 ]; then windows_rc=1; fi
}
if [ -z "$iso_tool" ] || ! command -v parted >/dev/null || ! command -v mkfs.fat >/dev/null; then
    echo 'SKIP: Windows FAT-only and FAT32 + NTFS verification (install xorriso or genisoimage, parted and dosfstools)'
else
    make_windows_fixture fat-only
    windows_case fat-only direct
    windows_case fat-only buffered
    if command -v mkfs.ntfs >/dev/null; then
        make_windows_fixture full
        windows_case full direct
        windows_case full buffered
    else
        echo 'SKIP: Windows FAT32 + NTFS verification (install mkfs.ntfs from ntfs-3g)'
    fi
fi

# Optional real ISO mounting test. The private mount namespace hides all host
# /mnt contents, and only the generated ISO is handed to the application.
if [ -z "$iso_tool" ]; then
    echo 'SKIP: mount/unmount test (install xorriso or genisoimage)'
    [ "$raw_rc" -eq 0 ] || exit "$raw_rc"
    [ "$write_cli_rc" -eq 0 ] || exit "$write_cli_rc"
    exit "$windows_rc"
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
[ "$write_cli_rc" -eq 0 ] || exit "$write_cli_rc"
exit "$windows_rc"
