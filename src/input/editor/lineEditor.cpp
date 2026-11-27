// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file lineEditor.cpp
 * @brief Native POSIX line editing, key dispatch, history and completion.
 *
 * All editor state belongs to the UI thread. Signal handling remains the
 * responsibility of the application's resize and cancellation guards.
 * Buffer positions are byte offsets, with UTF-8 code-point navigation.
 */
// Native POSIX line editor. All state is owned by the UI thread; no signal
// handlers are installed here. Existing resize/cancellation guards own them.
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <langinfo.h>
#include <locale.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ISOCMD_TEST_TERMINAL_SYNC
#include "../../../tests/common/terminalSync.h"
#endif
#ifdef ISOCMD_EDITOR_STANDALONE
#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#else
import std;
#endif
import isocmd.input.lineEditor;

const char* editorLineBuffer = nullptr;
const char* editorPrompt = nullptr;
const char* editorDisplayPrompt = nullptr;
int editorCursor = 0, editorBufferEnd = 0, editorSelectionMark = 0, editorDone = 0;
int editorCatchSigwinch = 1;
int editorCompletionAppendCharacter = ' ', editorCompletionHandled = 0;
const char* editorWordBreakCharacters = ";";
FILE* editorOutputStream = stdout;
EditorHook *editorEventHook = nullptr, *editorStartupHook = nullptr;
EditorRedisplay* editorResizeHook = nullptr;
int editorResizeWakeFd = -1;
EditorRedisplay* editorRedisplayHook = editorRedisplay;
EditorCompletion* editorCompletionHook = nullptr;
EditorCompletionDisplay* editorCompletionDisplayHook = nullptr;
EditorInputReader* editorInputReader = editorReadByte;
EditorCommand* editorLastCommand = nullptr;

namespace native_editor {

	using namespace std::chrono;
	constexpr auto eventInterval = milliseconds(100);
	constexpr auto resizeFallbackInterval = milliseconds(16);
	std::vector<std::string> history;
	std::array<EditorCommand*, 256> byteKeys{};
	// Only multibyte terminal sequences need a lookup table.
	std::vector<std::pair<std::string, EditorCommand*>> sequenceKeys;
	std::string promptStorage;
	int escapeTimeout = 10;
	bool pasteEnabled = true;

	/// State owned by one active input session.
	struct Session {
		std::string text, draft;
		winsize size{};
		std::size_t historyPos = history.size();
		bool markActive = false, eof = false, firstDraw = true;
		int draftPoint = 0;
		steady_clock::time_point nextHook = steady_clock::now();
		steady_clock::time_point nextResize = nextHook;
	};
	Session* current = nullptr;
	// UI-thread-only typeahead, retained across prompt boundaries.
	std::deque<unsigned char> queued;

	/**
	 * @brief Returns the configured editor output stream.
	 *
	 * @return Configured stream, or @c stdout when none is set.
	 */
	FILE* output() {

		return editorOutputStream ? editorOutputStream : stdout;

	}

	/**
	 * @brief Synchronizes the exposed buffer pointer and byte offsets with the active session.
	 *
	 * Clamps the cursor and selection mark to the current buffer length. Does nothing without an
	 * active session.
	 */
	void syncBuffer() {

		if (!current) return;
		editorLineBuffer = current->text.c_str();
		editorBufferEnd = static_cast<int>(current->text.size());
		editorCursor = std::clamp(editorCursor, 0, editorBufferEnd);
		editorSelectionMark = std::clamp(editorSelectionMark, 0, editorBufferEnd);

	}

	/**
	 * @brief Replaces a byte range in the active buffer.
	 *
	 * Clamps the range, rejects results exceeding @c INT_MAX bytes, moves the cursor after the
	 * replacement and clears the active selection.
	 *
	 * @param start Inclusive starting byte offset.
	 * @param end Exclusive ending byte offset.
	 * @param value Replacement text; may alias the active buffer.
	 */
	void replace(int start, int end, std::string_view value) {

		if (!current) return;
		start = std::clamp(start, 0, editorBufferEnd);
		end = std::clamp(end, start, editorBufferEnd);
		if (value.size() > static_cast<std::size_t>(INT_MAX - (editorBufferEnd - (end - start)))) return;
		// Copy first: callers may pass a substring of the active buffer.
		std::string copy(value);
		current->text.replace(start, end - start, copy);
		editorCursor = start + static_cast<int>(copy.size());
		current->markActive = false;
		syncBuffer();

	}

	/**
	 * @brief Finds the preceding UTF-8 code-point boundary.
	 *
	 * @param p Current byte offset.
	 * @return Previous byte offset, or zero at the start.
	 */
	int previous(int p) {

		if (!current || p <= 0) return 0;
		--p;
		while (p > 0 && (static_cast<unsigned char>(current->text[p]) & 0xc0) == 0x80) --p;
		return p;

	}

	/**
	 * @brief Finds the following UTF-8 code-point boundary.
	 *
	 * @param p Current byte offset.
	 * @return Next byte offset, or the buffer end.
	 */
	int next(int p) {

		if (!current || p >= editorBufferEnd) return editorBufferEnd;
		++p;
		while (p < editorBufferEnd && (static_cast<unsigned char>(current->text[p]) & 0xc0) == 0x80) ++p;
		return p;

	}

	/**
	 * @brief Checks whether a character is alphanumeric in the current locale.
	 *
	 * @param p Byte offset of a character in the active buffer.
	 * @return True for an alphanumeric character; false at the buffer end or on a decoding error.
	 */
	bool word(int p) {

		if (p >= editorBufferEnd) return false;
		std::mbstate_t state{};
		wchar_t wc{};
		const auto n = std::mbrtowc(&wc, current->text.data() + p, editorBufferEnd - p, &state);
		return n != static_cast<std::size_t>(-1) && n != static_cast<std::size_t>(-2) && std::iswalnum(wc);

	}

	/**
	 * @brief Finds the beginning of the preceding or current word.
	 *
	 * @param p Starting byte offset.
	 * @return Byte offset after moving left across separators and alphanumeric characters.
	 */
	int wordLeft(int p) {

		while (p && !word(previous(p))) p = previous(p);
		while (p && word(previous(p))) p = previous(p);
		return p;

	}

	/**
	 * @brief Finds the end of the next or current word.
	 *
	 * @param p Starting byte offset.
	 * @return Byte offset after moving right across separators and alphanumeric characters.
	 */
	int wordRight(int p) {

		while (p < editorBufferEnd && !word(p)) p = next(p);
		while (p < editorBufferEnd && word(p)) p = next(p);
		return p;

	}

	/**
	 * @brief Moves the cursor and updates selection state.
	 *
	 * @param p Destination byte offset.
	 * @param select Whether to extend or begin a selection.
	 */
	void move(int p, bool select = false) {

		if (!current) return;
		if (select && !current->markActive) editorSelectionMark = editorCursor;
		current->markActive = select;
		editorCursor = p;

	}

	/**
	 * @brief Inserts text at the cursor or replaces the active selection.
	 *
	 * @param value Text to insert.
	 */
	void insert(std::string_view value) {

		if (!current || value.empty()) return;
		const auto [lo, hi] = current->markActive ? std::minmax(editorCursor, editorSelectionMark) : std::minmax(editorCursor, editorCursor);
		replace(lo, hi, value);

	}

	/**
	 * @brief Erases a byte range or the active selection.
	 *
	 * An active selection overrides the supplied range.
	 *
	 * @param lo Inclusive starting byte offset.
	 * @param hi Exclusive ending byte offset.
	 */
	void erase(int lo, int hi) {

		if (!current) return;
		if (current->markActive) {
			lo = std::min(editorCursor, editorSelectionMark);
			hi = std::max(editorCursor, editorSelectionMark);
		}
		if (hi <= lo) return;
		replace(lo, hi, {});

	}

	/// Moves the cursor one UTF-8 code point left.
	int left(int, int) {

		move(previous(editorCursor));
		return 0;

	}

	/// Moves the cursor one UTF-8 code point right.
	int right(int, int) {

		move(next(editorCursor));
		return 0;

	}

	/// Moves the cursor to the beginning of the buffer.
	int home(int, int) {

		move(0);
		return 0;

	}

	/// Moves the cursor to the end of the buffer.
	int end(int, int) {

		move(editorBufferEnd);
		return 0;

	}

	/// Deletes the preceding code point or active selection.
	int backspace(int, int) {

		erase(previous(editorCursor), editorCursor);
		return 0;

	}

	/// Deletes the following code point or active selection.
	int del(int, int) {

		erase(editorCursor, next(editorCursor));
		return 0;

	}

	/// Signals EOF for an empty buffer; otherwise deletes forward.
	int eofOrDelete(int, int) {

		if (!editorBufferEnd) {
			current->eof = true;
			editorDone = 1;
		} else del(1, 0);
		return 0;

	}

	/// Deletes text from the buffer start to the cursor.
	int killStart(int, int) {

		erase(0, editorCursor);
		return 0;

	}

	/// Deletes text from the cursor to the buffer end.
	int killEnd(int, int) {

		erase(editorCursor, editorBufferEnd);
		return 0;

	}

	/// Deletes the preceding alphanumeric word.
	int killWord(int, int) {

		erase(wordLeft(editorCursor), editorCursor);
		return 0;

	}

	/// Consumes a key without modifying editor state.
	int noAction(int, int) {

		return 0;

	}

	/// Clears the terminal screen and homes the cursor.
	int clear(int, int) {

		std::fputs("\033[H\033[2J", output());
		return 0;

	}

	/**
	 * @brief Normalizes supported alternate terminal key sequences.
	 *
	 * @param key Original key sequence.
	 * @return Canonical sequence, or the original view if no alias matches.
	 */
	std::string_view canonical(std::string_view key) {

		if (key == "\033OA") return "\033[A";
		if (key == "\033OB") return "\033[B";
		if (key == "\033OC") return "\033[C";
		if (key == "\033OD") return "\033[D";
		if (key == "\033OH" || key == "\033[1~" || key == "\033[7~") return "\033[H";
		if (key == "\033OF" || key == "\033[4~" || key == "\033[8~") return "\033[F";
		return key;

	}

	/**
	 * @brief Lazily installs default byte and escape-sequence bindings once.
	 *
	 * Subsequent calls preserve application-specific keybindings.
	 */
	void initialize() {

		static const bool initialized = [] {
			auto& k = byteKeys;
			auto seq = [](std::string_view key) -> EditorCommand*& {
				for (auto& [name, handler] : sequenceKeys) if (name == key) return handler;
				return sequenceKeys.emplace_back(std::string(key), nullptr).second;
			};
			sequenceKeys.reserve(24);
			for (int i = 32; i <= 255; ++i) if (i != 127) k[i] = editorInsertKey;
			k[static_cast<unsigned char>('\r')] = k[static_cast<unsigned char>('\n')] = editorSubmit;
			k[static_cast<unsigned char>('\t')] = editorComplete;
			k[static_cast<unsigned char>('\f')] = clear;
			k[static_cast<unsigned char>('\001')] = seq("\033[H") = home;
			k[static_cast<unsigned char>('\005')] = seq("\033[F") = end;
			k[static_cast<unsigned char>('\002')] = seq("\033[D") = left;
			k[static_cast<unsigned char>('\006')] = seq("\033[C") = right;
			k[static_cast<unsigned char>('\010')] = k[static_cast<unsigned char>('\177')] = backspace;
			seq("\033[3~") = del;
			k[static_cast<unsigned char>('\004')] = eofOrDelete;
			k[static_cast<unsigned char>('\025')] = killStart;
			k[static_cast<unsigned char>('\013')] = killEnd;
			k[0] = [](int, int) {
				editorSelectionMark = editorCursor;
				current->markActive = true;
				return 0;
			};
			k[static_cast<unsigned char>('\007')] = [](int, int) {
				current->markActive = false;
				return 0;
			};
			k[static_cast<unsigned char>('\020')] = seq("\033[A") = seq("\033[5~") = editorPreviousHistory;
			k[static_cast<unsigned char>('\016')] = seq("\033[B") = seq("\033[6~") = editorNextHistory;
			k[static_cast<unsigned char>('\033')] = noAction;
			seq("\033b") = seq("\033[1;5D") = [](int, int) {
				move(wordLeft(editorCursor));
				return 0;
			};
			seq("\033f") = seq("\033[1;5C") = [](int, int) {
				move(wordRight(editorCursor));
				return 0;
			};
			seq("\033d") = [](int, int) {
				erase(editorCursor, wordRight(editorCursor));
				return 0;
			};
			seq("\033\177") = killWord;
			seq("\033[1;2D") = [](int, int) {
				move(previous(editorCursor), true);
				return 0;
			};
			seq("\033[1;2C") = [](int, int) {
				move(next(editorCursor), true);
				return 0;
			};
			seq("\033[1;2H") = [](int, int) {
				move(0, true);
				return 0;
			};
			seq("\033[1;2F") = [](int, int) {
				move(editorBufferEnd, true);
				return 0;
			};
			seq("\033[1;6D") = [](int, int) {
				move(wordLeft(editorCursor), true);
				return 0;
			};
			seq("\033[1;6C") = [](int, int) {
				move(wordRight(editorCursor), true);
				return 0;
			};
			return true;
		}();
		(void)initialized;

	}

	/// RAII guard for terminal input mode and bracketed paste.
	struct TerminalGuard {
		termios saved{};
		bool raw = false, bracketed = false;

		/**
		 * @brief Saves terminal attributes and enters noncanonical, no-echo input mode.
		 *
		 * Preserves signal generation and enables bracketed paste when requested and the output is
		 * a terminal.
		 */
		TerminalGuard() {

			if (tcgetattr(STDIN_FILENO, &saved) != 0) return;
			auto mode = saved;
			mode.c_lflag &= ~(ICANON | ECHO | ECHONL | IEXTEN);
			mode.c_iflag &= ~(IXON | IXOFF | ISTRIP | INLCR | IGNCR | ICRNL);
			mode.c_cc[VMIN] = 1;
			mode.c_cc[VTIME] = 0;
			int result;
			do {
				result = tcsetattr(STDIN_FILENO, TCSANOW, &mode);
			} while (result < 0 && errno == EINTR);
			raw = result == 0;
			bracketed = raw && pasteEnabled && isatty(fileno(output()));
			if (bracketed) {
				std::fputs("\033[?2004h", output());
				std::fflush(output());
			}

		}

		/**
		 * @brief Disables bracketed paste and restores saved terminal attributes.
		 *
		 * Retries interrupted terminal restoration calls.
		 */
		~TerminalGuard() {

			if (bracketed) {
				std::fputs("\033[?2004l", output());
				std::fflush(output());
			}
			if (raw) {
				while (tcsetattr(STDIN_FILENO, TCSANOW, &saved) < 0 && errno == EINTR) {}
			}

		}
	};

	/// Invokes the configured redisplay hook, if present.
	void redraw() {

		if (editorRedisplayHook) editorRedisplayHook();

	}

	/**
	 * @brief Checks the terminal size when no dedicated resize watcher owns repainting.
	 */
	void checkTerminalSize() {

		if (!editorCatchSigwinch || (editorInputReader && editorInputReader != editorReadByte)) return;
		winsize size{};
		if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row && size.ws_col &&
		    (current->size.ws_row != size.ws_row || current->size.ws_col != size.ws_col)) {
			current->size = size;
			redraw();
		}

	}

	/**
	 * @brief Services resize and background events on independent deadlines.
	 *
	 * The resize hook normally runs on notification from the wake pipe.
	 * A timer is retained only when a wake descriptor is unavailable.
	 * Ordinary prompts retain the slower fallback size check.
	 */
	void idle() {

		const auto now = steady_clock::now();
		if (editorResizeHook && editorResizeWakeFd < 0 && now >= current->nextResize) {
			current->nextResize = now + resizeFallbackInterval;
			editorResizeHook();
		}
		if (!editorDone && now >= current->nextHook) {
			current->nextHook = now + eventInterval;
			if (editorEventHook) editorEventHook();
			if (!editorDone && !editorResizeHook) checkTerminalSize();
		}

	}

	constexpr int timeout = -2;

	/**
	 * @brief Computes the remaining wait until an absolute deadline.
	 * @param deadline Absolute deadline for the next event or byte.
	 * @return Remaining milliseconds, rounded up to avoid premature wakeups.
	 */
	int remainingWait(steady_clock::time_point deadline) {

		return static_cast<int>(std::max(milliseconds::zero(),
		    ceil<milliseconds>(deadline - steady_clock::now())).count());

	}

	/**
	 * @brief Computes the polling interval for a timed or idle-aware read.
	 * @param wait Requested timeout in milliseconds, negative for an indefinite read.
	 * @param deadline Absolute deadline for a timed read.
	 * @return Remaining milliseconds, or -1 for an unrestricted poll.
	 */
	int pollInterval(int wait, steady_clock::time_point deadline) {

		if (wait >= 0) {
			return static_cast<int>(std::max(0LL, static_cast<long long>(
			    duration_cast<milliseconds>(deadline - steady_clock::now()).count())));
		}
		const bool periodic = editorEventHook || (!editorResizeHook && editorCatchSigwinch);
		if (!editorResizeHook || editorResizeWakeFd >= 0)
			return periodic ? remainingWait(current->nextHook) : -1;
		return remainingWait(periodic ? std::min(current->nextResize, current->nextHook) : current->nextResize);

	}

	/// Returns the configured input reader, falling back to the native byte reader.
	EditorInputReader* inputReader() {

		return editorInputReader ? editorInputReader : editorReadByte;

	}

	/**
	 * @brief Consumes a batch of resize notifications and invokes the resize hook.
	 * @param fd Borrowed nonblocking descriptor reported readable by poll().
	 */
	void serviceResize(int fd) {

		char notifications[256];
		ssize_t n;
		do {
			n = ::read(fd, notifications, sizeof notifications);
		} while (n < 0 && errno == EINTR);
		if (n > 0 && editorResizeHook) editorResizeHook();
		else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) editorResizeWakeFd = -1;

	}

	/**
	 * @brief Waits for keyboard input or a resize notification.
	 *
	 * Timed reads finish decoding a key before repainting. When both sources
	 * are ready, handle the resize first, then return keyboard readiness.
	 * @param wait Requested byte timeout, negative for an ordinary wait.
	 * @param deadline Absolute deadline for timed reads.
	 * @return Positive for stdin readiness, zero for other events, or -1 on error.
	 */
	int pollInput(int wait, steady_clock::time_point deadline) {

		pollfd fds[] = {
			{STDIN_FILENO, POLLIN, 0},
			{wait < 0 && editorResizeHook ? editorResizeWakeFd : -1, POLLIN, 0}
		};
		const int ready = poll(fds, 2, pollInterval(wait, deadline));
		if (ready <= 0) return ready;
		if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) editorResizeWakeFd = -1;
		if (fds[1].revents & POLLIN) {
			serviceResize(fds[1].fd);
			// A repaint can collect typeahead through a cursor-position query.
			// Recheck stdin rather than trusting readiness from before the hook.
			if (!queued.empty() || editorDone) return 0;
			fds[0].revents = 0;
			return poll(fds, 1, 0);
		}
		return fds[0].revents != 0 ? 1 : 0;

	}

	/**
	 * @brief Reads a byte from typeahead or the configured input reader.
	 *
	 * @param wait Maximum wait in milliseconds; negative for an indefinite wait with idle processing.
	 * @return Unsigned byte value, @c EOF on input failure or termination, or @c timeout when the deadline expires.
	 */
	int inputByte(int wait = -1) {

		const auto deadline = steady_clock::now() + milliseconds(std::max(wait, 0));
		for (;;) {
			if (wait < 0) idle();
			if (editorDone) return EOF;
			const int pending = editorTakePendingInput();
			if (pending >= 0) return pending;
			if (wait < 0 && inputReader() != editorReadByte)
			    return inputReader()(stdin);
			const int ready = pollInput(wait, deadline);
			if (editorDone) return EOF;
			if (ready > 0) return inputReader()(stdin);
			if (ready == 0 && wait >= 0) return timeout;
			if (ready < 0 && errno != EINTR) return EOF;
			if (wait >= 0 && steady_clock::now() >= deadline) return timeout;
		}

	}

	/**
	 * @brief Collects an escape-prefixed terminal key sequence.
	 *
	 * Recognizes CSI and SS3 sequences and retains a second Escape as typeahead.
	 * @return Collected bytes, including the initial Escape.
	 */
	std::string readEscapeKey() {

		std::string key(1, '\033');
		int c = inputByte(escapeTimeout);
		if (c < 0) return key;
		if (c == 27) {
			queued.push_front(27);
			return key;
		}
		key += static_cast<char>(c);
		if (c != '[' && c != 'O') return key;
		// CSI/SS3: parameter and intermediate bytes followed by a final byte.
		for (int i = 0; i < 64; ++i) {
			c = inputByte(escapeTimeout);
			if (c < 0) break;
			key += static_cast<char>(c);
			if (c >= 0x40 && c <= 0x7e) break;
		}
		return key;

	}

	/**
	 * @brief Appends the continuation bytes of a UTF-8 sequence.
	 * @param key Initial byte, followed by any collected continuation bytes.
	 * @param first UTF-8 leading byte determining the expected sequence length.
	 */
	void readUtf8Tail(std::string& key, int first) {

		const int length = first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
		for (int i = 1; i < length; ++i) {
			const int c = inputByte(escapeTimeout);
			if (c < 0) break;
			if ((c & 0xc0) != 0x80) {
				queued.push_front(static_cast<unsigned char>(c));
				break;
			}
			key += static_cast<char>(c);
		}

	}

	/**
	 * @brief Collects a terminal key sequence or UTF-8 byte sequence.
	 * @param first First byte already read from input.
	 * @return Collected bytes; may be incomplete on timeout.
	 */
	std::string readKey(int first) {

		if (first == 27) return readEscapeKey();
		std::string key(1, static_cast<char>(first));
		if (first >= 0xc2 && first <= 0xf4) readUtf8Tail(key, first);
		return key;

	}

	/**
	 * @brief Reads and inserts a bracketed-paste payload.
	 *
	 * Consumes the closing paste marker, normalizes CR and CRLF to LF, and inserts the payload
	 * without submitting the prompt.
	 */
	void bracketedPaste() {

		constexpr std::string_view terminator = "\033[201~";
		std::string payload, pending;
		while (!editorDone) {
			const int c = inputByte();
			if (c < 0) {
				if (!editorDone) {
					current->eof = true;
					editorDone = 1;
				}
				return;
			}
			pending += static_cast<char>(c);
			while (!terminator.starts_with(pending)) {
				if (pending.front()) payload += pending.front();
				pending.erase(0, 1);
			}
			if (pending == terminator) break;
		}
		// Pasted line endings stay in the buffer, never submit commands.
		std::string normalized;
		normalized.reserve(payload.size());
		for (std::size_t i = 0; i < payload.size(); ++i) {
			if (payload[i] == '\r') {
				normalized += '\n';
				if (i + 1 < payload.size() && payload[i + 1] == '\n') ++i;
			} else if (payload[i]) normalized += payload[i];
		}
		insert(normalized);
		editorLastCommand = nullptr;

	}

	/**
	 * @brief Dispatches a decoded key to its bound command or text insertion.
	 *
	 * Uses indexed lookup for single bytes, direct insertion for multibyte text, and canonical
	 * lookup for escape sequences.
	 *
	 * @param key Collected key or UTF-8 byte sequence.
	 */
	void dispatch(const std::string& key) {

		if (key == "\033[200~" && pasteEnabled) {
			bracketedPaste();
			return;
		}
		if (key.size() == 1) {
			const auto byte = static_cast<unsigned char>(key.front());
			const auto fn = byteKeys[byte];
			if (fn == editorInsertKey) {
				if (byte != 0) insert(key);
			} else if (fn) fn(1, byte);
			editorLastCommand = fn;
		} else if (static_cast<unsigned char>(key.front()) >= 0x80) {
			insert(key);
			editorLastCommand = editorInsertKey;
		} else {
			const auto sequence = canonical(key);
			const auto fn = editorKeySequenceHandler(sequence);
			if (fn) fn(1, static_cast<unsigned char>(sequence.back()));
			editorLastCommand = fn;
		}

	}

	/**
	 * @brief Finds the common prefix of a nonempty candidate vector.
	 *
	 * The input vector must contain at least one candidate.
	 *
	 * @param values Nonempty vector of candidate strings.
	 * @return Common prefix without a trailing partial UTF-8 code point.
	 */
	std::string commonPrefix(const std::vector<std::string>& values) {

		std::string prefix = values.front();
		for (const auto& value : values) {
			std::size_t i = 0;
			while (i < prefix.size() && i < value.size() && prefix[i] == value[i]) ++i;
			// Never insert a partial UTF-8 character.
			while (i && i < prefix.size() && (static_cast<unsigned char>(prefix[i]) & 0xc0) == 0x80) --i;
			prefix.resize(i);
		}
		return prefix;

	}

	/**
	 * @brief Expands a leading ~/ using HOME for filesystem lookup.
	 * @param path Path to expand, left unchanged when HOME is unavailable.
	 */
	void expandHome(std::string& path) {

		if (!path.starts_with("~/")) return;
		if (const char* home = std::getenv("HOME")) path.replace(0, 1, home);

	}

	/**
	 * @brief Checks whether a directory entry is eligible for completion.
	 * @param name Directory entry name.
	 * @param partial Typed filename prefix.
	 * @return True for matching names, including hidden files and the parent directory only when requested.
	 */
	bool matchesFilename(std::string_view name, std::string_view partial) {

		if (name == "..") return partial == "..";
		return name != "." && name.starts_with(partial) &&
		    (!name.starts_with('.') || partial.starts_with('.'));

	}

	/**
	 * @brief Appends a slash to a sole directory match, including directory symlinks.
	 * @param values Filesystem completion candidates.
	 */
	void markDirectoryMatch(std::vector<std::string>& values) {

		if (values.size() != 1) return;
		std::string actual = values.front();
		expandHome(actual);
		struct stat info{};
		if (stat(actual.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
			values.front() += '/';
			editorCompletionAppendCharacter = 0;
		}

	}

	/**
	 * @brief Collects matching filesystem names using POSIX directory traversal.
	 *
	 * Expands ~/ for lookup, preserves the typed path prefix and filters hidden entries unless
	 * explicitly requested. A sole directory match receives a trailing slash and suppresses the
	 * append character.
	 *
	 * @param text Partial path to complete.
	 * @return Sorted, deduplicated completion vector, with a common prefix when multiple matches exist.
	 */
	std::vector<std::string> fileMatches(const std::string& text) {

		const auto slash = text.rfind('/');
		const auto base = slash == std::string::npos ? std::string{} : text.substr(0, slash + 1);
		const auto partial = text.substr(base.size());
		std::string directory = base.empty() ? "." : base;
		expandHome(directory);
		std::vector<std::string> values;
		// POSIX already provides the directory traversal we need. Avoid constructing
		// filesystem paths and error-code objects for each completion candidate.
		struct Directory {
			DIR* handle;

			/// Closes the directory handle if it was opened successfully.
			~Directory() {

				if (handle) closedir(handle);

			}
		} dir{opendir(directory.c_str())};
		if (!dir.handle) return values;
		while (const auto* entry = readdir(dir.handle)) {
			const std::string_view name(entry->d_name);
			if (!matchesFilename(name, partial)) continue;
			values.emplace_back(base);
			values.back().append(name);
		}
		markDirectoryMatch(values);
		return editorCompletionMatches(std::move(values));

	}

	/**
	 * @brief Prints a prompt without its nonprinting marker bytes.
	 * @param out Destination stream.
	 * @param prompt Prompt text or final prompt line.
	 */
	void printPrompt(FILE* out, std::string_view prompt) {

		for (char c : prompt) if (c != '\001' && c != '\002') std::fputc(c, out);

	}

	/**
	 * @brief Renders the fallback input buffer and restores the cursor position.
	 * @param out Destination terminal stream.
	 */
	void printInputBuffer(FILE* out) {

		for (unsigned char c : current->text) {
			if (c < 32 || c == 127) {
				std::fputc('^', out);
				std::fputc(c ^ 64, out);
			} else std::fputc(c, out);
		}
		std::fputs("\033[K", out);
		// Fallback only; production interactive prompts use FixedPrompt.
		if (editorCursor < editorBufferEnd) std::fprintf(out, "\033[%dD", editorBufferEnd - editorCursor);

	}

	/**
	 * @brief Inserts a unique completion and its configured append character.
	 * @param replacement Unique matching candidate.
	 * @param text Original token text.
	 * @param start Token start byte offset.
	 * @param finish Token end byte offset.
	 */
	void insertUniqueMatch(std::string replacement, std::string_view text, int start, int finish) {

		if (editorCompletionAppendCharacter &&
		    (finish == editorBufferEnd || current->text[finish] != editorCompletionAppendCharacter))
		    replacement += static_cast<char>(editorCompletionAppendCharacter);
		if (replacement != text) replace(start, finish, replacement);

	}

	/**
	 * @brief Applies the common prefix and optionally displays multiple matches.
	 * @param matches Common prefix followed by individual candidates.
	 * @param text Original token text.
	 * @param start Token start byte offset.
	 * @param finish Token end byte offset.
	 * @param mode Completion mode, as accepted by editorCompleteInternal().
	 */
	void completeMultiple(const std::vector<std::string>& matches, std::string_view text,
	                      int start, int finish, int mode) {

		const int count = static_cast<int>(matches.size()) - 1;
		int maxLength = 0;
		for (std::size_t i = 1; i < matches.size(); ++i)
		    maxLength = std::max(maxLength, static_cast<int>(matches[i].size()));
		if (mode != '?' && matches.front().size() > text.size())
		    replace(start, finish, matches.front());
		if ((mode == '?' || mode == '!') && editorCompletionDisplayHook)
		    editorCompletionDisplayHook(matches, count, maxLength);

	}

	/**
	 * @brief Parses a decimal cursor coordinate without integer overflow.
	 * @param value Coordinate bytes.
	 * @return Positive coordinate, or zero for invalid, zero or overflowing values.
	 */
	int parseCoordinate(std::string_view value) {

		int n = 0;
		for (char digit : value) {
			if (digit < '0' || digit > '9' || n > (INT_MAX - (digit - '0')) / 10) return 0;
			n = n * 10 + digit - '0';
		}
		return n;

	}

	/**
	 * @brief Extracts a trailing cursor-position report, preserving preceding input.
	 * @param bytes Collected terminal input, ending in R. The valid report is removed.
	 * @return Reported row, or zero if the suffix is not a valid report.
	 */
	int extractCursorRow(std::string& bytes) {

		const auto start = bytes.rfind("\033[");
		if (start == std::string::npos) return 0;
		const auto response = std::string_view(bytes).substr(start + 2, bytes.size() - start - 3);
		const auto semicolon = response.find(';');
		if (semicolon == std::string_view::npos || semicolon == 0 || semicolon + 1 == response.size()) return 0;
		const int row = parseCoordinate(response.substr(0, semicolon));
		if (!row || !parseCoordinate(response.substr(semicolon + 1))) return 0;
		bytes.resize(start);
		return row;

	}
} // namespace native_editor

using namespace native_editor;

/// Clears stored history and resets the active session's history navigation state.
void editorClearHistory() {

	history.clear();
	if (current) {
		current->historyPos = 0;
		current->draft.clear();
		current->draftPoint = 0;
	}

}

/**
 * @brief Appends a nonempty entry to the in-memory history.
 *
 * @param s Entry to store.
 */
void editorAddHistory(std::string_view s) {

	if (!s.empty()) history.emplace_back(s);

}

/**
 * @brief Exposes the in-memory history for reading.
 *
 * @return Const reference to the stored entries in insertion order.
 */
const std::vector<std::string>& editorHistoryList() {

	return history;

}

/**
 * @brief Initializes character classification and decoding for UTF-8 input.
 *
 * Honors the environment locale and attempts UTF-8 fallbacks when necessary. Call at startup
 * before creating worker threads, since setlocale changes process-wide state.
 */
void editorInitializeLocale() {

	// The UI emits UTF-8 even when started under the C/POSIX locale.
	// Set only LC_CTYPE, once at process startup, before starting workers.
	std::setlocale(LC_CTYPE, "");
	const std::string_view codeset(nl_langinfo(CODESET));
	if (codeset != "UTF-8" && codeset != "UTF8") {
		if (!std::setlocale(LC_CTYPE, "C.UTF-8"))
		std::setlocale(LC_CTYPE, "en_US.UTF-8");
	}

}

/**
 * @brief Sets or clears a single-byte keybinding.
 *
 * @param key Byte value in the range 0–255.
 * @param fn Command handler, or nullptr to unbind.
 * @return Zero on success, or -1 for an invalid byte value.
 */
int editorBindKey(int key, EditorCommand* fn) {

	initialize();
	if (key < 0 || key > 255) return - 1;
	byteKeys[static_cast<unsigned char>(key)] = fn;
	return 0;

}

/**
 * @brief Sets or clears a key-sequence binding.
 *
 * Single-byte keys use the fixed array; recognized aliases share a canonical binding.
 *
 * @param key Nonempty byte or escape sequence.
 * @param fn Command handler, or nullptr to disable the binding.
 * @return Zero on success, or -1 for an empty sequence.
 */
int editorBindKeySequence(std::string_view key, EditorCommand* fn) {

	initialize();
	if (key.empty()) return - 1;
	if (key.size() == 1) return editorBindKey(static_cast<unsigned char>(key.front()), fn);
	const auto name = canonical(key);
	for (auto& [sequence, handler] : sequenceKeys) {
		if (sequence == name) {
			handler = fn;
			return 0;
		}
	}
	sequenceKeys.emplace_back(std::string(name), fn);
	return 0;

}

/**
 * @brief Looks up the handler for a byte or terminal key sequence.
 *
 * @param key Key sequence to look up.
 * @return Bound command, or nullptr if no active binding exists.
 */
EditorCommand* editorKeySequenceHandler(std::string_view key) {

	initialize();
	if (key.empty()) return nullptr;
	if (key.size() == 1) return byteKeys[static_cast<unsigned char>(key.front())];
	const auto name = canonical(key);
	for (const auto& [sequence, handler] : sequenceKeys) if (sequence == name) return handler;
	return nullptr;

}

/**
 * @brief Sets the timeout used when collecting multibyte input.
 *
 * @param milliseconds Inter-byte timeout, clamped to 1–10000 ms.
 */
void editorSetEscapeTimeout(int milliseconds) {

	escapeTimeout = std::clamp(milliseconds, 1, 10000);

}

/**
 * @brief Configures bracketed paste for subsequent input sessions.
 *
 * @param enabled Whether to enable bracketed paste.
 */
void editorSetBracketedPaste(bool enabled) {

	pasteEnabled = enabled;

}

/**
 * @brief Reads one byte from the supplied stream descriptor, retrying on EINTR.
 *
 * @param stream Input stream whose file descriptor is read.
 * @return Unsigned byte value, or @c EOF on end of input or read failure.
 */
int editorReadByte(FILE* stream) {

	unsigned char c;
	ssize_t n;
	do {
		n = ::read(fileno(stream), &c, 1);
	} while (n < 0 && errno == EINTR);
	return n == 1 ? c : EOF;

}

/**
 * @brief Inserts repeated copies of a nonzero byte.
 *
 * @param count Number of copies; nonpositive values are ignored.
 * @param key Byte value in the range 1–255.
 * @return Zero, following the editor command convention.
 */
int editorInsertKey(int count, int key) {

	if (count > 0 && key > 0 && key <= 255) insert(std::string(static_cast<std::size_t>(count), static_cast<char>(key)));
	return 0;

}

/**
 * @brief Inserts text at the cursor or replaces the selection.
 *
 * @param text Text to insert.
 */
void editorInsertText(std::string_view text) {

	insert(text);

}

/**
 * @brief Replaces the entire active input buffer.
 *
 * Preserves the previous cursor offset, clamped to the new buffer length.
 *
 * @param text New buffer contents.
 */
void editorReplaceLine(std::string_view text) {

	if (!current) return;
	int point = editorCursor;
	replace(0, editorBufferEnd, text);
	editorCursor = std::min(point, editorBufferEnd);

}

/**
 * @brief Copies a clamped byte range from the active input buffer.
 *
 * @param a Inclusive starting byte offset.
 * @param b Exclusive ending byte offset.
 * @return Owning copy of the range, or an empty string without an active session.
 */
std::string editorCopyText(int a, int b) {

	if (!current) return {};
	a = std::clamp(a, 0, editorBufferEnd);
	b = std::clamp(b, a, editorBufferEnd);
	return current->text.substr(a, b - a);

}

/**
 * @brief Checks whether the active session has selection mode enabled.
 *
 * @return One when selection mode is active, otherwise zero.
 */
int editorSelectionActive() {

	return current && current->markActive;

}

/// Marks the active input session as complete.
int editorSubmit(int, int) {

	editorDone = 1;
	return 0;

}

/**
 * @brief Navigates backward through history, preserving the current draft.
 *
 * @param count Number of entries to move backward; nonpositive values do not move.
 * @return Zero, following the editor command convention.
 */
int editorPreviousHistory(int count, int) {

	if (!current) return 0;
	auto& s = *current;
	if (s.historyPos >= history.size()) {
		s.historyPos = history.size();
		s.draft = s.text;
		s.draftPoint = editorCursor;
	}
	if (s.historyPos && count > 0) {
		s.historyPos -= std::min(s.historyPos, static_cast<std::size_t>(count));
		replace(0, editorBufferEnd, history[s.historyPos]);
	}
	return 0;

}

/**
 * @brief Navigates forward through history and restores the draft at the end.
 *
 * @param count Number of entries to move forward; nonpositive values do not move.
 * @return Zero, following the editor command convention.
 */
int editorNextHistory(int count, int) {

	if (!current || count <= 0) return 0;
	auto& s = *current;
	if (s.historyPos < history.size()) {
		s.historyPos = std::min(history.size(), s.historyPos + static_cast<std::size_t>(count));
		replace(0, editorBufferEnd, s.historyPos == history.size() ? s.draft : history[s.historyPos]);
		if (s.historyPos == history.size()) editorCursor = std::min(s.draftPoint, editorBufferEnd);
	}
	return 0;

}

/**
 * @brief Stores the prompt and updates both exposed prompt pointers.
 *
 * @param prompt Prompt text to copy into editor-owned storage.
 */
void editorSetPrompt(std::string_view prompt) {

	promptStorage = prompt;
	editorPrompt = editorDisplayPrompt = promptStorage.c_str();

}
// Resize watchers synchronize the fallback poller; the renderer queries the actual dimensions.

/**
 * @brief Synchronizes the cached terminal dimensions with the resize watcher.
 *
 * @param rows Terminal height in rows.
 * @param cols Terminal width in columns.
 */
void editorSetScreenSize(int rows, int cols) {

	if (current) {
		current->size.ws_row = static_cast<unsigned short>(rows);
		current->size.ws_col = static_cast<unsigned short>(cols);
	}

}

/**
 * @brief Renders the prompt and buffer using the fallback redisplay implementation.
 *
 * Strips prompt marker bytes, renders control bytes visibly, and repositions the cursor using
 * byte offsets. Production interactive prompts use the custom FixedPrompt renderer.
 */
void editorRedisplay() {

	if (!current) return;
	FILE* out = output();
	const std::string_view p = editorDisplayPrompt ? editorDisplayPrompt : "";
	const auto split = p.rfind('\n');
	if (current->firstDraw) {
		printPrompt(out, p);
	} else if (isatty(fileno(out))) {
		std::fputc('\r', out);
		printPrompt(out, p.substr(split == std::string_view::npos ? 0 : split + 1));
	}
	current->firstDraw = false;
	if (isatty(fileno(out))) printInputBuffer(out);
	std::fflush(out);

}

/**
 * @brief Requests an immediate redraw through the configured redisplay hook.
 *
 * @return Zero.
 */
int editorForcedUpdateDisplay() {

	redraw();
	return 0;

}

/**
 * @brief Sorts and deduplicates completion candidates.
 *
 * @param values Candidate strings, taken by value.
 * @return Empty vector for no matches, a single candidate for a unique match, or the common prefix followed by all distinct candidates.
 */
std::vector<std::string> editorCompletionMatches(std::vector<std::string> values) {

	if (values.empty()) return {};
	std::sort(values.begin(), values.end());
	values.erase(std::unique(values.begin(), values.end()), values.end());
	if (values.size() > 1) values.insert(values.begin(), commonPrefix(values));
	return values;

}

/**
 * @brief Completes the token preceding the cursor using the configured completion hook.
 *
 * Uses filesystem completion when the hook returns no candidates without marking completion
 * handled.
 *
 * @param mode Completion mode: ? for listing only, ! for insertion and listing, or another value for insertion only.
 * @return Zero, following the editor command convention.
 */
int editorCompleteInternal(int mode) {

	if (!current) return 0;
	const int finish = editorCursor;
	int start = finish;
	const std::string_view breaks = editorWordBreakCharacters ? editorWordBreakCharacters : ";";
	while (start > 0 && breaks.find(current->text[start - 1]) == std::string_view::npos) --start;
	const auto text = current->text.substr(start, finish - start);
	editorCompletionHandled = 0;
	auto m = editorCompletionHook ? editorCompletionHook(text, start, finish) : std::vector<std::string>{};
	if (m.empty() && !editorCompletionHandled) m = fileMatches(text);
	if (m.empty()) return 0;
	if (m.size() == 1) {
		if (mode != '?') insertUniqueMatch(std::move(m.front()), text, start, finish);
	} else {
		completeMultiple(m, text, start, finish, mode);
	}
	return 0;

}

/// Completes the current token or displays matches on a repeated Tab press.
int editorComplete(int, int) {

	editorCompletionAppendCharacter = ' ';
	return editorCompleteInternal(editorLastCommand == static_cast<EditorCommand*>(editorComplete) ? '?' : '\t');

}

/**
 * @brief Runs a synchronous native line-editing session.
 *
 * Installs a terminal guard, invokes startup and redisplay hooks, processes input until
 * completion, and restores terminal and exposed session state on scope exit. Nested sessions
 * are rejected with EBUSY.
 *
 * @param prompt Prompt text displayed for the session.
 * @return Submitted text (including an empty string), or @c std::nullopt on EOF without text or when another session is active.
 */
std::optional<std::string> editLine(std::string_view prompt) {

#ifdef ISOCMD_TEST_TERMINAL_SYNC
	isocmd_tests::terminal_sync::lineEditorStarted();
#endif
	initialize();
	if (current) {
		errno = EBUSY;
		return std::nullopt;
	}
	Session session;

	/// Clears exposed session state on normal exit or exception unwinding.
	struct Reset {
		~Reset() {

			current = nullptr;
			editorLineBuffer = nullptr;
			editorCursor = editorBufferEnd = editorSelectionMark = editorDone = 0;
			editorLastCommand = nullptr;
			editorPrompt = editorDisplayPrompt = nullptr;

		}
	} reset;
	current = &session;
	ioctl(STDOUT_FILENO, TIOCGWINSZ, &session.size);
	editorCursor = editorSelectionMark = editorDone = 0;
	editorLastCommand = nullptr;
	syncBuffer();
	editorSetPrompt(prompt);
	TerminalGuard guard;
	if (editorStartupHook) editorStartupHook();
	redraw();
	while (!editorDone) {
		const int byte = inputByte();
		if (byte < 0) {
			if (!editorDone) {
				session.eof = session.text.empty();
				editorDone = 1;
			}
			break;
		}
		dispatch(readKey(byte));
		if (!editorDone) redraw();
	}
	// FixedPrompt owns the successful-submit newline. EOF and the fallback
	// renderer have no such wrapper responsibility.
	if (session.eof || editorRedisplayHook == static_cast<EditorRedisplay*>(editorRedisplay) || !isatty(fileno(output()))) {
		std::fputc('\n', output());
		std::fflush(output());
	}
	if (session.eof) return std::nullopt;
	return std::move(session.text);

}

/**
 * @brief Requests the terminal cursor position and extracts its row.
 *
 * Waits up to 100 ms and examines at most 4096 bytes. Preserves input bytes preceding the
 * response, or all collected bytes when no valid report is received, as typeahead.
 *
 * @return One-based row from a valid terminal response, or zero if unavailable.
 */
int editorQueryCursorRow() {

	if (!current || !isatty(STDIN_FILENO) || !isatty(fileno(output()))) return 0;
	std::fputs("\033[6n", output());
	std::fflush(output());
	const auto deadline = steady_clock::now() + milliseconds(100);
	std::string bytes;
	int row = 0;
	while (bytes.size() < 4096) {
		const auto remaining = duration_cast<milliseconds>(deadline - steady_clock::now()).count();
		if (remaining <= 0) break;
		pollfd fd{STDIN_FILENO, POLLIN, 0};
		const int ready = poll(&fd, 1, static_cast<int>(remaining));
		if (ready < 0 && errno == EINTR) continue;
		if (ready <= 0 || !(fd.revents & POLLIN)) break;
		const int c = editorReadByte(stdin);
		if (c < 0) break;
		bytes += static_cast<char>(c);
		if (c != 'R') continue;
		row = extractCursorRow(bytes);
		if (row) break;
	}
	// Typeahead that precedes the report (or arrives when no report is sent)
	// belongs to the editor, not to the completion-list renderer.
	for (unsigned char c : bytes) queued.push_back(c);
	return row;

}

/// Discards all queued typeahead bytes.
void editorDiscardPendingInput() noexcept {

	queued.clear();

}

/**
 * @brief Removes the oldest queued typeahead byte.
 *
 * @return Unsigned byte value, or -1 when the queue is empty.
 */
int editorTakePendingInput() noexcept {

	if (queued.empty()) return - 1;
	int c = queued.front();
	queued.pop_front();
	return c;

}
