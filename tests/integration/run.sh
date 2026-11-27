#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
[ "$#" -eq 2 ] || { echo "Usage: $0 INTEGRATION_BINARY MENU_BINARY" >&2; exit 2; }
binary=$(realpath "$1")
menu=$(realpath "$2")
. "$(dirname "$0")/../common/sandbox.sh"
setup_sandbox
timeout -k 5s 180s "$binary"
TERM=xterm timeout -k 5s 35s "$binary" --terminal-test
TERM=xterm timeout -k 5s 45s "$binary" --editor-guards-test
TERM=xterm timeout -k 5s 40s "$binary" --menu-test "$menu"
TERM=xterm timeout -k 5s 95s "$binary" --workflow-test "$menu"
TERM=xterm timeout -k 5s 95s "$binary" --write-ui-test
TERM=xterm timeout -k 5s 95s "$binary" --progress-test

# Each workflow has its own HOME/database/config, even when run after another.
workflow_status=0
for workflow in settings theme import conversion; do
    if (
        setup_sandbox || exit 1
        TERM=xterm timeout -k 5s 190s "$binary" "--$workflow-workflow-test" "$menu"
    ); then
        :
    else
        workflow_status=1
    fi
done
exit "$workflow_status"
