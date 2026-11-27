#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Only invoked by run.sh; never accepts a user-selected disk.
set -eu
sandbox=${ISOCMD_TEST_SANDBOX:?}
[ "$(id -u)" -eq 0 ] && [ "$PWD" = "$sandbox" ] && [ "$(stat -c '%u:%a' "$sandbox")" = 0:700 ] || exit 2
app="$sandbox/isocmd-cli"
verify="$sandbox/isocmd-privileged-tests"
self="$sandbox/write-cli-tests.sh"
a=/dev/testa
b=/dev/testb
sys="$sandbox/write-sys-block"
. "$sandbox/cli-helpers.sh"

verify_isolation() {
    [ "$(stat -Lc '%i' /proc/self/ns/mnt)" != "$ISOCMD_WRITE_PARENT_NS" ] || return 2
    [ "$(stat -Lc '%d:%i' /sys/block)" = "$(stat -Lc '%d:%i' "$sys")" ] || return 2
    [ -f /dev/.isocmd-write-fixture ] && [ "$(cat /dev/.isocmd-write-fixture)" = "$sandbox" ] || return 2
    [ "$(find /dev -type b | LC_ALL=C sort)" = "$(printf '%s\n' "$a" "$b")" ] || return 2
    "$verify" --verify-loop "$a" backing.img || return 2
    "$verify" --verify-loop "$b" backing2.img || return 2
}

if [ "$1" = setup ]; then
    [ "$#" -eq 4 ] || exit 2
    export ISOCMD_WRITE_PARENT_NS="$2"
    [ "$(stat -Lc '%i' /proc/self/ns/mnt)" != "$ISOCMD_WRITE_PARENT_NS" ] || exit 2
    original_a=$3 original_b=$4
    # Before hiding host /dev, check association and pin both original loops.
    [ "$original_a" != "$original_b" ] || exit 2
    for slot in a b; do
        case "$slot" in a) target=$original_a; backing="$sandbox/backing.img" ;; b) target=$original_b; backing="$sandbox/backing2.img" ;; esac
        case "$target" in /dev/loop[0-9]*) ;; *) exit 2;; esac
        [ -b "$target" ] && [ ! -L "$target" ] && [ "$(stat -c %t "$target")" = 7 ] || exit 2
        [ "$(losetup -n -O BACK-FILE -- "$target")" = "$backing" ] || exit 2
    done
    exec 8<"$original_a" 9<"$original_b"
    minor_a=$(printf '%d' "0x$(stat -c %T "$original_a")")
    minor_b=$(printf '%d' "0x$(stat -c %T "$original_b")")
    # All original block-device names disappear from /dev in this namespace.
    # Only the verified major/minor pairs are recreated; sysfs metadata is synthetic.
    mount --make-rprivate /
    mount -t tmpfs -o mode=0755,dev,nosuid,noexec tmpfs /dev
    # A private devpts instance gives verbose CLI output deterministic terminal
    # dimensions without exposing any host PTY or storage nodes.
    mkdir /dev/pts
    mount -t devpts -o newinstance,ptmxmode=0666,mode=0600,nosuid,noexec devpts /dev/pts
    ln -s pts/ptmx /dev/ptmx
    mknod -m 666 /dev/null c 1 3
    mknod -m 666 /dev/zero c 1 5
    mknod -m 666 /dev/random c 1 8
    mknod -m 666 /dev/urandom c 1 9
    mknod -m 600 "$a" b 7 "$minor_a"
    mknod -m 600 "$b" b 7 "$minor_b"
    printf '%s\n' "$sandbox" >/dev/.isocmd-write-fixture
    mkdir -p "$sys"
    for name in testa testb; do
        mkdir -p "$sys/$name/device"
        printf '1\n' >"$sys/$name/removable"
        printf '1234\n' >"$sys/$name/idVendor"
        printf 'Test\n' >"$sys/$name/device/model"
        printf '32768\n' >"$sys/$name/size"
        printf '0 0\n' >"$sys/$name/inflight"
    done
    mount --bind "$sys" /sys/block
    verify_isolation
    # Small distinct non-Windows optical-image fixtures. Aligned sizes keep
    # this test independent of the low-level writer's separate padding test.
    truncate -s 65536 a.iso b.iso
    printf '\001CD001' | dd of=a.iso bs=1 seek=32768 conv=notrunc status=none
    printf '\001CD001' | dd of=b.iso bs=1 seek=32768 conv=notrunc status=none
    printf 'fixture A' | dd of=a.iso bs=1 seek=4096 conv=notrunc status=none
    printf 'fixture B' | dd of=b.iso bs=1 seek=4096 conv=notrunc status=none
    printf 'not an optical image\n' > bad-write.iso
    truncate -s 17M oversized.iso
    truncate -s 16M zero.expected
    for name in a b; do
        cp "$name.iso" "$name.expected"
        truncate -s 16M "$name.expected"
    done
    # Exercise a verified write across the 4 MiB buffer boundary and a
    # partial final sector, with nonzero data at both boundaries.
    truncate -s 5242917 padded.iso
    printf '\001CD001' | dd of=padded.iso bs=1 seek=32768 conv=notrunc status=none
    printf 'cross-chunk' | dd of=padded.iso bs=1 seek=4194300 conv=notrunc status=none
    printf 'last-byte' | dd of=padded.iso bs=1 seek=5242908 conv=notrunc status=none
    cp padded.iso padded.expected
    truncate -s 16M padded.expected
    printf 'tail-marker' | dd of=padded.expected bs=1 seek=8388608 conv=notrunc status=none
    mkdir "$sandbox/write-mounted"
    # util-linux script preserves the child's exit status. Redirect application
    # stderr inside the PTY child, so only stdout is terminal-rendered.
    cat >"$sandbox/pty-cli" <<'PTY'
#!/bin/sh
set -eu
sandbox=${ISOCMD_TEST_SANDBOX:?}
printf '%s\n' "$@" >"$sandbox/pty-args"
: >"$sandbox/pty-app.stderr"
result=0
SHELL=/bin/sh script -q -e -c '
    stty rows 24 cols 120 || exit 2
    set --
    while IFS= read -r arg; do set -- "$@" "$arg"; done <"$ISOCMD_TEST_SANDBOX/pty-args"
    exec "$ISOCMD_TEST_SANDBOX/isocmd-cli" "$@" 2>"$ISOCMD_TEST_SANDBOX/pty-app.stderr"
' /dev/null || result=$?
cat "$sandbox/pty-app.stderr" >&2
exit "$result"
PTY
    chmod 700 "$sandbox/pty-cli"
    failed=0 total=0
    for scenario in capacity duplicate mounted busy invalid-image-silent invalid-image-verbose \
        single-silent broadcast-silent paired-silent partial-silent partial-verbose \
        single-verbose broadcast-verbose paired-verbose \
        verify-invalid-image-silent verify-invalid-image-verbose \
        verify-single-silent verify-broadcast-silent verify-paired-silent \
        verify-partial-silent verify-partial-verbose \
        verify-single-verbose verify-broadcast-verbose verify-paired-verbose \
        verify-padded-silent verify-padded-verbose; do
        total=$((total + 1))
        # Separate shells preserve errexit while allowing other cases to run
        # after a failed assertion.
        if sh "$self" case "$scenario"; then :; else
            failed=$((failed + 1))
            printf 'FAIL: w2usb scenario %s\n' "$scenario" >&2
        fi
    done
    printf 'w2usb CLI: %s/%s scenarios passed\n' "$((total - failed))" "$total"
    [ "$failed" -eq 0 ]
    exit
fi

[ "$#" -eq 2 ] && [ "$1" = case ] || exit 2
verify_isolation
# Reset only the two independently verified disposable devices. Compare the
# full 16 MiB, not just the source prefix, after every operation.
dd if=zero.expected of="$a" bs=1M conv=fsync status=none
dd if=zero.expected of="$b" bs=1M conv=fsync status=none
check_bytes() {
    cmp -s -- "$1.expected" "$a" || cli_fail "unexpected bytes on $a (expected $1)"
    cmp -s -- "$2.expected" "$b" || cli_fail "unexpected bytes on $b (expected $2)"
}
seed_padded_target() {
    # The first byte beyond the unpadded ISO must be overwritten by zero
    # padding; a marker well beyond the aligned image must be left intact.
    printf 'tail-marker' | dd of="$a" bs=1 seek=8388608 conv=notrunc,fsync status=none
    printf '\245' | dd of="$a" bs=1 seek=5242917 conv=notrunc,fsync status=none
}
capture_verbose() {
    app="$sandbox/pty-cli"
    cli_capture "$@"
    app="$sandbox/isocmd-cli"
}
verbose_result() {
    cli_has stdout "Status: write2usb → $1"
    if [ "$success_status" = PASS ]; then
        cli_has stdout "Written: $2 | Verified: $2 | Time Elapsed:"
    else
        cli_has stdout "Written: $2 | Time Elapsed:"
        cli_lacks stdout "Verified:"
    fi
    cli_empty stderr
}
validation_result() {
    cli_has stderr 'Validation errors:'
    cli_empty stdout
    check_bytes zero zero
}
# Reuse the same CLI assertions for ordinary and verified writes. Pass the
# flag as a real argument, never through shell word splitting or eval.
scenario=$2
success_status=DONE
suffix=''
case "$scenario" in
    verify-*)
        scenario=${scenario#verify-}
        success_status=PASS
        suffix=' (--verify)'
        set -- --verify ;;
    *) set -- ;;
esac
case "$scenario" in
    capacity)
        cli_capture 'w2usb: oversized image' w2usb "$PWD/oversized.iso" "$a"
        validation_result; cli_has stderr 'ISO exceeds target drive capacity:'
        cli_has stderr 'oversized.iso'; cli_has stderr "$a"; cli_expect_rc 1 ;;
    duplicate)
        cli_capture 'w2usb: duplicate valid target' w2usb "$PWD/a.iso" "$a" "$a"
        validation_result; cli_has stderr "'$a' is listed more than once"; cli_expect_rc 1 ;;
    mounted)
        # An actual private mount-table entry with a synthetic source name.
        # tmpfs never opens or formats the loop device itself.
        mount -t tmpfs -o size=1M "$a" "$sandbox/write-mounted"
        trap 'umount "$sandbox/write-mounted"' EXIT
        grep -Fq -- "$a $sandbox/write-mounted tmpfs " /proc/mounts || exit 2
        cli_capture 'w2usb: mounted target rejection' w2usb "$PWD/a.iso" "$a"
        validation_result; cli_has stderr "'$a' or its partitions are mounted"; cli_expect_rc 1 ;;
    busy)
        printf '0 1\n' >"$sys/testa/inflight"
        trap 'printf "0 0\\n" >"$sys/testa/inflight"' EXIT
        cli_capture 'w2usb: busy target rejection' w2usb --silent "$PWD/a.iso" "$a"
        validation_result; cli_has stderr "'$a' has pending kernel-level tasks"; cli_expect_rc 1 ;;
    invalid-image-silent)
        cli_capture "w2usb: invalid optical image, silent$suffix" w2usb "$@" --silent "$PWD/bad-write.iso" "$a"
        cli_quiet; check_bytes zero zero; cli_expect_rc 1 ;;
    invalid-image-verbose)
        capture_verbose "w2usb: invalid optical image, verbose$suffix" w2usb "$@" "$PWD/bad-write.iso" "$a"
        verbose_result FAILED 0/1; cli_record stdout 'bad-write.iso' "$a" FAIL
        check_bytes zero zero; cli_expect_rc 1 ;;
    single-silent)
        cli_capture "w2usb: single silent success$suffix" w2usb "$@" --silent "$PWD/a.iso" "$a"
        cli_quiet; check_bytes a zero; cli_expect_rc 0 ;;
    broadcast-silent)
        cli_capture "w2usb: broadcast silent success$suffix" w2usb "$@" --silent "$PWD/a.iso" "$a" "$b"
        cli_quiet; check_bytes a a; cli_expect_rc 0 ;;
    paired-silent)
        cli_capture "w2usb: paired silent success$suffix" w2usb "$@" --silent "$PWD/b.iso" "$PWD/a.iso" "$a" "$b"
        cli_quiet; check_bytes b a; cli_expect_rc 0 ;;
    partial-silent)
        cli_capture "w2usb: partial silent batch$suffix" w2usb "$@" --silent "$PWD/a.iso" "$PWD/bad-write.iso" "$a" "$b"
        cli_quiet; check_bytes a zero; cli_expect_rc 1 ;;
    partial-verbose)
        capture_verbose "w2usb: partial verbose batch$suffix" w2usb "$@" "$PWD/a.iso" "$PWD/bad-write.iso" "$a" "$b"
        verbose_result PARTIAL 1/2; cli_record stdout 'a.iso' "$a" "$success_status"; cli_record stdout 'bad-write.iso' "$b" FAIL
        check_bytes a zero; cli_expect_rc 1 ;;
    single-verbose)
        capture_verbose "w2usb: single verbose success$suffix" w2usb "$@" "$PWD/a.iso" "$a"
        verbose_result COMPLETED 1/1; cli_record stdout 'a.iso' "$a" "$success_status"
        check_bytes a zero; cli_expect_rc 0 ;;
    broadcast-verbose)
        capture_verbose "w2usb: broadcast verbose success$suffix" w2usb "$@" "$PWD/a.iso" "$a" "$b"
        verbose_result COMPLETED 2/2; cli_record stdout 'a.iso' "$a" "$success_status"; cli_record stdout 'a.iso' "$b" "$success_status"
        check_bytes a a; cli_expect_rc 0 ;;
    paired-verbose)
        capture_verbose "w2usb: paired verbose success$suffix" w2usb "$@" "$PWD/b.iso" "$PWD/a.iso" "$a" "$b"
        verbose_result COMPLETED 2/2; cli_record stdout 'b.iso' "$a" "$success_status"; cli_record stdout 'a.iso' "$b" "$success_status"
        check_bytes b a; cli_expect_rc 0 ;;
    padded-silent)
        seed_padded_target
        cli_capture "w2usb: padded multi-chunk silent success$suffix" w2usb "$@" --silent "$PWD/padded.iso" "$a"
        cli_quiet; check_bytes padded zero; cli_expect_rc 0 ;;
    padded-verbose)
        seed_padded_target
        capture_verbose "w2usb: padded multi-chunk verbose success$suffix" w2usb "$@" "$PWD/padded.iso" "$a"
        verbose_result COMPLETED 1/1; cli_record stdout 'padded.iso' "$a" "$success_status"
        check_bytes padded zero; cli_expect_rc 0 ;;
    *) exit 2 ;;
esac
# The verbose renderer must never report an unverified DONE when --verify
# was requested; ordinary writes must never report PASS. Match the status
# after the progress bar rather than a substring in a path or banner.
case "$scenario" in
    *-verbose)
        if [ "$success_status" = PASS ]; then unexpected=DONE; else unexpected=PASS; fi
        if grep -Eq "\] $unexpected( |$)" cli.stdout; then
            cli_fail "unexpected write status: $unexpected"
        fi ;;
esac
cli_pass
