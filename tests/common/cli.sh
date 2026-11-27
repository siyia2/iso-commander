# SPDX-License-Identifier: GPL-3.0-or-later

# Shared CLI capture, exit-status and output assertions.

# Source after setup_sandbox; caller sets app to an absolute executable path.
# Preserve raw streams for diagnostics and strip ANSI CSI and line-editor nonprinting delimiters for assertions.
cli_fail() {
    printf 'FAIL: %s: %s\n' "$cli_case" "$*" >&2
    printf '%s\n' '--- stdout ---' >&2; cat cli.stdout.raw >&2
    printf '\n%s\n' '--- stderr ---' >&2; cat cli.stderr.raw >&2
    exit 1
}
cli_run() {
    cli_case=$1; expected_rc=$2; shift 2
    cli_capture "$cli_case" "$@"
    cli_expect_rc "$expected_rc"
}
# Allow tests to verify side effects and output before checking the exit code.
cli_capture() {
    cli_case=$1; shift
    rc=0
    timeout -k 2s 30s "$app" "$@" </dev/null >cli.stdout.raw 2>cli.stderr.raw || rc=$?
    # CSI includes SGR, cursor movement, erase and private terminal modes.
    esc=$(printf '\033')
    for stream in stdout stderr; do
        LC_ALL=C sed "s/${esc}\[[0-?]*[ -/]*[@-~]//g" "cli.$stream.raw" | tr -d '\001\002' | tr '\r' '\n' >"cli.$stream"
    done
    # A sanitizer report must not accidentally satisfy an expected exit 1.
    if grep -Eq 'AddressSanitizer|LeakSanitizer|ThreadSanitizer|UndefinedBehaviorSanitizer|runtime error:' cli.stderr.raw; then
        cli_fail 'sanitizer diagnostic'
    fi
}
cli_expect_rc() {
    [ "$rc" -eq "$1" ] || cli_fail "exit $rc, expected $1"
}
cli_has() {
    grep -Fq -- "$2" "cli.$1" || cli_fail "$1 missing: $2"
}
cli_lacks() {
    if grep -Fq -- "$2" "cli.$1"; then cli_fail "$1 unexpectedly contains: $2"; fi
}
# Require all fragments on the same result line (order-independent across jobs).
cli_record() {
    stream=$1; first=$2; second=$3; third=$4
    grep -F -- "$first" "cli.$stream" | grep -F -- "$second" | grep -Fq -- "$third" ||
        cli_fail "$stream missing result line: $first / $second / $third"
}
cli_line() {
    grep -Fxq -- "$2" "cli.$1" || cli_fail "$1 missing complete line: $2"
}
cli_empty() {
    [ ! -s "cli.$1.raw" ] || cli_fail "expected empty $1"
}
cli_quiet() {
    cli_empty stdout; cli_empty stderr
}
cli_summary() {
    cli_line stdout "$1 summary:"
    cli_line stdout " Successful: $2"
    cli_line stdout " Failed:     $3"
    skipped=${4:-0}
    if [ "$1" != Unmount ]; then cli_line stdout " Skipped:    $skipped"; fi
    case "$1" in
        Conversion) operation=c2iso ;;
        Mount) operation=mount ;;
        Unmount) operation=umount ;;
        *) cli_fail "unknown summary: $1" ;;
    esac
    if [ "$3" -eq 0 ]; then
        if [ "$2" -eq 0 ] && [ "$skipped" -gt 0 ]; then status=SKIPPED
        else status=COMPLETED; fi
    elif [ "$2" -eq 0 ] && [ "$skipped" -eq 0 ]; then status=FAILED
    else status=PARTIAL; fi
    cli_has stdout "Status: $operation → $status"
    # Consecutive result groups have exactly one empty line between them.
    # Do not require specific result bodies or count deduplicated messages.
    awk -v op="$operation" '
        {
            kind = 0
            if (index($0, "✓ " op ": ") == 1) kind = 1
            else if (index($0, "– " op ": ") == 1) kind = 2
            else if (index($0, "✗ " op ": ") == 1) kind = 3
            if (kind) {
                if (previous && blanks != (kind != previous)) exit 1
                previous = kind
            }
            if ($0 == "") ++blanks
            else blanks = 0
        }
    ' cli.stdout || cli_fail 'incorrect verbose result group spacing'
    if grep -Fxq -- "$operation:" cli.stdout; then
        cli_fail "unexpected standalone operation heading: $operation:"
    fi
    cli_lacks stdout 'Display verbose output?'
    cli_lacks stdout 'Verbose? (y/n):'
}
cli_pass() { printf 'PASS: %s\n' "$cli_case"; }
