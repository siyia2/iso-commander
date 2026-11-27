#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
# Compile the actual editor module independently of the application's std module.
# CXXFLAGS may be supplied for sanitizer builds. The declaration-only module
# is not instrumented (GCC 14 crashes when sanitizing its module interface);
# the editor implementation and PTY driver are both instrumented.
cd "$tmp"
${CXX:-g++} -std=c++23 -fmodules-ts -DISOCMD_EDITOR_STANDALONE -Wall -Wextra -Werror ${CXXFLAGS:-} \
    -fno-sanitize=all -x c++ -c "$root/modules/isocmd/input/lineEditor.cppm" -o editorModule.o
${CXX:-g++} -std=c++23 -fmodules-ts -DISOCMD_EDITOR_STANDALONE -Wall -Wextra -Werror ${CXXFLAGS:-} \
    "$root/src/input/editor/lineEditor.cpp" "$root/tests/nativeEditor/driver.cpp" \
    editorModule.o -lutil -o test-editor
mkdir "folder name"
ln -s "folder name" "linked folder"
touch regular.txt .hidden shared-a shared-b .shared-hidden
export HOME="$tmp"
LC_ALL=C TERM=xterm timeout -k 2s 25s ./test-editor
LC_ALL=C.UTF-8 TERM=xterm timeout -k 2s 25s ./test-editor
