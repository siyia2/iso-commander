// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../common/testHarness.h"
#include "../common/terminalSync.h"

import isocmd.write2usb.ui.textWidth;

namespace isocmd_tests {
	namespace {
		using Clock = std::chrono::steady_clock;
		using namespace std::chrono_literals;

		// Own the PTY and child on every exit path, including failed assertions.
		class TerminalSession {
			int fd_ = -1;
			pid_t pid_ = -1;
			const Clock::time_point deadline_ = Clock::now() + 90s;

			[[noreturn]] static void fail(const char* operation) {

				throw std::system_error(errno, std::generic_category(), operation);

			}

			bool drainExitOutput() {

				struct pollfd pfd{fd_, POLLIN, 0};
				const int ready = ::poll(&pfd, 1, 10);
				if (ready < 0) {
					if (errno == EINTR) return false;
					fail("poll during child exit");
				}
				if (ready == 0) return false;
				if (pfd.revents & POLLNVAL)
				    throw std::runtime_error("invalid PTY during child exit");
				if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) return false;
				char buffer[8192];
				const auto count = ::read(fd_, buffer, sizeof(buffer));
				if (count < 0 && (errno == EINTR || errno == EAGAIN)) return false;
				if (count == 0 || (count < 0 && errno == EIO)) {
					return true;
				}
				if (count < 0) fail("read PTY during child exit");
				output.append(buffer, static_cast<std::size_t>(count));
				return false;

			}

		public:
			std::string output;
			std::string filterLabel;

			explicit TerminalSession(const char* binary, const char* argument = "--terminal-child", unsigned short cols = 80) {

				// forkpty establishes a controlling terminal and initial dimensions
				// before the child can initialize the production resize watcher.
				struct winsize size{};
				size.ws_row = 24;
				size.ws_col = cols;
				char* const args[] = {const_cast<char*>(binary),
					                  const_cast<char*>(argument), nullptr};
				pid_ = ::forkpty(&fd_, nullptr, nullptr, &size);
				if (pid_ < 0) fail("forkpty");
				if (pid_ == 0) {
					::execv(binary, args);
					::_exit(127);
				}

			}

			TerminalSession(const TerminalSession&) = delete;
			TerminalSession& operator=(const TerminalSession&) = delete;

			~TerminalSession() {

				if (pid_ > 0) {
					::kill(pid_, SIGKILL);
					while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
				}
				if (fd_ >= 0) ::close(fd_);

			}

			std::size_t waitFor(std::string_view marker, std::size_t start = 0) {

				const auto limit = std::min(deadline_, Clock::now() + 8s);
				for (;;) {
					if (const auto pos = output.find(marker, start); pos != std::string::npos)
					    return pos + marker.size();
					const auto remaining = limit - Clock::now();
					if (remaining <= Clock::duration::zero())
					    throw std::runtime_error("timed out waiting for " + std::string(marker));
					const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
					struct pollfd pfd{fd_, POLLIN, 0};
					const int ready = ::poll(&pfd, 1, static_cast<int>(ms));
					if (ready < 0) {
						if (errno == EINTR) continue;
						fail("poll");
					}
					if (ready == 0) continue;
					char buffer[8192];
					const auto count = ::read(fd_, buffer, sizeof(buffer));
					if (count < 0 && errno == EINTR) continue;
					if (count == 0 || (count < 0 && errno == EIO))
					    throw std::runtime_error("PTY closed before " + std::string(marker));
					if (count < 0) fail("read PTY");
					output.append(buffer, static_cast<std::size_t>(count));
				}

			}

			void send(std::string_view input) {

				while (!input.empty()) {
					const auto count = ::write(fd_, input.data(), input.size());
					if (count < 0 && errno == EINTR) continue;
					if (count < 0) fail("write PTY");
					if (count == 0) throw std::runtime_error("zero-length PTY write");
					input.remove_prefix(static_cast<std::size_t>(count));
				}

			}

			void resize(unsigned short rows, unsigned short cols) {

				struct winsize size{};
				size.ws_row = rows;
				size.ws_col = cols;
				// The kernel delivers SIGWINCH to the foreground process group.
				if (::ioctl(fd_, TIOCSWINSZ, &size) < 0) fail("TIOCSWINSZ");

			}

			void waitForExit() {

				const auto limit = std::min(deadline_, Clock::now() + 8s);
				bool reaped = false;
				bool eof = false;
				int status = 0;
				// Keep consuming output while waiting for the child. A full PTY can
				// otherwise block Line editor cleanup or sanitizer diagnostics indefinitely.
				// Drain after waitpid as well so failure diagnostics include the tail.
				while (!reaped || !eof) {
					if (!reaped) {
						const auto result = ::waitpid(pid_, &status, WNOHANG);
						if (result == pid_) {
							pid_ = -1;
							reaped = true;
						} else if (result < 0 && errno != EINTR) {
							fail("waitpid");
						}
					}
					if (reaped && eof) break;
					if (Clock::now() >= limit)
					    throw std::runtime_error(reaped ? "PTY did not close after child exit" : "child did not exit");
					if (eof) {
						std::this_thread::sleep_for(10ms);
						continue;
					}
					eof = drainExitOutput();
				}
				if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
				    throw std::runtime_error("child exit status: " + std::to_string(status));

			}
		};

		// Keep terminal control sequences from affecting failure diagnostics.
		void printOutput(std::string_view output) {

			constexpr char hex[] = "0123456789abcdef";
			std::cerr << "PTY output: ";
			for (unsigned char c : output) {
				if (c >= 32 && c < 127) std::cerr << static_cast<char>(c);
				else std::cerr << "\\x" << hex[c >> 4] << hex[c & 15];
			}
			std::cerr << '\n';

		}

		/// Advances to the end of a CSI sequence, or the end of the input.
		void skipCsi(std::string_view raw, std::size_t& i) {

			i += 2;
			while (i < raw.size() && !(raw[i] >= 0x40 && raw[i] <= 0x7e)) ++i;

		}

		/// Advances to the BEL terminator of a test OSC marker, or the end of the input.
		void skipOsc(std::string_view raw, std::size_t& i) {

			while (i + 1 < raw.size() && raw[i + 1] != '\a') ++i;
			if (i + 1 < raw.size()) ++i;

		}

		// Strip CSI, test OSC markers and Line editor delimiters; keep UTF-8 glyphs.
		std::string plain(std::string_view raw) {

			std::string result;
			for (std::size_t i = 0; i < raw.size(); ++i) {
				const unsigned char c = raw[i];
				if (c == 27 && i + 1 < raw.size() && raw[i + 1] == '[') skipCsi(raw, i);
				else if (c == 27 && i + 1 < raw.size() && raw[i + 1] == ']') skipOsc(raw, i);
				else if (c != 1 && c != 2 && c != '\r') result += raw[i];
			}
			return result;

		}

		bool has(std::string_view s,std::string_view word) {

			return s.find(word)!=std::string_view::npos;

		}

		// Isolate the latest full list redraw rather than matching stale scrollback.
		std::string listFrame(const TerminalSession& term,std::size_t start,std::size_t end) {

			const auto clear=term.output.rfind("\033[2J\033[3J\033[H",end);
			if (clear!=std::string::npos && clear>=start) start=clear;
			return plain(std::string_view(term.output).substr(start,end-start));

		}

		// The test-only link wrappers distinguish a new line-editor session from
		// redisplays of the previous one, and a real pause from a progress frame.
		// Wait for the complete input row as well, not merely entry to line editor.
		enum class Prompt { input, filter, pause };
		constexpr auto inputPrompt = Prompt::input;
		constexpr auto filterPrompt = Prompt::filter;
		constexpr auto pausePrompt = Prompt::pause;

		constexpr std::string_view rowEnd = "\033[K\r";

		std::size_t waitForInputRow(TerminalSession& term, std::size_t start) {

			return term.waitFor(rowEnd, start);

		}

		std::size_t waitForNewInput(TerminalSession& term, std::size_t start = 0) {

			return waitForInputRow(term, term.waitFor(terminal_sync::input, start));

		}

		std::string_view rawInputRow(std::string_view output, std::size_t end) {

			CHECK(end >= rowEnd.size() && end <= output.size());
			const auto stop = end - rowEnd.size();
			CHECK(output.substr(stop, rowEnd.size()) == rowEnd);
			CHECK(stop > 0);
			const auto start = output.rfind('\r', stop - 1);
			CHECK(start != std::string_view::npos);
			return output.substr(start + 1, stop - start - 1);

		}

		std::string inputRow(const TerminalSession& term, std::size_t end) {

			return plain(rawInputRow(term.output, end));

		}

		std::size_t waitForStepPrompt(TerminalSession& term, Prompt prompt, std::size_t start) {

			if (prompt == pausePrompt) return term.waitFor(terminal_sync::pause, start);
			const auto end = waitForNewInput(term, start);
			if (prompt == filterPrompt) {
				term.filterLabel = inputRow(term, end);
				CHECK(!term.filterLabel.empty());
			}
			return end;

		}

		// A resize does not start a new line-editor session. Require a completed
		// input row (and, where appropriate, the caller's repaint evidence).
		std::size_t waitForRepaintedInput(TerminalSession& term, std::size_t start) {

			const auto begin = term.waitFor("\033[?2026h", start);
			// Some repaint implementations close the synchronized update before
			// the input row; others include the row in the update. Wait for both.
			const auto row = waitForInputRow(term, begin);
			term.waitFor("\033[?2026l", begin);
			return row;

		}

		// The empty input row gives us the actual label, whatever its wording.
		// Match only the editable portion after each completed preview repaint.
		std::string queryFrame(TerminalSession& term, std::size_t start, std::string_view query,
		                       bool visibleTail = false) {

			CHECK(!term.filterLabel.empty());
			for (;;) {
				const auto begin = term.waitFor("\033[?2026h", start);
				const auto painted = term.waitFor("\033[?2026l", begin);
				const auto end = waitForInputRow(term, painted);
				const auto row = inputRow(term, end);
				if (row.starts_with(term.filterLabel)) {
					const auto input = std::string_view(row).substr(term.filterLabel.size());
					if (visibleTail ? input.ends_with(query) : input == query)
						return listFrame(term, begin, end);
				}
				start = end;
			}

		}

		std::string screenFrame(TerminalSession& term,std::size_t& start,std::string_view expected) {

			try {
				for (;;) {
					const auto begin=term.waitFor("\033[?2026h",start);
					const auto end=term.waitFor("\033[?2026l",begin);
					auto frame=plain(std::string_view(term.output).substr(begin,end-begin));
					start=end;
					if (has(frame,expected)) return frame;
				}
			} catch (const std::exception& e) {
				throw std::runtime_error("waiting for screen frame containing '" +
				    std::string(expected) + "': " + e.what());
			}

		}

		// Input redisplay may use carriage returns and horizontal cursor moves,
		// but must not advance vertically or print an entire multi-screen paste.
		void checkSingleRowInput(std::string_view output, std::string_view label) {

			CHECK(has(plain(output), label)); // The fixed label survives scrolling.
			CHECK(!has(output, "\n"));
			CHECK(!has(output, "\033[A") && !has(output, "\033[B"));
			CHECK(!has(plain(output), std::string(2400, 'b')));

		}

		// Check that the gap is exactly one visible, non-space cell, not a
		// particular Unicode overflow glyph.
		void checkOverflowGap(std::string_view row, std::string_view label, std::string_view tail) {

			CHECK(row.starts_with(label) && row.ends_with(tail));
			CHECK(row.size() >= label.size() + tail.size());
			const auto gap = row.substr(label.size(), row.size() - label.size() - tail.size());
			CHECK(visibleWidth(std::string(gap)) == 1 && gap != " ");

		}

		void checkLongInput(TerminalSession& term, std::size_t start) {

			const auto prompt = waitForNewInput(term, start);
			const auto label = inputRow(term, prompt);
			CHECK(!label.empty());
			term.send("START-OF-INPUT" + std::string(160, 'a') + "TYPED-END!");
			const auto typed = waitForInputRow(term, term.waitFor("!", prompt));
			checkSingleRowInput(std::string_view(term.output).substr(prompt, typed - prompt), label);
			CHECK(inputRow(term, typed).starts_with(label));
			term.send("\033[200~" + std::string(2400, 'b') + "PASTE-END@\nmultiline\nPASTE-FINAL#\033[201~");
			const auto pasted = waitForInputRow(term, term.waitFor("#", typed));
			checkSingleRowInput(std::string_view(term.output).substr(typed, pasted - typed), label);
			CHECK(inputRow(term, pasted).starts_with(label));
			// Resize with the cursor at the far end of the line, then edit its
			// hidden beginning and submit the entire, untruncated buffer.
			term.resize(18, 40);
			const auto repaint = term.waitFor("LONG-REPAINT", pasted);
			const auto restored = waitForInputRow(term, term.waitFor("#", repaint));
			term.resize(32, 120);
			const auto wider = term.waitFor("LONG-REPAINT", restored);
			const auto widened = waitForInputRow(term, term.waitFor("#", wider));
			term.send("\x01");
			const auto beginning = term.waitFor("START-OF-INPUT", widened);
			const auto right = waitForInputRow(term, beginning);
			const auto row = inputRow(term, right);
			CHECK(row.starts_with(label + "START-OF-INPUT"));
			CHECK(!row.ends_with("PASTE-FINAL#") && !row.ends_with("a") && !row.ends_with("b"));
			CHECK(visibleWidth(row) <= 119);
			term.send("X\x05\n");
			term.waitFor("TEST-LONG-INPUT-PASS", right);

		}

		void checkOverflowSpace(TerminalSession& term, std::size_t start) {

			// The fixture has a fixed-width label. The viewport starts at a space.
			const auto prompt = waitForNewInput(term, start);
			const auto label = inputRow(term, prompt);
			term.send("\033[200~" + std::string(160, 'a') + " " + std::string(84, 'b') + "\033[201~");
			const auto tail = waitForInputRow(term, term.waitFor(std::string(84, 'b'), prompt));
			checkOverflowGap(inputRow(term, tail), label, std::string(84, 'b'));
			term.send("\x01X\x05\n");
			term.waitFor("TEST-OVERFLOW-SPACE-PASS", tail);

		}

	} // namespace

	int terminalTest(const char* binary) {

		try {
			TerminalSession terminal(binary);
			try {
				const auto initial = waitForNewInput(terminal);
				terminal.send("abc");
				waitForInputRow(terminal, terminal.waitFor("abc", initial));
				for (const auto& [rows, cols] : {std::pair{18, 45}, std::pair{32, 100}, std::pair{12, 30}, std::pair{24, 80}}) {
					const auto start = terminal.output.size();
					terminal.resize(rows, cols);
					const auto repainted = terminal.waitFor("TEST-REPAINT", start);
					waitForInputRow(terminal, terminal.waitFor("abc", repainted));
				}
				// Move the editing cursor before another character is inserted.
				terminal.send("\x02X\x06" "def\n");
				const auto passed = terminal.waitFor("TEST-TERMINAL-PASS");
				checkLongInput(terminal, passed);
				checkOverflowSpace(terminal, terminal.waitFor("TEST-LONG-INPUT-PASS", passed));
				const auto unicode = terminal.waitFor("UNICODE ↵ Ω> ", terminal.waitFor("TEST-OVERFLOW-SPACE-PASS", passed));
				terminal.send("é界");
				const auto typed = waitForInputRow(terminal, terminal.waitFor("é界", unicode));
				CHECK(inputRow(terminal, typed) == "UNICODE ↵ Ω> é界");
				terminal.send("\n");
				terminal.waitFor("TEST-UNICODE-PASS", typed);
				terminal.waitForExit();
			} catch (...) {
				printOutput(terminal.output);
				throw;
			}
			std::cout << "PASS: PTY resize, long single-row input, input preservation and Line editor restoration\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: PTY resize: " << e.what() << '\n';
			return 1;
		}

	}

	int lineEditorGuardsTest(const char* binary) {

		try {
			TerminalSession terminal(binary, "--editor-guards-child");
			try {
				waitForNewInput(terminal);
				terminal.send("\x1b");
				const auto eof = waitForNewInput(terminal, terminal.output.size());
				terminal.send("\x04");
				const auto pause = terminal.waitFor(terminal_sync::pause, terminal.waitFor("RAII-PAUSE", eof));
				terminal.resize(21, 70);
				terminal.waitFor(terminal_sync::pause, terminal.waitFor("RAII-PAUSE", pause));
				terminal.send("\x1b");
				const char* inputs[] = {"5 6\n", "\x1b[6~", "\x1b", "r", "n\n", "\x1b"};
				for (std::size_t stage = 0; stage < std::size(inputs); ++stage) {
					terminal.waitFor("RAII-SETTINGS-" + std::to_string(stage) + "-READY");
					terminal.send(inputs[stage]);
				}
				terminal.waitFor("RAII-GUARDS-PASS");
				terminal.waitForExit();
			} catch (...) { printOutput(terminal.output); throw; }
			std::cout << "PASS: RAII keybindings, hooks, completion and terminal restoration\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: RAII restoration: " << e.what() << '\n'; return 1;
		}

	}

	// Drive the real application entry point, linked with a sandbox-only lock.
	int menuTest(const char* binary) {

		try {
			TerminalSession terminal(binary, nullptr);
			try {
				auto menu = [&](std::string_view title, std::size_t start) {

					const auto end = waitForNewInput(terminal, start);
					CHECK(has(listFrame(terminal, start, end), title));
					return end;

				};
				auto start = menu("Main Menu", 0);
				terminal.send("invalid");
				waitForInputRow(terminal, terminal.waitFor("invalid", start));
				start = terminal.output.size(); terminal.resize(18, 55);
				auto end = waitForRepaintedInput(terminal, start);
				CHECK(has(listFrame(terminal, start, end), "Main Menu"));
				CHECK(inputRow(terminal, end).ends_with("invalid"));
				start = terminal.output.size(); terminal.send("\n");
				menu("Main Menu", start);
				start = terminal.output.size(); terminal.send("1\n");
				menu("ManageISO", start);
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Main Menu", start);
				start = terminal.output.size(); terminal.send("2\n");
				menu("Convert2ISO", start);
				start = terminal.output.size(); terminal.send("1\n");
				end = waitForNewInput(terminal, start);
				// The search prompt has a multiline header; its copy is not a contract.
				const auto header = terminal.output.rfind(terminal_sync::input, end);
				CHECK(header != std::string::npos && header >= start);
				CHECK(has(plain(std::string_view(terminal.output).substr(header, end-header)), "\n"));
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Convert2ISO", start);
				start = terminal.output.size(); terminal.send("\x04");
				menu("Main Menu", start);
				start = terminal.output.size(); terminal.send("3\n");
				waitForNewInput(terminal, start);
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Main Menu", start);
				start = terminal.output.size(); terminal.send("4\n");
				end = waitForNewInput(terminal, start);
				CHECK(has(listFrame(terminal, start, end), "Settings Editor"));
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Main Menu", start);
				terminal.send("5\n"); terminal.waitForExit();
			} catch (...) { printOutput(terminal.output); throw; }
			std::cout << "PASS: interactive main menu, submenus, settings, import and resize\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: interactive navigation: " << e.what() << '\n'; return 1;
		}

	}

	namespace {
		constexpr auto selectPrompt = inputPrompt;

		std::string workflowStep(TerminalSession& term, std::string_view input, Prompt prompt) {

			const auto start=term.output.size(); term.send(input);
			const auto end = waitForStepPrompt(term, prompt, start);
			return listFrame(term,start,end);

		}

		void checkWorkflowPage(TerminalSession& term, std::string_view input, std::string_view title,
		                       std::string_view first, std::string_view absent) {

			const auto frame=workflowStep(term, input,selectPrompt);
			CHECK(has(frame,title) && has(frame,first) && !has(frame,absent));

		}

		std::string workflowPreview(TerminalSession& term, std::string_view key, std::string_view query) {

			const auto start=term.output.size(); term.send(key);
			return queryFrame(term,start,query);

		}

		void checkWorkflowPagination(TerminalSession& term) {

			CHECK(has(listFrame(term, 0, waitForNewInput(term)), "Main Menu"));
			workflowStep(term, "1\n",inputPrompt);
			checkWorkflowPage(term, "5\n","Page 1/4","groupalpha1.iso","groupalpha3.iso");
			checkWorkflowPage(term, "\x1b[5~","Page 1/4","groupalpha1.iso","groupalpha3.iso");
			checkWorkflowPage(term, "\x1b[6~","Page 2/4","groupalpha3.iso","groupalpha1.iso");
			checkWorkflowPage(term, "g4\n","Page 4/4","groupbeta1.iso","groupalpha1.iso");
			checkWorkflowPage(term, "\x1b[6~","Page 4/4","groupbeta1.iso","groupalpha1.iso");
			checkWorkflowPage(term, "g1\n","Page 1/4","groupalpha1.iso","groupbeta1.iso");

		}

		void checkLongFilterInput(TerminalSession& term) {

			// A long no-match query must scroll only the input, not the Filter label.
			const std::string query = "FILTER-START!" + std::string(2400, 'z') + "FILTER-TAIL!";
			auto begin=term.output.size();
			term.send("\x15\033[200~" + query + "\033[201~");
			auto frame=queryFrame(term,begin,"FILTER-TAIL!",true);
			std::size_t end;
			CHECK(has(frame,"No filter results"));
			CHECK(has(frame, term.filterLabel) && has(frame, "FILTER-TAIL!"));
			begin=term.output.size(); term.send("\x01");
			end=term.waitFor("FILTER-START!",begin);
			end=waitForInputRow(term,end);
			const auto row = inputRow(term, end);
			CHECK(row.starts_with(term.filterLabel + "FILTER-START!"));
			CHECK(!has(row, "FILTER-TAIL!") && !row.ends_with("z"));
			begin=term.output.size(); term.send("\x05");
			end=term.waitFor("FILTER-TAIL!",begin);
			term.waitFor("\033[K\r",end);
			begin=term.output.size(); term.resize(18,55);
			CHECK(has(queryFrame(term,begin,"FILTER-TAIL!",true),term.filterLabel));
			begin=term.output.size(); term.resize(18,95);
			CHECK(has(queryFrame(term,begin,"FILTER-TAIL!",true),term.filterLabel));
			frame=workflowPreview(term,"\x15zzzz-no-fixture","zzzz-no-fixture");
			CHECK(has(frame,"No filter results"));

		}

		void checkWorkflowFilterPreview(TerminalSession& term) {

			// Stage a selection before preview; cancellation must retain it.
			CHECK(has(workflowStep(term, "1;\n",selectPrompt),"Pending Indices"));
			workflowStep(term, "/",filterPrompt);
			auto begin=term.output.size(); term.send("groupalpha");
			auto frame=queryFrame(term,begin,"groupalpha");
			CHECK(has(frame,"groupalpha1.iso") && !has(frame,"groupbeta1.iso"));
			frame=workflowPreview(term, "\x1b[6~","groupalpha");
			CHECK(has(frame,"groupalpha3.iso") && !has(frame,"groupalpha1.iso"));
			frame=workflowPreview(term, "\x1b[6~","groupalpha"); CHECK(has(frame,"groupalpha5.iso"));
			frame=workflowPreview(term, "\x1b[6~","groupalpha"); CHECK(has(frame,"groupalpha5.iso"));
			frame=workflowPreview(term, "\x1b[5~","groupalpha"); CHECK(has(frame,"groupalpha3.iso"));
			begin=term.output.size(); term.resize(18,95);
			frame=queryFrame(term,begin,"groupalpha"); CHECK(has(frame,"groupalpha3.iso"));
			// Changing query resets scroll; widening must use original source.
			frame=workflowPreview(term, "\x15groupbeta","groupbeta");
			CHECK(has(frame,"groupbeta1.iso") && !has(frame,"groupalpha1.iso"));
			frame=workflowPreview(term, "\x15zzzz-no-fixture","zzzz-no-fixture");
			CHECK(has(frame,"No filter results"));
			checkLongFilterInput(term);
			// No-match Enter must leave us in the filter prompt.
			const auto label = term.filterLabel;
			workflowStep(term, "\n",filterPrompt);
			CHECK(term.filterLabel == label);
			CHECK(has(workflowStep(term, "\x1b",selectPrompt),"Pending Indices"));

		}

		void checkWorkflowCommittedFilters(TerminalSession& term) {

			// Commit a non-overlapping filter, then replace it from the source.
			workflowStep(term, "/",filterPrompt); auto frame=workflowPreview(term, "groupbeta","groupbeta");
			CHECK(has(frame,"groupbeta1.iso"));
			frame=workflowStep(term, "\n",selectPrompt); CHECK(has(frame,"groupbeta1.iso") && !has(frame,"groupalpha1.iso"));
			workflowStep(term, "/",filterPrompt); frame=workflowPreview(term, "groupalpha","groupalpha");
			CHECK(has(frame,"groupalpha1.iso"));
			frame=workflowStep(term, "\n",selectPrompt); CHECK(has(frame,"groupalpha1.iso") && !has(frame,"groupbeta1.iso"));
			CHECK(has(frame,"Pending Indices"));
			checkWorkflowPage(term, "\x1b[6~","Page 2/3","groupalpha3.iso","groupalpha1.iso");
			workflowStep(term, "\x1b",selectPrompt); // clear committed filter

		}

		void checkWorkflowCopy(TerminalSession& term, const fs::path& source, const fs::path& dest) {

			// Execute the original staged path, regardless of filter history.
			workflowStep(term, "P",inputPrompt);
			auto begin=term.output.size(); term.send(dest.string()+"\n");
			auto end=waitForNewInput(term, begin);
			CHECK(has(plain(std::string_view(term.output).substr(begin,end-begin)),"COMPLETED"));
			begin=term.output.size(); term.resize(20,110);
			end=waitForRepaintedInput(term, begin);
			CHECK(has(plain(std::string_view(term.output).substr(begin,end-begin)),"COMPLETED"));
			auto frame=workflowStep(term, "y\n",pausePrompt);
			CHECK(has(frame,"Copied:") && has(frame,"groupalpha1.iso"));
			CHECK(!has(frame,"groupalpha2.iso"));
			begin=term.output.size(); term.resize(24,120);
			end=term.waitFor(terminal_sync::pause,begin);
			frame=listFrame(term,begin,end);
			CHECK(has(frame,"Copied:") && has(frame,"groupalpha1.iso"));
			CHECK(readFile(dest/"groupalpha1.iso")==readFile(source/"groupalpha1.iso"));
			frame=workflowStep(term, "\n",selectPrompt);
			CHECK(!has(frame,"Copied:"));

		}

		void checkWorkflowDelete(TerminalSession& term, const fs::path& source) {

			// Return to menu; exercise delete rejection followed by confirmation.
			workflowStep(term, "\x1b",inputPrompt);
			workflowStep(term, "3\n",selectPrompt);
			workflowStep(term, "1\n",inputPrompt);
			workflowStep(term, "n\n",selectPrompt); CHECK(fs::exists(source/"groupalpha1.iso"));
			workflowStep(term, "1\n",inputPrompt);
			const auto begin=term.output.size(); term.send("y\n"); waitForNewInput(term, begin);
			auto frame=workflowStep(term, "Y\n",pausePrompt);
			CHECK(has(frame,"Deleted:") && has(frame,"groupalpha1.iso") && !has(frame,"Copied:"));
			frame=workflowStep(term, "\x1b",selectPrompt);
			CHECK(!has(frame,"Deleted:") && !fs::exists(source/"groupalpha1.iso"));
			CHECK(fs::exists(source/"groupalpha2.iso"));

		}

		void checkWorkflowMove(TerminalSession& term, const fs::path& source, const fs::path& dest) {

			// The source directory sorts before the destination; the deleted
			// first row is now replaced by alpha2. Move it to the fixture dir.
			workflowStep(term, "\x1b",inputPrompt);
			auto frame=workflowStep(term, "4\n",selectPrompt); CHECK(has(frame,"groupalpha2.iso"));
			workflowStep(term, "1\n",inputPrompt);
			const auto begin=term.output.size(); term.send(dest.string()+"\n");
			waitForNewInput(term, begin);
			frame=workflowStep(term, "y\n",pausePrompt);
			CHECK(has(frame,"Moved:") && has(frame,"groupalpha2.iso") && !has(frame,"Deleted:") && !has(frame,"Copied:"));
			frame=workflowStep(term, "\x04",selectPrompt);
			CHECK(!has(frame,"Moved:") && !fs::exists(source/"groupalpha2.iso"));
			CHECK(readFile(dest/"groupalpha2.iso")=="fixture payload: groupalpha2.iso");
			CHECK(fs::exists(source/"groupalpha3.iso"));

		}

		void finishWorkflow(TerminalSession& term) {

			workflowStep(term, "\x1b",inputPrompt); workflowStep(term, "\x1b",inputPrompt);
			// Observe the exit action's clear-screen sequence before waiting
			// for shutdown, distinguishing lost input from a cleanup hang.
			const auto begin=term.output.size(); term.send("5\n");
			term.waitFor("\033[2J\033[3J\033[H",begin);
			term.waitForExit();

		}

	}

	int workflowTest(const char* binary) {

		try {
			const char* sandbox=std::getenv("ISOCMD_TEST_SANDBOX");
			const char* home=std::getenv("HOME");
			CHECK(sandbox && home && fs::path(home)==fs::path(sandbox)/"home");
			TempDir fixture;
			const auto source=fixture.path/"aa-source", dest=fixture.path/"zz-destination";
			fs::create_directories(source); fs::create_directories(dest);
			std::string database;
			for (const std::string name : {"groupalpha1.iso","groupalpha2.iso","groupalpha3.iso","groupalpha4.iso",
				                           "groupalpha5.iso","groupalpha6.iso","groupbeta1.iso","groupbeta2.iso"}) {
				writeFile(source/name,"fixture payload: "+name);
				database+=(source/name).string()+"\n";
			}
			const auto db=fs::path(home)/".local/share/isocmd/database/iso_commander_database.txt";
			const auto config=fs::path(home)/".config/isocmd/config";
			fs::create_directories(db.parent_path()); fs::create_directories(config.parent_path());
			writeFile(db,database); writeFile(config,"pagination = 2\nauto_update = off\nfilenames_only = off\n");
			TerminalSession term(binary,nullptr,120);
			try {
				checkWorkflowPagination(term);
				checkWorkflowFilterPreview(term);
				checkWorkflowCommittedFilters(term);
				checkWorkflowCopy(term, source, dest);
				checkWorkflowDelete(term, source);
				checkWorkflowMove(term, source, dest);
				finishWorkflow(term);
			} catch (...) { printOutput(term.output); throw; }
			std::cout<<"PASS: real menu pagination, filter preview, staged copy, move and delete workflows\n";
			return 0;
		} catch(const std::exception& e) { std::cerr<<"FAIL: interactive workflows: "<<e.what()<<'\n'; return 1; }

	}

	namespace {
		// Each extended workflow runs in a fresh runner sandbox and launches
		// the real menu binary. Never point destructive commands at user data.
		fs::path workflowHome() {

			const char* sandbox = std::getenv("ISOCMD_TEST_SANDBOX");
			const char* home = std::getenv("HOME");
			CHECK(sandbox && home && ::geteuid() != 0);
			CHECK(fs::path(home) == fs::path(sandbox)/"home");
			CHECK(fs::is_directory(home) && !fs::is_symlink(home));
			return home;

		}

		constexpr auto mainPrompt = inputPrompt;
		constexpr auto selectionPrompt = inputPrompt;
		constexpr auto valuePrompt = inputPrompt;
		constexpr auto continuePrompt = pausePrompt;
		constexpr auto returnPrompt = pausePrompt;

		std::string step(TerminalSession& term, std::string_view input, Prompt marker) {

			const auto start = term.output.size();
			term.send(input);
			const auto end = waitForStepPrompt(term, marker, start);
			return plain(std::string_view(term.output).substr(start, end-start));

		}

		void expectStep(TerminalSession& term, std::string_view input,
		                Prompt marker, std::string_view expected) {

			const auto frame = step(term, input, marker);
			if (!has(frame, expected))
			    throw std::runtime_error("expected '" + std::string(expected) +
			        "' before the next input wait");

		}

		template<class Workflow>
		void runMenuWorkflow(const char* binary, Workflow workflow) {

			TerminalSession term(binary, nullptr, 120);
			try {
				CHECK(has(listFrame(term, 0, waitForNewInput(term)), "Main Menu"));
				workflow(term);
				term.send("5\n");
				term.waitForExit();
			} catch (...) { printOutput(term.output); throw; }

		}

		std::vector<std::string> fileLines(const fs::path& file) {

			std::vector<std::string> lines;
			std::istringstream stream(readFile(file));
			for (std::string line; std::getline(stream, line);)
			    if (!line.empty()) lines.push_back(line);
			std::sort(lines.begin(), lines.end());
			return lines;

		}
	}

	int settingsWorkflowTest(const char* binary) {

		try {
			const auto home = workflowHome();
			const auto config = home/".config/isocmd/config";
			fs::create_directories(config.parent_path());
			writeFile(config, "pagination = 2\nauto_update = off\nfilenames_only = off\n");
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				const auto frame = step(term, "4\n", selectionPrompt);
				CHECK(has(frame, "Settings Editor"));
				CHECK(has(frame, "--- Write2USB Settings ---"));
				const auto original = readFile(config);
				expectStep(term, "?", returnPrompt, "Selecting Edits");
				step(term, "\n", selectionPrompt);
				// Select two settings; navigate without applying anything.
				expectStep(term, "5 6\n", valuePrompt, "Setting: filenames_only");
				expectStep(term, "\x1b[6~", valuePrompt, "Setting: pagination");
				expectStep(term, "\x1b[5~", valuePrompt, "Setting: filenames_only");
				expectStep(term, "g2\n", valuePrompt, "Setting: pagination");
				expectStep(term, "1001\n", pausePrompt, "Invalid value:");
				CHECK(readFile(config) == original);
				expectStep(term, "\n", valuePrompt, "Current: 2");
				// Resize with a typed value, then submit the preserved buffer.
				const auto typed = term.output.size();
				term.send("3"); waitForInputRow(term, term.waitFor("3", typed));
				const auto resizeStart = term.output.size();
				term.resize(20, 100);
				const auto end = waitForRepaintedInput(term, resizeStart);
				CHECK(inputRow(term, end).ends_with("3"));
				expectStep(term, "\n", continuePrompt, "Updated and saved to:");
				CHECK(has(readFile(config), "pagination = 3\n"));
				expectStep(term, "\n", valuePrompt, "Setting: filenames_only");
				// Skip the remaining batch item; no change to its value.
				step(term, "\n", selectionPrompt);
				CHECK(has(readFile(config), "filenames_only = off\n"));
				step(term, "5\n", valuePrompt);
				step(term, "\x1b", selectionPrompt);
				CHECK(has(readFile(config), "filenames_only = off\n"));
				step(term, "r", inputPrompt);
				step(term, "n\n", selectionPrompt);
				CHECK(has(readFile(config), "pagination = 3\n"));
				CHECK(has(readFile(config), "verify_checksum = off\n"));
				expectStep(term, "7\n", valuePrompt, "Setting: verify_checksum");
				expectStep(term, "yes\n", pausePrompt, "Invalid value:");
				CHECK(has(readFile(config), "verify_checksum = off\n"));
				step(term, "\n", valuePrompt);
				expectStep(term, "on\n", continuePrompt, "Updated and saved to:");
				CHECK(has(readFile(config), "verify_checksum = on\n"));
				step(term, "\n", selectionPrompt);
				CHECK(has(readFile(config), "buffered_writes = on\n"));
				expectStep(term, "8\n", valuePrompt, "Setting: buffered_writes");
				expectStep(term, "off\n", continuePrompt, "Updated and saved to:");
				CHECK(has(readFile(config), "buffered_writes = off\n"));
				step(term, "\n", selectionPrompt);
				expectStep(term, "14\n", valuePrompt, "Setting: write2usb_list");
				expectStep(term, "full\n", continuePrompt, "Updated and saved to:");
				CHECK(has(readFile(config), "write2usb_list = full\n"));
				step(term, "\n", selectionPrompt);
				step(term, "\x1b", mainPrompt);

			});
			// New process must load the persisted value, not just the old cache.
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				step(term, "4\n", selectionPrompt);
				expectStep(term, "6\n", valuePrompt, "Current: 3");
				step(term, "\x1b", selectionPrompt);
				expectStep(term, "7\n", valuePrompt, "Current: on");
				step(term, "\x1b", selectionPrompt);
				expectStep(term, "8\n", valuePrompt, "Current: off");
				step(term, "\x1b", selectionPrompt);
				expectStep(term, "14\n", valuePrompt, "Current: full");
				step(term, "\x1b", selectionPrompt);
				step(term, "r", inputPrompt);
				expectStep(term, "y\n", continuePrompt, "Defaults applied");
				CHECK(has(readFile(config), "pagination = 25\n"));
				CHECK(has(readFile(config), "buffered_writes = on\n"));
				CHECK(has(readFile(config), "verify_checksum = off\n"));
				CHECK(has(readFile(config), "write2usb_list = compact\n"));
				step(term, "\n", selectionPrompt);
				expectStep(term, "6\n", valuePrompt, "Current: 25");
				step(term, "\x1b", selectionPrompt);
				step(term, "\x1b", mainPrompt);

			});
			std::cout << "PASS: interactive settings batch navigation, validation, resize, persistence and reset\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: interactive settings: " << e.what() << '\n'; return 1;
		}

	}

	namespace {
		// Use raw PTY output: stripping CSI would hide stale or incorrect palettes.
		std::string rawStep(TerminalSession& term, std::string_view input, Prompt marker) {

			const auto start = term.output.size();
			term.send(input);
			const auto end = waitForStepPrompt(term, marker, start);
			return term.output.substr(start, end-start);

		}

		struct Appearance {
			std::string_view accent, settingsAccent, prompt, skin, base;
		};

		constexpr Appearance originalAppearance{
			"\033[1;38;2;235;50;245m", "\033[1;38;2;0;115;215m", "\033[1;38;2;0;115;215m",
			"\033[1;38;2;222;222;222m", "\033[1;38;2;222;222;222m"};
		constexpr Appearance draculaAppearance{
			"\033[1;38;2;80;250;123m", "\033[1;38;2;80;250;123m", "\033[1;38;2;110;128;185m",
			"\033[1;38;2;86;182;194m", "\033[1;38;2;106;128;154m"};
		constexpr Appearance oceanAppearance{
			"\033[1;38;2;130;225;190m", "\033[1;38;2;130;225;190m", "\033[1;38;2;110;140;175m",
			"\033[1;38;2;86;182;194m", "\033[1;38;2;106;128;154m"};

		void checkMenuAppearance(std::string_view frame, const Appearance& a) {

			CHECK(has(frame, std::string(a.skin)+"+----------------------+"));
			CHECK(has(rawInputRow(frame, frame.size()), a.prompt));

		}

		void checkListAppearance(std::string_view frame, const Appearance& a, std::size_t cols) {

			const auto row = frame.find(std::string(a.accent)+"theme-fixture.iso");
			CHECK(row != std::string_view::npos);
			const auto end = frame.find('\n', row);
			CHECK(end != std::string_view::npos);
			const auto lineStart = frame.rfind('\n', row);
			CHECK(lineStart != std::string_view::npos);
			CHECK(has(frame.substr(row, end-row), std::string("\033[0m")+std::string(a.base)));
			const auto line = plain(frame.substr(lineStart+1, end-lineStart-1));
			CHECK(visibleWidth(line) < cols);
			if (cols == 80) CHECK(has(line, "…"));
			CHECK(has(rawInputRow(frame, frame.size()), a.prompt));

		}

		void checkAppearance(TerminalSession& term, const Appearance& a) {

			auto frame = rawStep(term, "1\n", inputPrompt);
			CHECK(has(frame, "ManageISO"));
			checkMenuAppearance(frame, a);
			frame = rawStep(term, "5\n", selectionPrompt);
			checkListAppearance(frame, a, 120);
			const auto start = term.output.size(); term.resize(20, 80);
			const auto end = waitForRepaintedInput(term, start);
			checkListAppearance(std::string_view(term.output).substr(start, end-start), a, 80);
			const auto wideStart = term.output.size(); term.resize(24, 120);
			const auto wideEnd = waitForRepaintedInput(term, wideStart);
			checkListAppearance(std::string_view(term.output).substr(wideStart, wideEnd-wideStart), a, 120);
			step(term, "\x1b", inputPrompt);
			frame = rawStep(term, "\x1b", mainPrompt);
			CHECK(has(frame, "Main Menu"));
			checkMenuAppearance(frame, a);

		}

		void setAppearanceValue(TerminalSession& term, const fs::path& config,
		                        std::string_view index, std::string_view key, std::string_view value) {

			expectStep(term, std::string(index)+"\n", valuePrompt, std::string("Setting: ")+std::string(key));
			expectStep(term, std::string(value)+"\n", continuePrompt, "Updated and saved to:");
			CHECK(has(readFile(config), std::string(key)+" = "+std::string(value)+"\n"));
			step(term, "\n", selectionPrompt);

		}

		void enterAppearanceSettings(TerminalSession& term, const Appearance& a) {

			const auto frame = rawStep(term, "4\n", selectionPrompt);
			CHECK(has(frame, std::string(a.settingsAccent)+"=== Settings Editor ==="));

		}
	}

	int themeWorkflowTest(const char* binary) {

		try {
			const auto home = workflowHome();
			const auto config = home/".config/isocmd/config";
			const auto db = home/".local/share/isocmd/database/iso_commander_database.txt";
			TempDir fixture;
			const auto directory = fixture.path/std::string(100, 'a');
			fs::create_directories(directory);
			const auto iso = directory/"theme-fixture.iso";
			writeFile(iso, "appearance fixture");
			fs::create_directories(config.parent_path()); fs::create_directories(db.parent_path());
			writeFile(config, "base = white\nskin = white\ntheme = original\nauto_update = off\nfilenames_only = off\ncp_mv_rm_list = full\n");
			writeFile(db, iso.string()+"\n");
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				checkAppearance(term, originalAppearance);
				enterAppearanceSettings(term, originalAppearance);
				setAppearanceValue(term, config, "1", "base", "steel");
				setAppearanceValue(term, config, "2", "skin", "cyan");
				setAppearanceValue(term, config, "3", "theme", "dracula");
				step(term, "\x1b", mainPrompt);
				checkAppearance(term, draculaAppearance);
				enterAppearanceSettings(term, draculaAppearance);
				setAppearanceValue(term, config, "3", "theme", "ocean");
				step(term, "\x1b", mainPrompt);
				checkAppearance(term, oceanAppearance);

			});
			// A fresh process must reproduce the selected palette before any edit.
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				checkAppearance(term, oceanAppearance);
				enterAppearanceSettings(term, oceanAppearance);
				step(term, "r", inputPrompt);
				expectStep(term, "y\n", continuePrompt, "Defaults applied");
				const auto saved = readFile(config);
				CHECK(has(saved, "base = white\n") && has(saved, "skin = white\n") && has(saved, "theme = original\n"));
				step(term, "\n", selectionPrompt);
				step(term, "\x1b", mainPrompt);
				checkAppearance(term, originalAppearance);

			});
			CHECK(readFile(iso) == "appearance fixture");
			std::cout << "PASS: interactive theme, skin and base switching, ANSI palette, resize, persistence and reset\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: interactive appearance: " << e.what() << '\n'; return 1;
		}

	}

	int importWorkflowTest(const char* binary) {

		try {
			const auto home = workflowHome();
			const auto dbDir = home/".local/share/isocmd/database";
			const auto db = dbDir/"iso_commander_database.txt";
			const auto paths = dbDir/"iso_commander_path_database.txt";
			const auto filters = dbDir/"iso_commander_filter_database.txt";
			fs::create_directories(dbDir);
			writeFile(db, ""); writeFile(paths, ""); writeFile(filters, "fixture-filter\n");
			TempDir fixture;
			const auto source = fixture.path/"import-source";
			const auto nested = source/"nested";
			const auto missing = fixture.path/"missing-directory";
			fs::create_directories(nested);
			writeFile(source/"alpha.iso", "alpha");
			writeFile(nested/"beta.ISO", "beta");
			writeFile(source/"ignored.txt", "ignore");
			const std::vector<std::string> expected = {(source/"alpha.iso").string(), (nested/"beta.ISO").string()};
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				step(term, "3\n", inputPrompt);
				expectStep(term, "?\n", returnPrompt, "Cleanup/Display Commands");
				step(term, "\n", inputPrompt);
				expectStep(term, missing.string()+"\n", continuePrompt, "Lack of valid paths");
				CHECK(readFile(db).empty() && readFile(paths).empty());
				step(term, "\n", inputPrompt);
				const auto input = source.string()+";"+nested.string()+";"+missing.string()+"\n";
				auto frame = step(term, input, continuePrompt);
				CHECK(has(frame, "2 ISO imported") && has(frame, "Invalid paths omitted from search:"));
				CHECK(has(frame, missing.string()));
				CHECK(fileLines(db) == expected);
				CHECK(fileLines(paths) == std::vector<std::string>{source.string()+";"+nested.string()});
				CHECK(!has(readFile(paths), missing.string()));
				// Repaint the report; must not re-import or duplicate history.
				const auto start = term.output.size(); term.resize(20, 100);
				const auto end = term.waitFor(terminal_sync::pause, start);
				CHECK(has(plain(std::string_view(term.output).substr(start, end-start)), "2 ISO imported"));
				CHECK(fileLines(db) == expected);
				step(term, "\n", inputPrompt);
				expectStep(term, source.string()+"\n", continuePrompt, "No new ISO found");
				CHECK(fileLines(db) == expected);
				step(term, "\n", inputPrompt);
				frame = step(term, "*stats\n", returnPrompt);
				CHECK(has(frame, "=== ISO Database ===") && has(frame, "Entries: 2"));
				step(term, "\n", inputPrompt);
				expectStep(term, "!clr_paths\n", continuePrompt, "FolderPath database cleared successfully.");
				CHECK(readFile(paths).empty() && fileLines(db) == expected);
				CHECK(readFile(filters) == "fixture-filter\n");
				step(term, "\n", inputPrompt);
				expectStep(term, "!clr_filter\n", continuePrompt, "FilterTerm database cleared successfully.");
				CHECK(readFile(filters).empty() && fileLines(db) == expected);
				step(term, "\n", inputPrompt);
				expectStep(term, "!clr\n", continuePrompt, "ISO database cleared successfully.");
				CHECK(readFile(db).empty());
				step(term, "\n", inputPrompt);
				step(term, "\x1b", mainPrompt);

			});
			CHECK(readFile(db).empty() && readFile(paths).empty() && readFile(filters).empty());
			CHECK(readFile(source/"alpha.iso") == "alpha" && readFile(nested/"beta.ISO") == "beta");
			std::cout << "PASS: interactive import, duplicate scan, statistics, history and database cleanup\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: interactive import: " << e.what() << '\n'; return 1;
		}

	}

	namespace {
		void discoverConversionImages(TerminalSession& term, const fs::path& source) {

			expectStep(term, "2\n", mainPrompt, "Convert2ISO");
			step(term, "1\n", inputPrompt);
			expectStep(term, "ls\n", continuePrompt, "entries to display.");
			step(term, "\n", inputPrompt);
			auto frame = step(term, source.string()+"\n", continuePrompt);
			CHECK(has(frame, "3 {.bin/.img} files found"));
			frame = step(term, "\n", selectionPrompt);
			CHECK(has(frame, "alpha.img") && has(frame, "beta.img") && has(frame, "broken.img"));

		}

		void stageFilteredConversion(TerminalSession& term) {

			expectStep(term, "1;\n", selectionPrompt, "Pending Indices");
			auto frame = step(term, "clr\n", selectionPrompt);
			CHECK(!has(frame, "Pending Indices"));
			expectStep(term, "1;\n", selectionPrompt, "Pending Indices");
			step(term, "/", filterPrompt);
			const auto start = term.output.size(); term.send("beta");
			frame = queryFrame(term, start, "beta");
			CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));
			frame = step(term, "\n", selectionPrompt);
			CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));

		}

		void checkSuccessfulConversions(TerminalSession& term, const fs::path& source, const std::string& alpha, const std::string& beta) {

			// Execute the staged source index while another image is visible.
			expectStep(term, "P", inputPrompt, "COMPLETED");
			auto frame = step(term, "y\n", continuePrompt);
			CHECK(has(frame, "Convert2ISO:") && has(frame, "alpha.iso") && has(frame, "→ ISO."));
			CHECK(!has(frame, "beta.iso") && !has(frame, "Failed."));
			frame = step(term, "\n", selectionPrompt);
			CHECK(has(frame, "beta.img") && !has(frame, "alpha.img") && !has(frame, "→ ISO."));
			CHECK(readFile(source/"alpha.iso") == alpha);
			CHECK(!fs::exists(source/"beta.iso") && !fs::exists(source/"broken.iso"));
			expectStep(term, "1\n", inputPrompt, "COMPLETED");
			frame = step(term, "n\n", selectionPrompt);
			CHECK(!has(frame, "→ ISO."));
			CHECK(readFile(source/"beta.iso") == beta);
			step(term, "\x1b", selectionPrompt);

		}

		void checkFailedConversion(TerminalSession& term, const fs::path& source, const fs::path& db) {

			expectStep(term, "3\n", inputPrompt, "FAILED");
			const auto frame = step(term, "Y\n", continuePrompt);
			CHECK(has(frame, "broken.img") && has(frame, "Failed.") && !has(frame, "→ ISO."));
			step(term, "\x1b", selectionPrompt);
			CHECK(!fs::exists(source/"broken.iso"));
			CHECK(fileLines(db) == (std::vector<std::string>{(source/"alpha.iso").string(), (source/"beta.iso").string()}));

		}

		void checkSkippedConversion(TerminalSession& term, const fs::path& source) {

			// Existing output is a skip and must never be overwritten.
			writeFile(source/"alpha.iso", "keep existing ISO");
			expectStep(term, "1\n", inputPrompt, "COMPLETED");
			const auto frame = step(term, "y\n", continuePrompt);
			CHECK(has(frame, "alpha.img") && has(frame, "ISOalrExists → Skipped."));
			CHECK(!has(frame, "Failed.") && !has(frame, "→ ISO."));
			step(term, "\x04", selectionPrompt);
			CHECK(readFile(source/"alpha.iso") == "keep existing ISO");

		}

		void checkConversionCache(TerminalSession& term) {

			step(term, "\x1b", inputPrompt);
			const auto frame = step(term, "ls\n", selectionPrompt);
			CHECK(has(frame, "alpha.img") && has(frame, "beta.img"));
			step(term, "\x1b", inputPrompt);
			expectStep(term, "!clr\n", continuePrompt, "BIN/IMG cache cleared.");
			step(term, "\n", inputPrompt);
			expectStep(term, "ls\n", continuePrompt, "entries to display.");
			step(term, "\n", inputPrompt);
			step(term, "\x1b", mainPrompt); // Convert2ISO submenu
			step(term, "\x1b", mainPrompt); // main menu

		}

	}

	int conversionWorkflowTest(const char* binary) {

		try {
			const auto home = workflowHome();
			const auto db = home/".local/share/isocmd/database/iso_commander_database.txt";
			TempDir fixture;
			const auto source = fixture.path/"images";
			fs::create_directories(source);
			const std::string alpha(2048, 'A'), beta(2048, 'B');
			auto rawSector = [](const std::string& payload, char mode) {

				std::string data(2352, '\0'); data[15] = mode;
				data.replace(16, payload.size(), payload); return data;

			};
			writeFile(source/"alpha.img", rawSector(alpha, 1));
			writeFile(source/"beta.img", rawSector(beta, 1));
			writeFile(source/"broken.img", rawSector(alpha, 3));
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				discoverConversionImages(term, source);
				stageFilteredConversion(term);
				checkSuccessfulConversions(term, source, alpha, beta);
				checkFailedConversion(term, source, db);
				checkSkippedConversion(term, source);
				checkConversionCache(term);

			});
			CHECK(readFile(source/"alpha.img") == rawSector(alpha, 1));
			CHECK(readFile(source/"beta.img") == rawSector(beta, 1));
			CHECK(readFile(source/"broken.img") == rawSector(alpha, 3));
			std::cout << "PASS: interactive image discovery, staged/filtered conversion, failure, skip and RAM cache\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: interactive conversion: " << e.what() << '\n'; return 1;
		}

	}

	static void checkWriteElapsed(const std::string& frame, int i) {

		// The slowest device takes 200 ms to write and another 400 ms
		// to verify, even when verification fails. The summary must
		// not stop at write completion or another device finishing.
		const std::string elapsedLabel="Time Elapsed: ";
		const auto elapsedPos=frame.find(elapsedLabel);
		CHECK(elapsedPos!=std::string::npos);
		const double elapsed=std::stod(frame.substr(elapsedPos+elapsedLabel.size()));
		CHECK(elapsed >= (i<2 ? 0.6 : 0.2));

	}

	static void checkWriteSpeed(const std::string& frame, int i) {

		if (i==0) CHECK(has(frame,"PASS") && has(frame,"20.0 MB/s (avg)") && !has(frame,"100.0 MB/s (avg)"));
		if (i==3) CHECK(has(frame,"DONE") && !has(frame,"PASS") && has(frame,"20.0 MB/s (avg)"));

	}

	static void checkWriteResult(const std::string& frame, int i) {

		const bool successful=i==0 || i==3;
		CHECK(has(frame,successful?"COMPLETED":i==1?"PARTIAL":"FAILED"));
		CHECK(has(frame,i==2?"Written: 0/3":"Written: 3/3"));
		if (i==3) CHECK(!has(frame,"Verified:"));
		else CHECK(has(frame,i==0?"Verified: 3/3":i==1?"Verified: 2/3":"Verified: 0/3"));
		checkWriteElapsed(frame, i);
		CHECK(has(frame,"iso2.iso") && has(frame,"iso10.iso"));
		checkWriteSpeed(frame, i);
		CHECK(has(frame,"/dev/full") && has(frame,"/dev/zero") && has(frame,"/dev/null"));

	}

	int writeUiTest(const char* binary) {

		try {
			TerminalSession term(binary,"--write-ui-child",120);
			try {
				auto end=waitForNewInput(term);
				CHECK(has(listFrame(term, 0, end), "iso2.iso"));
				// One Tab completes a unique index and the common device prefix.
				term.send("1\t");
				end=waitForInputRow(term, term.waitFor("1>/dev/",end));
				CHECK(inputRow(term,end).ends_with("1>/dev/"));
				term.send("z\t");
				end=waitForInputRow(term, term.waitFor("1>/dev/zero",end));
				CHECK(inputRow(term,end).ends_with("1>/dev/zero"));
				// Exactly one ISO and device remain: one Tab completes the entire pair.
				term.send(";\t");
				end=waitForInputRow(term, term.waitFor("1>/dev/zero;2>/dev/null",end));
				CHECK(inputRow(term,end).ends_with("1>/dev/zero;2>/dev/null"));
				term.send("\x1b");
				end=term.waitFor("TEST-MAPPING-CANCELLED",end);
				end=waitForNewInput(term, end);
				term.send("1>/dev/null;1>/dev/null\n");
				end=term.waitFor("used multiple times",end); end=term.waitFor(terminal_sync::pause,end);
				term.send("\n"); end=waitForNewInput(term, end);
				const std::string mapping="1>/dev/zero;2>/dev/null;1>/dev/full";
				term.send(mapping+"\n"); end=waitForNewInput(term, end);
				const auto start=term.output.size(); term.resize(24,100);
				end=waitForRepaintedInput(term, start);
				term.send("n\n"); end=waitForNewInput(term, end);
				// Declining retains the input for editing; resubmit it unchanged.
				CHECK(inputRow(term, end).ends_with(mapping)); term.send("\n"); end=waitForNewInput(term, end);
				const auto afterConfirm=term.output.size(); term.send("y\n");
				for (int i=0;i<4;++i) {
					const auto startResult=i==0?afterConfirm:end;
					end=term.waitFor("TEST-WRITE-RESULT-"+std::to_string(i),startResult);
					const auto frame=plain(std::string_view(term.output).substr(startResult,end-startResult));
					checkWriteResult(frame, i);
					term.send("\n");
				}
				end=term.waitFor("TEST-ASYNC-CANCELLATION-PASS",end);
				end=term.waitFor("TEST-VERIFY-OFF",end);
				end=waitForNewInput(term, end);
				term.send("1>/dev/zero;2>/dev/null\n"); end=waitForNewInput(term, end);
				term.send("y\n");
				term.waitFor("TEST-WRITE-UI-PASS",end); term.waitForExit();
				// Exactly eight line-editor sessions: two mapping attempts, retry,
				// confirmation, decline/retry, confirmation, and the final pair.
				std::size_t sessions = 0;
				for (std::size_t pos = 0; (pos = term.output.find(terminal_sync::input, pos)) != std::string::npos;
				     pos += std::string_view(terminal_sync::input).size()) ++sessions;
				CHECK(sessions == 8);
			} catch (...) { printOutput(term.output); throw; }
			std::cout<<"PASS: device mapping retry, resize, confirmation, multi-device task association and results\n";
			return 0;
		} catch(const std::exception& e) { std::cerr<<"FAIL: USB write UI: "<<e.what()<<'\n'; return 1; }

	}

	// Original layout: one space after the bar, a left-aligned
	// five-character status field, then right-aligned speed values.
	static void checkSpeedSpacing(const std::string& text) {

		std::istringstream rows(text); std::string row;
		std::size_t count = 0;
		std::size_t speedEndColumn = 0;
		while (std::getline(rows, row)) {
			if (!has(row,"/dev/")) continue;
			auto unit = row.find(" MB/s");
			if (unit == std::string::npos) unit = row.find(" KB/s");
			CHECK(unit != std::string::npos);
			const auto barEnd = row.rfind(']', unit);
			CHECK(barEnd != std::string::npos);
			const auto statusStart = row.find_first_not_of(' ', barEnd + 1);
			CHECK(statusStart == barEnd + 2);
			const auto statusEnd = row.find(' ', statusStart);
			CHECK(statusEnd != std::string::npos && statusEnd <= statusStart + 4);
			const auto speedStart = row.find_first_not_of(' ', statusEnd);
			CHECK(speedStart != std::string::npos && speedStart >= statusStart + 5);
			CHECK(speedStart < unit);
			CHECK(row[speedStart] >= '0' && row[speedStart] <= '9');
			const auto column = visibleWidth(row.substr(0, unit + 5));
			if (count == 0) speedEndColumn = column;
			else CHECK(column == speedEndColumn);
			++count;
		}
		CHECK(count == 3);

	}

	static void checkInitialProgress(const std::string& frame) {

		CHECK(has(frame,"▄░░░░") && has(frame,"[⣶⣶⡆  ]") && has(frame,"▇▇▇▇▄"));
		CHECK(has(frame,"10%") && has(frame,"] 50%") && !has(frame,"CHK") && has(frame,"99%") && has(frame,"12.3 MB/s"));
		CHECK(has(frame,"very-long-fixture-image-for-resize-testing.iso"));
		checkSpeedSpacing(frame);

	}

	int progressScreenTest(const char* binary) {

		try {
			TerminalSession term(binary,"--progress-child",120);
			try {
				std::size_t start=0;
				auto frame=screenFrame(term,start,"51.2 KB/s");
				checkInitialProgress(frame);
				start=term.output.size(); term.resize(20,55);
				frame=screenFrame(term,start,"Processing");
				CHECK(!has(frame,"very-long-fixture-image-for-resize-testing.iso") && has(frame,"…"));
				std::istringstream lines(frame); std::string line;
				while (std::getline(lines,line)) if (has(line,"/dev/")) CHECK(visibleWidth(line)<=54);
				start=term.output.size(); term.resize(24,120);
				frame=screenFrame(term,start,"very-long-fixture-image-for-resize-testing.iso");
				CHECK(has(frame,"99%"));
				checkSpeedSpacing(frame);
				term.send("\n"); frame=screenFrame(term,start,"100% 179.8 MB/s");
				// The longest speed follows a full-width percentage by one space.
				CHECK(has(frame,"100% 179.8 MB/s") && has(frame,"] 5%"));
				checkSpeedSpacing(frame);
				term.send("\n"); frame=screenFrame(term,start,"INTERRUPTED");
				checkSpeedSpacing(frame);
				CHECK(has(frame,"Written: 3/3 | Verified: 1/3") && has(frame,"PASS") && has(frame,"FAIL") && has(frame,"] CXL  ") && has(frame,"(avg)"));
				CHECK(has(frame,"▇▇▇▇▇"));
				CHECK(has(frame,"51.2 KB/s (avg)") && !has(frame,"100.0 MB/s (avg)"));
				start=term.output.size(); term.resize(20,100);
				frame=screenFrame(term,start,"INTERRUPTED"); CHECK(has(frame,"Written: 3/3 | Verified: 1/3"));
				term.send("\n"); term.waitFor("TEST-PROGRESS-PASS",start); term.waitForExit();
			} catch (...) { printOutput(term.output); throw; }
			std::cout<<"PASS: live and final USB progress rows, glyphs, speeds, cancellation and terminal resizing\n";
			return 0;
		} catch(const std::exception& e) { std::cerr<<"FAIL: USB progress screen: "<<e.what()<<'\n'; return 1; }

	}
} // namespace isocmd_tests
