#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
if [ "$#" -ne 2 ]; then
    echo "Usage: $0 UNIT_BINARY ISOCMD_BINARY" >&2
    exit 2
fi
unit=$(realpath "$1")
app=$(realpath "$2")
test_dir=$(realpath "$(dirname "$0")")
. "$test_dir/common/sandbox.sh"
setup_sandbox
. "$test_dir/common/cli.sh"
timeout -k 5s 180s "$unit"

# Parser failures must occur before operation dispatch. No privileged targets.
expect_error() {
    expected_text=$1; shift
    cli_run "CLI parser: $*" 1 "$@"
    cli_has stderr 'Error:'
    cli_has stderr "$expected_text"
    cli_empty stdout
    cli_pass
}
for flag in --version -v; do
    cli_run "CLI version: $flag" 0 "$flag"
    cli_has stdout 'Iso Commander v'
    cli_empty stderr
    cli_pass
done
expect_error "Unknown action: 'not-an-action'." not-an-action dummy
expect_error "Option '--silent' must follow the action keyword." --silent mount
expect_error "Invalid depth option: '-dinvalid'." mount -dinvalid dummy
expect_error "Invalid depth option: '-d9999999999999999999'." c2iso -d9999999999999999999 dummy
expect_error "Invalid depth option: '-d'." c2iso -d dummy
expect_error "Invalid depth option: '-d1x'." c2iso --silent -d1x dummy
expect_error "Option '--silent' must precede all paths." mount dummy --silent
expect_error "Option '-d0' must precede all paths." c2iso dummy -d0
expect_error 'ISO paths must precede device paths' w2usb /dev/isocmd-test-does-not-exist dummy.iso
expect_error "Option '--silent' must precede all paths." w2usb dummy.iso --silent
expect_error "Option '--verify' must precede all paths." w2usb dummy.iso --verify
expect_error "Option '--verify' is only supported by w2usb." c2iso --verify dummy

# Real CLI dispatch with deterministic raw sectors, no device writes.
mkdir -p images/nested empty
mkfifo not-image
printf 'unsupported' > unsupported.txt
dd if=/dev/zero of=images/nested/sample.img bs=2352 count=2 status=none
printf '\001' | dd of=images/nested/sample.img bs=1 seek=15 conv=notrunc status=none
printf '\001' | dd of=images/nested/sample.img bs=1 seek=2367 conv=notrunc status=none
head -c 2048 /dev/zero | tr '\000' A > sector-a
head -c 2048 /dev/zero | tr '\000' B > sector-b
dd if=sector-a of=images/nested/sample.img bs=1 seek=16 conv=notrunc status=none
dd if=sector-b of=images/nested/sample.img bs=1 seek=2368 conv=notrunc status=none
cat sector-a sector-b > expected.iso
sample="$PWD/images/nested/sample.img"
output="$PWD/images/nested/sample.iso"
# A complete second sector with mode zero makes conversion fail after writing
# the first sector; the CLI must remove the partial ISO.
cp "$sample" broken.img
printf '\000' | dd of=broken.img bs=1 seek=2367 conv=notrunc status=none

cli_run 'c2iso: no paths' 0 c2iso
cli_has stdout 'No files to c2iso.'; cli_empty stderr; cli_pass
cli_run 'c2iso: empty directory' 0 c2iso "$PWD/empty"
cli_has stdout 'Scanning directory '; cli_has stdout '(max depth: unlimited)...'
cli_has stdout 'No convertible image files found.'; cli_empty stderr; cli_pass
cli_run 'c2iso: surface scan' 0 c2iso -d0 "$PWD/images"
cli_has stdout '(surface scan)...'; cli_has stdout 'No convertible image files found.'
cli_empty stderr; [ ! -e "$output" ] || cli_fail 'unexpected nested output'; cli_pass
cli_run 'c2iso: verbose success and depth 1' 0 c2iso -d1 "$PWD/images"
cli_has stdout '(max depth: 1)...'; cli_has stdout 'Located 1 image file; Attempting to convert...'
cli_has stdout "Convert2ISO: '$output': IMG → ISO."
cli_summary Conversion 1 0; cli_empty stderr
cmp expected.iso "$output" || cli_fail 'incorrect ISO bytes'; cli_pass

printf 'keep-existing-output' > "$output"
cli_run 'c2iso: verbose existing output skip' 0 c2iso "$sample"
cli_record stdout 'Convert2ISO:' 'sample.img' 'ISOalrExists → Skipped.'
cli_lacks stdout 'IMG → ISO.'; cli_summary Conversion 1 0; cli_empty stderr
[ "$(cat "$output")" = 'keep-existing-output' ] || cli_fail 'existing ISO overwritten'; cli_pass
rm "$output"

cli_run 'c2iso: invalid input paths' 1 c2iso "$PWD/missing.img" "$PWD/unsupported.txt" "$PWD/not-image"
cli_has stderr "Warning: '$PWD/missing.img' does not exist, skipping."
cli_has stderr "Warning: '$PWD/unsupported.txt' is not a supported image (bin/img/ccd/mdf/nrg/chd/daa/gbi), skipping."
cli_has stderr "Warning: '$PWD/not-image' is not a valid file or directory, skipping."
cli_has stdout 'No convertible image files found.'; cli_lacks stdout 'Conversion summary:'; cli_pass

cli_run 'c2iso: backend failure and partial-output cleanup' 1 c2iso "$PWD/broken.img"
cli_record stdout 'Convert2ISO:' 'broken.img' ': Failed.'
cli_summary Conversion 0 1; cli_empty stderr
[ ! -e broken.iso ] || cli_fail 'partial ISO retained'; cli_pass

cli_run 'c2iso: mixed backend success and failure' 1 c2iso "$sample" "$PWD/broken.img"
cli_has stdout 'Located 2 image files; Attempting to convert...'
cli_has stdout "Convert2ISO: '$output': IMG → ISO."; cli_record stdout 'Convert2ISO:' 'broken.img' ': Failed.'
cli_summary Conversion 1 1; cli_empty stderr
cmp expected.iso "$output" || cli_fail 'valid job failed'; [ ! -e broken.iso ] || cli_fail 'partial ISO retained'; cli_pass
rm "$output"

cli_run 'c2iso: mixed validation error and success' 1 c2iso "$PWD/missing.img" "$sample"
cli_has stderr 'does not exist, skipping.'; cli_has stdout "Convert2ISO: '$output': IMG → ISO."
# Summary counts dispatched jobs, not path validation errors.
cli_summary Conversion 1 0; cmp expected.iso "$output" || cli_fail 'valid job failed'; cli_pass
rm "$output"

ln -s nested images/alias
ln -s "$PWD/broken.img" images/ignored.img
cli_run 'c2iso: negative depth, symlink exclusion and deduplication' 0 c2iso -d-1 "$PWD/images" "$sample" "$sample"
cli_has stderr 'Warning: Negative depth; using unlimited depth.'
cli_has stdout '(max depth: unlimited)...'; cli_has stdout 'Located 1 image file; Attempting to convert...'
cli_summary Conversion 1 0; cmp expected.iso "$output" || cli_fail 'valid job failed'
[ ! -e images/ignored.iso ] || cli_fail 'followed symlink'; cli_pass
rm "$output"

cli_run 'c2iso: silent no paths' 0 c2iso --silent
cli_quiet; cli_pass
cli_run 'c2iso: silent surface scan' 0 c2iso --silent -d0 "$PWD/images"
cli_quiet; [ ! -e "$output" ] || cli_fail 'unexpected nested output'; cli_pass
cli_run 'c2iso: silent depth 1 success' 0 c2iso --silent -d1 "$PWD/images"
cli_quiet; cmp expected.iso "$output" || cli_fail 'incorrect ISO bytes'; cli_pass
rm "$output"
cli_run 'c2iso: silent negative depth and success' 0 c2iso --silent -d-1 "$PWD/images"
cli_quiet; cmp expected.iso "$output" || cli_fail 'incorrect ISO bytes'; cli_pass
printf 'keep-existing-output' > "$output"
cli_run 'c2iso: silent existing output' 0 c2iso --silent "$sample"
cli_quiet; [ "$(cat "$output")" = 'keep-existing-output' ] || cli_fail 'existing ISO overwritten'; cli_pass
rm "$output"
cli_run 'c2iso: silent mixed validation and success' 1 c2iso --silent "$PWD/missing.img" "$sample"
cli_quiet; cmp expected.iso "$output" || cli_fail 'valid job failed'; cli_pass
cli_run 'c2iso: silent backend failure' 1 c2iso --silent "$PWD/broken.img"
cli_quiet; [ ! -e broken.iso ] || cli_fail 'partial ISO retained'; cli_pass

# Only dispatch privileged actions here when permission checks must reject them.
if [ "$(id -u)" -ne 0 ]; then
    for mode in normal silent; do
        set --
        [ "$mode" != silent ] || set -- --silent
        cli_run "mount: $mode permission rejection" 1 mount "$@" "$PWD/empty"
        cli_has stderr 'Error: Root privileges required for mounting ISOs.'; cli_empty stdout; cli_pass
        for action in umount unmount; do
            cli_run "$action: $mode permission rejection" 1 "$action" "$@" -dinvalid "$PWD/empty"
            cli_has stderr 'Error: Root privileges required for unmounting ISOs.'; cli_empty stdout; cli_pass
        done
        cli_run "w2usb: $mode permission rejection" 1 w2usb "$@" -dinvalid "$PWD/expected.iso" /dev/isocmd-test-does-not-exist
        cli_has stderr 'Validation errors:'; cli_has stderr 'Root privileges are required'; cli_empty stdout; cli_pass
    done
else
    printf '%s\n' 'SKIP: unprivileged CLI permission checks (root)'
fi
