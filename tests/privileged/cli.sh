#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later

# Checks privileged CLI validation and isolated mount/unmount operations.

# Invoked only by the privileged runner with root-owned staged fixtures.
set -eu
[ "$#" -eq 1 ] && [ "$(id -u)" -eq 0 ] || exit 2
sandbox=${ISOCMD_TEST_SANDBOX:?}
[ "$PWD" = "$sandbox" ] && [ "$(stat -c '%u:%a' "$sandbox")" = 0:700 ] || exit 2
app="$sandbox/isocmd-cli"
. "$sandbox/cli-helpers.sh"

if [ "$1" = validation ]; then
    # No valid device is ever supplied: fail closed before a writer can run.
    # /dev/null is a character device and has no corresponding sysfs block entry.
    [ -c /dev/null ] && [ ! -e /sys/block/null ] || exit 2
    printf 'fixture' > valid-name.iso
    : > empty.iso
    printf 'fixture' > wrong.txt
    for mode in normal silent; do
        set --
        [ "$mode" != silent ] || set -- --silent
        cli_run "w2usb: $mode no ISO arguments" 1 w2usb "$@"
        cli_has stderr 'Validation errors:'; cli_line stderr ' • No ISO files specified.'; cli_empty stdout; cli_pass
        cli_run "w2usb: $mode no device arguments" 1 w2usb "$@" "$PWD/valid-name.iso"
        cli_has stderr 'Validation errors:'; cli_line stderr ' • No target devices specified.'; cli_empty stdout; cli_pass
        cli_run "w2usb: $mode mismatched counts" 1 w2usb "$@" "$PWD/valid-name.iso" "$PWD/empty.iso" /dev/null
        cli_line stderr ' • Got 2 ISOs and 1 devices; use 1 ISO for all devices, or one device per ISO.'; cli_empty stdout; cli_pass
        cli_run "w2usb: $mode invalid device" 1 w2usb "$@" "$PWD/valid-name.iso" /dev/null
        cli_line stderr " • '/dev/null' is not a removable USB flash device."; cli_empty stdout; cli_pass
        cli_run "w2usb: $mode aggregated validation and duplicate target" 1 w2usb "$@" "$PWD/missing.iso" "$PWD/wrong.txt" "$PWD/empty.iso" /dev/null /dev/null /dev/null
        cli_has stderr 'Validation errors:'
        cli_line stderr " • '$PWD/missing.iso' is not a regular file."
        cli_line stderr " • '$PWD/wrong.txt' is not an ISO file."
        cli_line stderr " • '$PWD/empty.iso' is empty or unreadable."
        cli_line stderr " • '/dev/null' is not a removable USB flash device."
        cli_line stderr " • '/dev/null' is listed more than once."
        [ "$(grep -Fxc -- " • '/dev/null' is listed more than once." cli.stderr)" -eq 1 ] || cli_fail 'duplicate target diagnostic repeated'
        cli_empty stdout; cli_pass
        cli_run "w2usb: $mode repeated invalid ISO and target" 1 w2usb "$@" "$PWD/missing.iso" "$PWD/wrong.txt" "$PWD/missing.iso" /dev/null /dev/null /dev/null
        expected=$(printf '%s\n' \
            " • '$PWD/missing.iso' is not a regular file." \
            " • '$PWD/wrong.txt' is not an ISO file." \
            " • '/dev/null' is not a removable USB flash device." \
            " • '/dev/null' is listed more than once.")
        [ "$(grep -F ' • ' cli.stderr)" = "$expected" ] || cli_fail 'unexpected USB diagnostics or order'
        cli_empty stdout; cli_pass
    done
    exit 0
fi

[ "$1" = mount ] || exit 2
# Do not run any mount/unmount command unless the existing private namespace
# and the bind-mounted sandbox /mnt are verified independently.
[ "$(stat -Lc '%d:%i' /mnt)" = "$(stat -Lc '%d:%i' "$sandbox/mnt")" ] || exit 2
[ "$(stat -Lc '%i' /proc/self/ns/mnt)" != "$(stat -Lc '%i' /proc/1/ns/mnt)" ] || exit 2
[ -f "$sandbox/mount.iso" ] && [ ! -L "$sandbox/mount.iso" ] || exit 2
[ ! -e /mnt/ISOs ] || { [ -d /mnt/ISOs ] && [ ! -L /mnt/ISOs ] && [ -z "$(ls -A /mnt/ISOs)" ]; } || exit 2
mkdir -p /mnt/ISOs empty-mount-input
printf 'not an ISO' > bad.iso
printf 'wrong extension' > wrong-mount.txt

cli_run 'mount: no paths' 0 mount
cli_has stdout 'No ISO files to mount.'; cli_empty stderr; cli_pass
cli_run 'mount: empty surface scan' 0 mount -d0 "$PWD/empty-mount-input"
cli_has stdout '(surface scan)...'; cli_has stdout 'No ISO files found to mount.'; cli_empty stderr; cli_pass
cli_run 'mount: repeated invalid paths' 1 mount "$PWD/missing.iso" "$PWD/wrong-mount.txt" "$PWD/missing.iso" "$PWD/wrong-mount.txt"
expected=$(printf '%s\n' \
    "Warning: '$PWD/missing.iso' does not exist, skipping." \
    "Warning: '$PWD/wrong-mount.txt' is not an ISO file, skipping.")
[ "$(grep -F 'Warning: ' cli.stderr)" = "$expected" ] || cli_fail 'unexpected mount path warnings or order'
cli_has stdout 'No ISO files to mount.'; cli_pass
cli_run 'mount: silent repeated invalid paths' 1 mount --silent "$PWD/missing.iso" "$PWD/missing.iso"
cli_quiet; cli_pass
cli_run 'mount: invalid image failure' 1 mount "$PWD/bad.iso"
cli_record stdout '✗ mount: ' 'bad.iso' '{badFS}'
cli_summary Mount 0 1; [ -z "$(ls -A /mnt/ISOs)" ] || cli_fail 'unexpected mount'; cli_pass
cli_run 'mount: mixed success and invalid image' 1 mount "$PWD/mount.iso" "$PWD/bad.iso"
cli_has stdout 'Located 2 ISO files; Attempting to mount...'; cli_record stdout '✓ mount: ' 'mount.iso' ' → '
cli_record stdout '✓ mount: ' 'mount.iso' '{iso9660}'
cli_record stdout '✗ mount: ' 'bad.iso' '{badFS}'; cli_summary Mount 1 1
# Verify the sole mount is actually our fixture, not just a success message.
set -- /mnt/ISOs/*
[ "$#" -eq 1 ] && [ -d "$1" ] || cli_fail 'expected one mounted image'
mount_point=$1
[ "$(cat "$mount_point/marker.txt")" = 'isocmd mount test' ] || cli_fail 'incorrect mounted contents'
cli_pass
cli_run 'mount: already mounted skip' 0 mount "$PWD/mount.iso"
cli_record stdout '– mount: ' 'mount.iso' '{alrMounted}'; cli_summary Mount 0 0 1
cli_has stdout '] 100% (0/1)'; cli_pass
cli_run 'mount: mixed skip and failure' 1 mount "$PWD/mount.iso" "$PWD/bad.iso"
cli_summary Mount 0 1 1; cli_has stdout '] 100% (0/2)'; cli_pass
cli_run 'umount: verbose success' 0 umount "$mount_point"
cli_has stdout 'Located 1 mount point; Attempting to unmount...'
cli_has stdout "✓ umount: $mount_point"; cli_summary Unmount 1 0
[ ! -e "$mount_point" ] || cli_fail 'mount directory retained'; cli_pass

# The supported-path hint appears exactly once, before either type of invalid path.
for action in umount unmount; do
    cli_run "$action: invalid paths and disallowed directory" 1 "$action" "$PWD/missing-mount" "$PWD/empty-mount-input" "$PWD/missing-mount" "$PWD/missing-mount-2" "$PWD/empty-mount-input"
    expected=$(printf '%s\n' \
        'Warning: Only /mnt/ISOs or /mnt/ISOs/isoFolder are valid.' \
        "Warning: '$PWD/missing-mount' does not exist or is not a valid mount point, skipping." \
        "Warning: '$PWD/empty-mount-input' is not allowed." \
        "Warning: '$PWD/missing-mount-2' does not exist or is not a valid mount point, skipping.")
    [ "$(grep -F 'Warning: ' cli.stderr)" = "$expected" ] || cli_fail 'unexpected unmount path warnings or hint order'
    cli_has stdout 'No ISO mount points found to unmount.'; cli_pass
    cli_run "$action: silent invalid paths" 1 "$action" --silent "$PWD/empty-mount-input" "$PWD/missing-mount" "$PWD/missing-mount"
    cli_quiet; cli_pass
done
cli_run 'umount: empty all scan' 0 umount all
cli_has stdout 'Scanning /mnt/ISOs for ISO mount points (surface scan)...'
cli_has stdout 'No ISO mount points found to unmount.'; cli_empty stderr; cli_pass

# A nonempty ordinary directory cannot be treated as a successful unmount.
# Successful unmounts remove the empty parent /mnt/ISOs. Recreate the fixture.
mkdir -p /mnt/ISOs/not-mounted
printf 'keep' > /mnt/ISOs/not-mounted/marker
cli_run 'umount: operation failure' 1 umount /mnt/ISOs/not-mounted
cli_record stdout '✗ umount: ' 'not-mounted' '{notAnISO}'; cli_summary Unmount 0 1
[ "$(cat /mnt/ISOs/not-mounted/marker)" = keep ] || cli_fail 'ordinary directory modified'; cli_pass
rm /mnt/ISOs/not-mounted/marker; rmdir /mnt/ISOs/not-mounted

cli_run 'mount: silent invalid image' 1 mount --silent "$PWD/bad.iso"
cli_quiet; cli_pass
cli_run 'mount: silent success' 0 mount --silent "$PWD/mount.iso"
cli_quiet; [ "$(cat "$mount_point/marker.txt")" = 'isocmd mount test' ] || cli_fail 'incorrect mounted contents'; cli_pass
cli_run 'mount: silent already-mounted skip' 0 mount --silent "$PWD/mount.iso"
cli_quiet; cli_pass
cli_run 'unmount: silent alias and all scan' 0 unmount --silent all
cli_quiet; [ ! -e "$mount_point" ] || cli_fail 'mount directory retained'; cli_pass
cli_run 'umount: silent invalid path' 1 umount --silent "$PWD/missing-mount"
cli_quiet; cli_pass

# Missing mount directory is still a successful empty scan; a non-directory
# at the same path triggers the existing filesystem-error exit status.
# The private /mnt namespace has been verified above.
if [ -d /mnt/ISOs ]; then rmdir /mnt/ISOs; fi
cli_run 'umount: absent mount directory' 0 umount all
cli_has stdout 'No ISO mount points found to unmount.'; cli_empty stderr; cli_pass
: > /mnt/ISOs
cli_run 'umount: scan filesystem error' 1 umount all
cli_has stderr "Warning: '/mnt/ISOs' Error scanning directory:"
grep -Eq '\.$' cli.stderr || cli_fail 'missing diagnostic period'
cli_pass
cli_run 'unmount: silent scan filesystem error' 1 unmount --silent all
cli_quiet; cli_pass
rm /mnt/ISOs

cli_run 'mount: negative depth before silent' 0 mount -d-1 --silent "$PWD/empty-mount-input"
cli_quiet; cli_pass
