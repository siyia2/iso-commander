# Native input backend

`lineEditor.cpp` implements `isocmd.input.lineEditor`, declared in
`modules/isocmd/input/lineEditor.cppm`. It requires only the C++ standard library and Linux /
POSIX terminal APIs. Production builds use `import std;`. The standalone test selects standard-library
headers with `ISOCMD_EDITOR_STANDALONE` to test the same module and source on GCC 14.

The line entry point is `std::optional<std::string> editLine(std::string_view)`.
An empty submitted line has a value; EOF returns `std::nullopt`. Application prompt controllers retain their current
bindings and guards; the editor owns the editable string, cursor and
history position for each prompt session. Global
hooks and keybindings remain UI-thread-only so existing restoration
contracts and pointer-identity tests remain applicable.

## Behavior

- POSIX terminal setup/restoration via RAII; the application retains its signal
  dispositions and cancellation policy. The editor does not install its own
  signal handlers.
- ASCII and UTF-8 code-point insertion, Backspace/Delete, Left/Right, Home/End,
  history navigation and Ctrl+U (delete from the start to the cursor).
  Ctrl+D returns EOF on empty input and deletes forward otherwise.
  Editing or clearing a recalled history entry and navigating away retains the
  change when the prompt is submitted; the existing history writer omits cleared
  entries. Submitting a recalled entry directly preserves its original value.
- Fixed 256-entry single-byte dispatch and a fixed nine-entry escape-sequence
  table; ordinary UTF-8 characters are inserted directly. CSI/SS3 aliases,
  context-specific hotkeys, bracketed paste (including literal line breaks),
  and a fixed 10 ms inter-byte timeout. Unsupported terminal sequences cannot
  be registered.
- Existing history persistence, prompt renderer (ANSI styling, horizontal
  scrolling and overflow markers), live filter, completion listing pagination,
  prefill and resize callbacks are reused. Standalone editor fallback rendering
  is intentionally simpler than the application's fixed-row renderer.
- Filesystem completion uses the semicolon-separated word convention, with
  directory slash insertion and common-prefix matching. Custom completion
  callbacks return owned vectors; the display contract retains the common prefix
  as the first entry for multiple candidates. Cursor-position reports
  defer ordinary input bytes rather than discarding them.
- Idle hooks and fallback terminal-size detection use `poll`; confirmation
  prompts can retain their custom self-pipe-based input hook.

## Scope and limitations

This targets ISO Commander's prompt behavior rather than general-purpose shell editing.
Emacs-style movement/history aliases, word movement/deletion, text selection,
Ctrl+K, undo, yank, character transposition, Vi editing mode, keyboard macros,
custom user keymaps and complete terminal-emulator compatibility are not provided.
Completion uses the application’s fixed semicolon separator; output goes to
stdout. UTF-8 movement is code-point-based rather than a full Unicode grapheme-cluster editor. The
existing process signal policy is unchanged; forced termination that bypasses
C++ unwinding is not covered by the terminal RAII guarantee. Application-specific
completion, filtering and resize behavior must be validated with the full
existing integration suite on the supported GCC toolchain.

## Build policy

The editor implementation, including the instrumented PTY variant, uses the
same optimization and LTO flags as the rest of the application in each build
configuration. Directory completion uses `opendir`, `readdir` and `stat`
with RAII cleanup rather than `std::filesystem` path iteration. This retains
POSIX filename bytes and directory/symlink completion without adding dependencies.

## Validation

Run `sh tests/nativeEditor/run.sh` for independent PTY tests, then
`make test-unit` and `make test-integration` on GCC 15 or newer. The standalone
suite also runs as part of the integration target. For sanitizers, use the
existing `SANITIZE` Makefile setting or pass compiler flags via `CXXFLAGS` to
the standalone script.
