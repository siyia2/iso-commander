# SPDX-License-Identifier: GPL-3.0-or-later

# Creates and cleans a private test working directory and environment.

# Source after resolving executable paths; this changes cwd and HOME.
setup_sandbox() {
    sandbox=$(mktemp -d "${TMPDIR:-/tmp}/isocmd-tests.XXXXXX")
    sandbox=$(realpath "$sandbox")
    trap 'rm -rf -- "$sandbox"' EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    export ISOCMD_TEST_SANDBOX="$sandbox" HOME="$sandbox/home" XDG_CONFIG_HOME="$sandbox/config" XDG_DATA_HOME="$sandbox/data" XDG_CACHE_HOME="$sandbox/cache" TMPDIR="$sandbox/tmp" LC_ALL=C TERM=dumb INPUTRC=/dev/null
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$TMPDIR"
    cd "$sandbox"
}
