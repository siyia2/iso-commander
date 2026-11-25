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
TERM=xterm timeout -k 5s 40s "$binary" --menu-test "$menu"
TERM=xterm timeout -k 5s 95s "$binary" --workflow-test "$menu"
TERM=xterm timeout -k 5s 95s "$binary" --write-ui-test
TERM=xterm timeout -k 5s 95s "$binary" --progress-test
