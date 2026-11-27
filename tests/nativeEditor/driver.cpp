// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file
 * @brief Exercises the standalone native line editor through a parent-controlled PTY.
 */

#include <cerrno>
#include <chrono>
#include <clocale>
#include <cwchar>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

import isocmd.input.lineEditor;

namespace {
	using namespace std::chrono;
	void check(bool ok, const char* message) {

		if (!ok) throw std::runtime_error(message);

	}
	using Line = std::optional<std::string>;
	int ticks = 0, listings = 0;
	int hook() { if (++ticks == 2) { std::fputs("IDLE-HOOK\n", stdout); std::fflush(stdout); } return 0; }
	int prefill() { editorInsertText("prefill"); editorStartupHook = nullptr; return 0; }
	int throwHook() { throw std::runtime_error("intentional unwind"); }
	int hotkey(int, int) { editorReplaceLine("page-command"); return editorSubmit(1, '\n'); }
	int customGetc(FILE* stream) { return editorReadByte(stream); }
	std::vector<std::string> complete(std::string_view text, int, int) {

		editorCompletionHandled = 1;
		editorCompletionAppendCharacter = ' ';
		std::vector<std::string> values;
		for (std::string_view v : {"alpha", "alpine", "beta"}) if (v.starts_with(text)) values.emplace_back(v);
		return editorCompletionMatches(std::move(values));
	}
	int tab(int, int) { return editorCompleteInternal(editorLastCommand == tab ? '?' : '!'); }
	void listing(const std::vector<std::string>& matches, int n, int longest) {

		check(n == 2 && longest == 6, "completion count");
		check(std::string_view(matches[0]) == "alp" && std::string_view(matches[1]) == "alpha" && std::string_view(matches[2]) == "alpine" && matches.size() == 3, "completion list format");
		++listings;
		// The parent interleaves ordinary typeahead and the CPR response.
		if (listings == 1) check(editorQueryCursorRow() == 7, "cursor row");
	}

	/**
	 * @brief Compares terminal flags, control characters and speeds with the baseline.
	 */
	void terminalMatches(const termios& before) {

		termios after{};
		check(tcgetattr(0, &after) == 0, "tcgetattr after");
		check(before.c_iflag == after.c_iflag && before.c_oflag == after.c_oflag && before.c_cflag == after.c_cflag && before.c_lflag == after.c_lflag && std::memcmp(before.c_cc, after.c_cc, NCCS) == 0 && cfgetispeed(&before) == cfgetispeed(&after) && cfgetospeed(&before) == cfgetospeed(&after), "terminal restoration");
	}
	void readExpected(const char* prompt, std::string_view expected, const termios& before) {

		Line result = editLine(prompt);
		check(result && std::string_view(*result) == expected, prompt);
		check(!editorLineBuffer && !editorPrompt && editorBufferEnd == 0 && editorCursor == 0, "session cleanup");
		terminalMatches(before);
	}

	/**
	 * @brief Checks editing results and terminal restoration inside the PTY child.
	 */
	int child() {

		alarm(20);
		try {
			editorInitializeLocale();
			std::mbstate_t state{};
			wchar_t glyph{};
			check(std::mbrtowc(&glyph, "↵", 3, &state) == 3 && wcwidth(glyph) == 1, "UTF-8 prompt character width");
			termios before{};
			check(tcgetattr(0, &before) == 0, "tcgetattr before");
			editorEventHook = hook;
			// The first lookup must initialize default bindings without an explicit setup call.
			check(editorKeySequenceHandler("A") == editorInsertKey, "lazy default printable binding");
			check(editorKeySequenceHandler("\t") == editorComplete, "lazy default Tab binding");
			check(editorKeySequenceHandler("\033OA") == editorPreviousHistory, "SS3 alias");
			check(editorKeySequenceHandler("\033[1~") == editorKeySequenceHandler("\033[H"), "Home alias");
			check(editorBindKey(-1, hotkey) == -1 && editorBindKey(256, hotkey) == -1, "byte range");
			check(editorCompletionMatches({}).empty(), "empty matches");
			check(editorCompletionMatches({"beta", "beta"}) == std::vector<std::string>{"beta"}, "deduplicate match");
			check(editorCompletionMatches({"alpine", "alpha", "alpha"}) == std::vector<std::string>{"alp", "alpha", "alpine"}, "sorted common prefix");
			check(editorCompletionMatches({"é", "ê"}).front().empty(), "UTF-8 prefix boundary");
			editorSetEscapeTimeout(10);
			readExpected("EDIT> ", "abXcdef", before);
			editorEventHook = nullptr;
			readExpected("UTF8> ", "aΩz", before);
			readExpected("EMPTY> ", "", before);
			editorBindKey('X', hotkey);
			readExpected("BYTE-HOTKEY> ", "page-command", before);
			check(editorKeySequenceHandler("X") == hotkey, "session entry preserves custom binding");
			editorBindKey('X', editorInsertKey);
			readExpected("BYTE-RESTORE> ", "X", before);
			readExpected("SELECT> ", "aXf", before);
			readExpected("PASTE> ", "a\nP?R/\n"+std::string(5000, 'b'), before);
			editorAddHistory("one");
			editorAddHistory("two");
			check(editorHistoryList().size() == 2 && std::string_view(editorHistoryList()[0]) == "one", "history storage");
			readExpected("HISTORY> ", "draft", before);
			readExpected("OLDER> ", "one", before);
			readExpected("NEWER> ", "two", before);
			check(!editorKeySequenceHandler("\022"), "Ctrl+R unbound");
			for (std::string_view key : {"\031", "\027", "\037", "\024", "\032"})
				check(!editorKeySequenceHandler(key), "removed editing command unbound");
			readExpected("UNBOUND> ", "abXcd", before);
			readExpected("LINE-DELETE> ", "X", before);
			readExpected("WORD-DELETE> ", "one ", before);
			readExpected("FORWARD-WORD-DELETE> ", " two", before);
			editorStartupHook = prefill;
			readExpected("PREFILL> ", "prefill!", before);
			editorBindKeySequence("\033[5~", hotkey);
			check(editorKeySequenceHandler("\033[5~") == hotkey, "key lookup");
			readExpected("HOTKEY> ", "page-command", before);
			editorBindKeySequence("\033", hotkey);
			readExpected("ESC> ", "page-command", before);
			editorCompletionHook = complete;
			editorCompletionDisplayHook = listing;
			editorBindKey('\t', tab);
			readExpected("COMPLETE> ", "alpha ", before);
			check(listings == 1, "listing count");
			editorCompletionHook = nullptr;
			editorCompletionDisplayHook = nullptr;
			editorBindKey('\t', editorComplete);
			readExpected("FILE> ", "folder name/", before);
			readExpected("REGULAR> ", "regular.txt ", before);
			readExpected("HIDDEN> ", ".hidden ", before);
			readExpected("SYMLINK> ", "linked folder/", before);
			readExpected("AMBIGUOUS> ", "shared-", before);
			readExpected("MISSING> ", "missing/path", before);
			readExpected("HOME> ", "~/folder name/", before);
			editorInputReader = customGetc;
			readExpected("CUSTOM> ", "ok", before);
			editorInputReader = editorReadByte;
			Line eof = editLine("EOF> ");
			check(!eof, "Ctrl+D");
			terminalMatches(before);
			editorStartupHook = throwHook;
			bool caught = false;
			try { Line unused = editLine("THROW> "); } catch (const std::runtime_error&) { caught = true; }
			check(caught && !editorLineBuffer, "exception cleanup");
			terminalMatches(before);
			editorStartupHook = nullptr;
			editorClearHistory();
			check(editorHistoryList().empty(), "clear history");
			std::puts("NATIVE-EDITOR-PASS");
			return 0;
		} catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n';
		return 1;
		}
	}

	/**
	 * @brief Owns the child process and PTY and drives the fixture prompts.
	 */
	struct Parent {
		int fd = -1;
		pid_t pid = -1;
		std::string output;
		std::size_t offset = 0;
		~Parent() { if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); } if (fd >= 0) close(fd); }
		void wait(std::string_view marker) {

			const auto limit = steady_clock::now() + seconds(5);
			while (true) {
				auto pos = output.find(marker, offset);
				if (pos != std::string::npos) { offset = pos + marker.size(); return; }
				auto ms = duration_cast<milliseconds>(limit - steady_clock::now()).count();
				check(ms > 0, "PTY timeout");
				pollfd p{fd, POLLIN, 0};
				int ready = poll(&p, 1, static_cast<int>(ms));
				if (ready < 0 && errno == EINTR) continue;
				check(ready > 0, "PTY poll");
				char buf[8192];
				ssize_t n = read(fd, buf, sizeof(buf));
				check(n > 0, "PTY closed before marker");
				output.append(buf, static_cast<std::size_t>(n));
			}
		}
		void send(std::string_view input) {

			while (!input.empty()) { ssize_t n = write(fd, input.data(), input.size()); if (n < 0 && errno == EINTR) continue; check(n > 0, "PTY write"); input.remove_prefix(static_cast<std::size_t>(n)); }
		}
		void step(std::string_view marker, std::string_view input) {

			wait(marker);
			send(input);

		}
	};
}

int main() {

	Parent p;
	try {
		p.pid = forkpty(&p.fd, nullptr, nullptr, nullptr);
		check(p.pid >= 0, "forkpty");
		if (p.pid == 0) return child();
		p.wait("EDIT> ");
		p.wait("IDLE-HOOK");
		p.send("abc\002X\006def\n");
		p.step("UTF8> ", "aéΩz\002\002\177\n");
		p.step("EMPTY> ", "\n");
		p.step("BYTE-HOTKEY> ", "X");
		p.step("BYTE-RESTORE> ", "X\n");
		p.step("SELECT> ", "abcdef\001\006\033[1;2C\033[1;2C\033[1;2C\033[1;2CX\n");
		p.step("PASTE> ", "\033[200~a\nP?R/\r\n"+std::string(5000, 'b') + "\033[201~\n");
		p.step("HISTORY> ", "draft\033[A\033[A\033[B\033[B\n");
		p.step("OLDER> ", "\033[A\033[A\n");
		p.step("NEWER> ", "\033[A\033[A\033[B\n");
		p.step("UNBOUND> ", "abcd\002\002\022\031\027\037\024X\n");
		p.step("LINE-DELETE> ", "abcd\002\002\025\013X\n");
		p.step("WORD-DELETE> ", "one two\033\177\n");
		p.step("FORWARD-WORD-DELETE> ", "one two\001\033d\n");
		p.step("PREFILL> ", "!\n");
		p.step("HOTKEY> ", "\033[5~");
		p.step("ESC> ", "\033");
		p.step("COMPLETE> ", "al\t");
		p.wait("\033[6n");
		p.send("h\033[7;3R\t\n");
		p.step("FILE> ", "fold\t\n");
		p.step("REGULAR> ", "reg\t\n");
		p.step("HIDDEN> ", ".hid\t\n");
		p.step("SYMLINK> ", "link\t\n");
		p.step("AMBIGUOUS> ", "sha\t\n");
		p.step("MISSING> ", "missing/path\t\n");
		p.step("HOME> ", "~/fold\t\n");
		p.step("CUSTOM> ", "ok\n");
		p.step("EOF> ", "\004");
		p.wait("NATIVE-EDITOR-PASS");
		int status = 0;
		check(waitpid(p.pid, &status, 0) == p.pid, "waitpid");
		p.pid = -1;
		check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "child result");
		std::cout << "PASS: native editor PTY editing, UTF-8, selection, paste, history, completion, CPR typeahead, callbacks, EOF and terminal restoration\n";
		return 0;
	} catch (const std::exception& e) { std::cerr << e.what() << "\n" << p.output.substr(p.output.size() > 2000 ? p.output.size() - 2000:0) << '\n';
	return 1;
	}
}
