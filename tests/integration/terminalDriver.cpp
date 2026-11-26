// SPDX-License-Identifier: GPL-3.0-or-later

#include <errno.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../common/testHarness.h"

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
				// otherwise block Readline cleanup or sanitizer diagnostics indefinitely.
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

		// Strip CSI commands and Readline's prompt delimiters; keep UTF-8 glyphs.
		std::string plain(std::string_view raw) {

			std::string result;
			for (std::size_t i=0;i<raw.size();++i) {
				const unsigned char c=raw[i];
				if (c==27 && i+1<raw.size() && raw[i+1]=='[') {
					i+=2;
					while (i<raw.size() && !(raw[i]>=0x40 && raw[i]<=0x7e)) ++i;
				} else if (c!=1 && c!=2 && c!='\r') result+=raw[i];
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

		// Readline echoes the old prompt while typing. Wait for a complete new
		// screen before accepting the next prompt as a completed action.
		std::size_t waitForRedrawnPrompt(TerminalSession& term, std::string_view prompt, std::size_t start) {

			const auto begin = term.waitFor("\033[?2026h", start);
			const auto end = term.waitFor("\033[?2026l", begin);
			return term.waitFor(prompt, end);

		}

		// Menu titles and list contents are drawn before Readline starts the
		// next input row. A prompt match alone can still be the old input echo.
		std::size_t waitForStepPrompt(TerminalSession& term, std::string_view prompt, std::size_t start) {

			if (prompt == "Enter choice [1-5]: " || prompt == "Enter choice [1-6]: " ||
			    prompt == "? for help: " || prompt == "Skip")
			    return waitForRedrawnPrompt(term, prompt, start);

			// Folder search prompts have a multiline header but no initial
			// synchronized redraw. Wait for a new Readline session instead.
			if (prompt == "? for help:" || prompt == "DB, ? for help:" ||
			    prompt == "into, ? for help:") {
				const auto ready = term.waitFor("\033[?2004h", start);
				const auto header = term.waitFor(prompt, ready);
				return term.waitFor("\033[K\r", header);
			}
			return term.waitFor(prompt, start);

		}

		// Each preview repaint has its own synchronized list frame, followed by
		// navigation hints and the input row. Ignore prompt-only redisplays left
		// over from the previous key, even when they contain the same query.
		std::string queryFrame(TerminalSession& term, std::size_t start, std::string_view query,
		                       bool visibleTail = false) {

			for (;;) {
				const auto begin = term.waitFor("\033[?2026h", start);
				const auto painted = term.waitFor("\033[?2026l", begin);
				const auto prompt = term.waitFor("Filter: ", painted);
				const auto end = term.waitFor("\033[K\r", prompt);
				const auto input = plain(std::string_view(term.output).substr(prompt, end-prompt));
				if (visibleTail ? input.ends_with(query) : input == query)
				    return listFrame(term, begin, end);
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
		void checkSingleRowInput(std::string_view output) {

			CHECK(has(plain(output), "LONG> «")); // Overflow must not replace the label.
			CHECK(!has(output, "\n"));
			CHECK(!has(output, "\033[A") && !has(output, "\033[B"));
			CHECK(!has(plain(output), std::string(2400, 'b')));

		}

		void checkLongInput(TerminalSession& term, std::size_t start) {

			const auto prompt = term.waitFor("LONG> ", start);
			term.send("START-OF-INPUT" + std::string(160, 'a') + "TYPED-END!");
			const auto typed = term.waitFor("!", prompt);
			checkSingleRowInput(std::string_view(term.output).substr(prompt, typed - prompt));
			term.send("\033[200~" + std::string(2400, 'b') + "PASTE-END@\nmultiline\nPASTE-FINAL#\033[201~");
			const auto pasted = term.waitFor("#", typed);
			checkSingleRowInput(std::string_view(term.output).substr(typed, pasted - typed));
			// Resize with the cursor at the far end of the line, then edit its
			// hidden beginning and submit the entire, untruncated buffer.
			term.resize(18, 40);
			const auto repaint = term.waitFor("LONG-REPAINT", pasted);
			const auto restored = term.waitFor("#", repaint);
			term.resize(32, 120);
			const auto wider = term.waitFor("LONG-REPAINT", restored);
			const auto widened = term.waitFor("#", wider);
			term.send("\x01");
			const auto beginning = term.waitFor("START-OF-INPUT", widened);
			const auto right = term.waitFor("\033[K", beginning);
			CHECK(has(plain(std::string_view(term.output).substr(widened, right - widened)),
			          "LONG> START-OF-INPUT"));
			CHECK(has(plain(std::string_view(term.output).substr(beginning, right - beginning)), "»"));
			term.send("X\x05\n");
			term.waitFor("TEST-LONG-INPUT-PASS", beginning);

		}

		void checkOverflowSpace(TerminalSession& term, std::size_t start) {

			// The previous test leaves a 120-column terminal. With this 31-cell
			// prompt, the viewport starts on the space 85 cells from the end.
			const auto prompt = term.waitFor("Display verbose output? (y/n): ", start);
			term.send("\033[200~" + std::string(160, 'a') + " " + std::string(84, 'b') + "\033[201~");
			const auto tail = term.waitFor(std::string(84, 'b'), prompt);
			const auto frame = plain(std::string_view(term.output).substr(prompt, tail - prompt));
			CHECK(has(frame, "Display verbose output? (y/n): «" + std::string(84, 'b')));
			term.send("\x01X\x05\n");
			term.waitFor("TEST-OVERFLOW-SPACE-PASS", tail);

		}

	} // namespace

	int terminalTest(const char* binary) {

		try {
			TerminalSession terminal(binary);
			try {
				terminal.waitFor("TEST> ");
				terminal.send("abc");
				terminal.waitFor("abc");
				for (const auto& [rows, cols] : {std::pair{18, 45}, std::pair{32, 100}, std::pair{12, 30}, std::pair{24, 80}}) {
					const auto start = terminal.output.size();
					terminal.resize(rows, cols);
					const auto repainted = terminal.waitFor("TEST-REPAINT", start);
					terminal.waitFor("abc", repainted);
				}
				// Move the editing cursor before another character is inserted.
				terminal.send("\x02X\x06" "def\n");
				const auto passed = terminal.waitFor("TEST-TERMINAL-PASS");
				checkLongInput(terminal, passed);
				checkOverflowSpace(terminal, passed);
				terminal.waitForExit();
			} catch (...) {
				printOutput(terminal.output);
				throw;
			}
			std::cout << "PASS: PTY resize, long single-row input, input preservation and Readline restoration\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: PTY resize: " << e.what() << '\n';
			return 1;
		}

	}

	int readlineGuardsTest(const char* binary) {

		try {
			TerminalSession terminal(binary, "--readline-guards-child");
			try {
				terminal.waitFor("RAII-ESC> ");
				terminal.send("\x1b");
				terminal.waitFor("RAII-EOF> ");
				terminal.send("\x04");
				const auto pause = terminal.waitFor("to return...", terminal.waitFor("RAII-PAUSE"));
				terminal.resize(21, 70);
				terminal.waitFor("to return...", terminal.waitFor("RAII-PAUSE", pause));
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
				auto menu = [&](std::string_view title, std::string_view prompt, std::size_t start) {

					return terminal.waitFor(prompt, terminal.waitFor(title, start));

				};
				auto start = menu("Main Menu", "Enter choice [1-5]: ", 0);
				terminal.send("invalid");
				terminal.waitFor("invalid", start);
				start = terminal.output.size(); terminal.resize(18, 55);
				start = menu("Main Menu", "Enter choice [1-5]: ", start);
				terminal.waitFor("invalid", start);
				start = terminal.output.size(); terminal.send("\n");
				menu("Main Menu", "Enter choice [1-5]: ", start);
				start = terminal.output.size(); terminal.send("1\n");
				menu("ManageISO", "Enter choice [1-6]: ", start);
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Main Menu", "Enter choice [1-5]: ", start);
				start = terminal.output.size(); terminal.send("2\n");
				menu("Convert2ISO", "Enter choice [1-5]: ", start);
				start = terminal.output.size(); terminal.send("1\n");
				terminal.waitFor("? for help:", terminal.waitFor("FolderPaths", start));
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Convert2ISO", "Enter choice [1-5]: ", start);
				start = terminal.output.size(); terminal.send("\x04");
				menu("Main Menu", "Enter choice [1-5]: ", start);
				start = terminal.output.size(); terminal.send("3\n");
				terminal.waitFor("DB, ? for help:", start);
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Main Menu", "Enter choice [1-5]: ", start);
				start = terminal.output.size(); terminal.send("4\n");
				terminal.waitFor("? for help: ", start);
				start = terminal.output.size(); terminal.send("\x1b");
				menu("Main Menu", "Enter choice [1-5]: ", start);
				terminal.send("5\n"); terminal.waitForExit();
			} catch (...) { printOutput(terminal.output); throw; }
			std::cout << "PASS: interactive main menu, submenus, settings, import and resize\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: interactive navigation: " << e.what() << '\n'; return 1;
		}

	}

	namespace {
		constexpr std::string_view selectPrompt="? for help: ";

		std::string workflowStep(TerminalSession& term, std::string_view input, std::string_view prompt) {

			const auto start=term.output.size(); term.send(input);
			if (prompt == "Filter: ") return queryFrame(term, start, "");
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

			term.waitFor("Enter choice [1-5]: ",term.waitFor("Main Menu"));
			workflowStep(term, "1\n","Enter choice [1-6]: ");
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
			CHECK(has(frame,"No filter results") && has(frame,"Filter: «"));
			begin=term.output.size(); term.send("\x01");
			end=term.waitFor("FILTER-START!",begin);
			end=term.waitFor("\033[K",end);
			frame=plain(std::string_view(term.output).substr(begin,end-begin));
			CHECK(has(frame,"Filter: FILTER-START!") && has(frame,"»"));
			begin=term.output.size(); term.send("\x05");
			end=term.waitFor("FILTER-TAIL!",begin);
			term.waitFor("\033[K\r",end);
			begin=term.output.size(); term.resize(18,55);
			CHECK(has(queryFrame(term,begin,"FILTER-TAIL!",true),"Filter: «"));
			begin=term.output.size(); term.resize(18,95);
			CHECK(has(queryFrame(term,begin,"FILTER-TAIL!",true),"Filter: «"));
			frame=workflowPreview(term,"\x15zzzz-no-fixture","zzzz-no-fixture");
			CHECK(has(frame,"No filter results"));

		}

		void checkWorkflowFilterPreview(TerminalSession& term) {

			// Stage a selection before preview; cancellation must retain it.
			CHECK(has(workflowStep(term, "1;\n",selectPrompt),"Pending Indices"));
			workflowStep(term, "/","Filter: ");
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
			workflowStep(term, "\n","Filter: ");
			CHECK(has(workflowStep(term, "\x1b",selectPrompt),"Pending Indices"));

		}

		void checkWorkflowCommittedFilters(TerminalSession& term) {

			// Commit a non-overlapping filter, then replace it from the source.
			workflowStep(term, "/","Filter: "); auto frame=workflowPreview(term, "groupbeta","groupbeta");
			CHECK(has(frame,"groupbeta1.iso"));
			frame=workflowStep(term, "\n",selectPrompt); CHECK(has(frame,"groupbeta1.iso") && !has(frame,"groupalpha1.iso"));
			workflowStep(term, "/","Filter: "); frame=workflowPreview(term, "groupalpha","groupalpha");
			CHECK(has(frame,"groupalpha1.iso"));
			frame=workflowStep(term, "\n",selectPrompt); CHECK(has(frame,"groupalpha1.iso") && !has(frame,"groupbeta1.iso"));
			CHECK(has(frame,"Pending Indices"));
			checkWorkflowPage(term, "\x1b[6~","Page 2/3","groupalpha3.iso","groupalpha1.iso");
			workflowStep(term, "\x1b",selectPrompt); // clear committed filter

		}

		void checkWorkflowCopy(TerminalSession& term, const fs::path& source, const fs::path& dest) {

			// Execute the original staged path, regardless of filter history.
			workflowStep(term, "P","into, ? for help:");
			auto begin=term.output.size(); term.send(dest.string()+"\n");
			auto end=term.waitFor("verbose output? (y/n): ",begin);
			CHECK(has(plain(std::string_view(term.output).substr(begin,end-begin)),"COMPLETED"));
			begin=term.output.size(); term.resize(20,110);
			end=term.waitFor("verbose output? (y/n): ",begin);
			CHECK(has(plain(std::string_view(term.output).substr(begin,end-begin)),"COMPLETED"));
			auto frame=workflowStep(term, "y\n","to continue...");
			CHECK(has(frame,"Copied:") && has(frame,"groupalpha1.iso"));
			CHECK(!has(frame,"groupalpha2.iso"));
			begin=term.output.size(); term.resize(24,120);
			end=term.waitFor("to continue...",begin);
			frame=listFrame(term,begin,end);
			CHECK(has(frame,"Copied:") && has(frame,"groupalpha1.iso"));
			CHECK(readFile(dest/"groupalpha1.iso")==readFile(source/"groupalpha1.iso"));
			frame=workflowStep(term, "\n",selectPrompt);
			CHECK(!has(frame,"Copied:"));

		}

		void checkWorkflowDelete(TerminalSession& term, const fs::path& source) {

			// Return to menu; exercise delete rejection followed by confirmation.
			workflowStep(term, "\x1b","Enter choice [1-6]: ");
			workflowStep(term, "3\n",selectPrompt);
			workflowStep(term, "1\n","Proceed? (y/n): ");
			workflowStep(term, "n\n",selectPrompt); CHECK(fs::exists(source/"groupalpha1.iso"));
			workflowStep(term, "1\n","Proceed? (y/n): ");
			const auto begin=term.output.size(); term.send("y\n"); term.waitFor("verbose output? (y/n): ",begin);
			auto frame=workflowStep(term, "Y\n","to continue...");
			CHECK(has(frame,"Deleted:") && has(frame,"groupalpha1.iso") && !has(frame,"Copied:"));
			frame=workflowStep(term, "\x1b",selectPrompt);
			CHECK(!has(frame,"Deleted:") && !fs::exists(source/"groupalpha1.iso"));
			CHECK(fs::exists(source/"groupalpha2.iso"));

		}

		void checkWorkflowMove(TerminalSession& term, const fs::path& source, const fs::path& dest) {

			// The source directory sorts before the destination; the deleted
			// first row is now replaced by alpha2. Move it to the fixture dir.
			workflowStep(term, "\x1b","Enter choice [1-6]: ");
			auto frame=workflowStep(term, "4\n",selectPrompt); CHECK(has(frame,"groupalpha2.iso"));
			workflowStep(term, "1\n","into, ? for help:");
			const auto begin=term.output.size(); term.send(dest.string()+"\n");
			term.waitFor("verbose output? (y/n): ",begin);
			frame=workflowStep(term, "y\n","to continue...");
			CHECK(has(frame,"Moved:") && has(frame,"groupalpha2.iso") && !has(frame,"Deleted:") && !has(frame,"Copied:"));
			frame=workflowStep(term, "\x04",selectPrompt);
			CHECK(!has(frame,"Moved:") && !fs::exists(source/"groupalpha2.iso"));
			CHECK(readFile(dest/"groupalpha2.iso")=="fixture payload: groupalpha2.iso");
			CHECK(fs::exists(source/"groupalpha3.iso"));

		}

		void finishWorkflow(TerminalSession& term) {

			workflowStep(term, "\x1b","Enter choice [1-6]: "); workflowStep(term, "\x1b","Enter choice [1-5]: ");
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

		constexpr std::string_view mainPrompt = "Enter choice [1-5]: ";
		constexpr std::string_view selectionPrompt = "? for help: ";
		constexpr std::string_view valuePrompt = "Skip";
		constexpr std::string_view continuePrompt = "to continue...";
		constexpr std::string_view returnPrompt = "to return...";

		std::string step(TerminalSession& term, std::string_view input, std::string_view marker) {

			const auto start = term.output.size();
			term.send(input);
			if (marker == "Filter: ") return queryFrame(term, start, "");
			auto end = waitForStepPrompt(term, marker, start);
			// The value prompt has an ANSI sequence between Skip and the colon.
			if (marker == valuePrompt) end = term.waitFor(": ", end);
			return plain(std::string_view(term.output).substr(start, end-start));

		}

		void expectStep(TerminalSession& term, std::string_view input,
		                std::string_view marker, std::string_view expected) {

			const auto frame = step(term, input, marker);
			if (!has(frame, expected))
			    throw std::runtime_error("expected '" + std::string(expected) +
			        "' before prompt '" + std::string(marker) + "'");

		}

		template<class Workflow>
		void runMenuWorkflow(const char* binary, Workflow workflow) {

			TerminalSession term(binary, nullptr, 120);
			try {
				term.waitFor(mainPrompt, term.waitFor("Main Menu"));
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
				expectStep(term, "1001\n", "to try again...", "Invalid value:");
				CHECK(readFile(config) == original);
				expectStep(term, "\n", valuePrompt, "Current: 2");
				// Resize with a typed value, then submit the preserved buffer.
				const auto typed = term.output.size();
				term.send("3"); term.waitFor("3", typed);
				const auto resizeStart = term.output.size();
				term.resize(20, 100);
				auto end = term.waitFor(valuePrompt, resizeStart);
				end = term.waitFor(": ", end);
				term.waitFor("3", end);
				expectStep(term, "\n", continuePrompt, "Updated and saved to:");
				CHECK(has(readFile(config), "pagination = 3\n"));
				expectStep(term, "\n", valuePrompt, "Setting: filenames_only");
				// Skip the remaining batch item; no change to its value.
				step(term, "\n", selectionPrompt);
				CHECK(has(readFile(config), "filenames_only = off\n"));
				step(term, "5\n", valuePrompt);
				step(term, "\x1b", selectionPrompt);
				CHECK(has(readFile(config), "filenames_only = off\n"));
				step(term, "r", "Reset all settings to default? (y/n): ");
				step(term, "n\n", selectionPrompt);
				CHECK(has(readFile(config), "pagination = 3\n"));
				CHECK(has(readFile(config), "verify_checksum = off\n"));
				expectStep(term, "7\n", valuePrompt, "Setting: verify_checksum");
				expectStep(term, "yes\n", "to try again...", "Invalid value:");
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
				step(term, "r", "Reset all settings to default? (y/n): ");
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
		std::string rawStep(TerminalSession& term, std::string_view input, std::string_view marker) {

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

		void checkMenuAppearance(std::string_view frame, const Appearance& a, std::string_view prompt) {

			CHECK(has(frame, std::string(a.skin)+"+----------------------+"));
			CHECK(has(frame, std::string(a.prompt)+std::string(prompt)));

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
			CHECK(has(frame, std::string(a.prompt)+", ? for help: "));

		}

		void checkAppearance(TerminalSession& term, const Appearance& a) {

			auto frame = rawStep(term, "1\n", "Enter choice [1-6]: ");
			CHECK(has(frame, "ManageISO"));
			checkMenuAppearance(frame, a, "Enter choice [1-6]: ");
			frame = rawStep(term, "5\n", selectionPrompt);
			checkListAppearance(frame, a, 120);
			const auto start = term.output.size(); term.resize(20, 80);
			const auto end = waitForStepPrompt(term, selectionPrompt, start);
			checkListAppearance(std::string_view(term.output).substr(start, end-start), a, 80);
			const auto wideStart = term.output.size(); term.resize(24, 120);
			const auto wideEnd = waitForStepPrompt(term, selectionPrompt, wideStart);
			checkListAppearance(std::string_view(term.output).substr(wideStart, wideEnd-wideStart), a, 120);
			step(term, "\x1b", "Enter choice [1-6]: ");
			frame = rawStep(term, "\x1b", mainPrompt);
			CHECK(has(frame, "Main Menu"));
			checkMenuAppearance(frame, a, mainPrompt);

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
				step(term, "r", "Reset all settings to default? (y/n): ");
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

				step(term, "3\n", "DB, ? for help:");
				expectStep(term, "?\n", returnPrompt, "Cleanup/Display Commands");
				step(term, "\n", "DB, ? for help:");
				expectStep(term, missing.string()+"\n", continuePrompt, "Lack of valid paths");
				CHECK(readFile(db).empty() && readFile(paths).empty());
				step(term, "\n", "DB, ? for help:");
				const auto input = source.string()+";"+nested.string()+";"+missing.string()+"\n";
				auto frame = step(term, input, continuePrompt);
				CHECK(has(frame, "2 ISO imported") && has(frame, "Invalid paths omitted from search:"));
				CHECK(has(frame, missing.string()));
				CHECK(fileLines(db) == expected);
				CHECK(fileLines(paths) == std::vector<std::string>{source.string()+";"+nested.string()});
				CHECK(!has(readFile(paths), missing.string()));
				// Repaint the report; must not re-import or duplicate history.
				const auto start = term.output.size(); term.resize(20, 100);
				const auto end = term.waitFor(continuePrompt, start);
				CHECK(has(plain(std::string_view(term.output).substr(start, end-start)), "2 ISO imported"));
				CHECK(fileLines(db) == expected);
				step(term, "\n", "DB, ? for help:");
				expectStep(term, source.string()+"\n", continuePrompt, "No new ISO found");
				CHECK(fileLines(db) == expected);
				step(term, "\n", "DB, ? for help:");
				frame = step(term, "*stats\n", returnPrompt);
				CHECK(has(frame, "=== ISO Database ===") && has(frame, "Entries: 2"));
				step(term, "\n", "DB, ? for help:");
				expectStep(term, "!clr_paths\n", continuePrompt, "FolderPath database cleared successfully.");
				CHECK(readFile(paths).empty() && fileLines(db) == expected);
				CHECK(readFile(filters) == "fixture-filter\n");
				step(term, "\n", "DB, ? for help:");
				expectStep(term, "!clr_filter\n", continuePrompt, "FilterTerm database cleared successfully.");
				CHECK(readFile(filters).empty() && fileLines(db) == expected);
				step(term, "\n", "DB, ? for help:");
				expectStep(term, "!clr\n", continuePrompt, "ISO database cleared successfully.");
				CHECK(readFile(db).empty());
				step(term, "\n", "DB, ? for help:");
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
			step(term, "1\n", "? for help:");
			expectStep(term, "ls\n", continuePrompt, "entries to display.");
			step(term, "\n", "? for help:");
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
			step(term, "/", "Filter: ");
			const auto start = term.output.size(); term.send("beta");
			frame = queryFrame(term, start, "beta");
			CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));
			frame = step(term, "\n", selectionPrompt);
			CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));

		}

		void checkSuccessfulConversions(TerminalSession& term, const fs::path& source, const std::string& alpha, const std::string& beta) {

			// Execute the staged source index while another image is visible.
			expectStep(term, "P", "verbose output? (y/n): ", "COMPLETED");
			auto frame = step(term, "y\n", continuePrompt);
			CHECK(has(frame, "Convert2ISO:") && has(frame, "alpha.iso") && has(frame, "→ ISO."));
			CHECK(!has(frame, "beta.iso") && !has(frame, "Failed."));
			frame = step(term, "\n", selectionPrompt);
			CHECK(has(frame, "beta.img") && !has(frame, "alpha.img") && !has(frame, "→ ISO."));
			CHECK(readFile(source/"alpha.iso") == alpha);
			CHECK(!fs::exists(source/"beta.iso") && !fs::exists(source/"broken.iso"));
			expectStep(term, "1\n", "verbose output? (y/n): ", "COMPLETED");
			frame = step(term, "n\n", selectionPrompt);
			CHECK(!has(frame, "→ ISO."));
			CHECK(readFile(source/"beta.iso") == beta);
			step(term, "\x1b", selectionPrompt);

		}

		void checkFailedConversion(TerminalSession& term, const fs::path& source, const fs::path& db) {

			expectStep(term, "3\n", "verbose output? (y/n): ", "FAILED");
			const auto frame = step(term, "Y\n", continuePrompt);
			CHECK(has(frame, "broken.img") && has(frame, "Failed.") && !has(frame, "→ ISO."));
			step(term, "\x1b", selectionPrompt);
			CHECK(!fs::exists(source/"broken.iso"));
			CHECK(fileLines(db) == (std::vector<std::string>{(source/"alpha.iso").string(), (source/"beta.iso").string()}));

		}

		void checkSkippedConversion(TerminalSession& term, const fs::path& source) {

			// Existing output is a skip and must never be overwritten.
			writeFile(source/"alpha.iso", "keep existing ISO");
			expectStep(term, "1\n", "verbose output? (y/n): ", "COMPLETED");
			const auto frame = step(term, "y\n", continuePrompt);
			CHECK(has(frame, "alpha.img") && has(frame, "ISOalrExists → Skipped."));
			CHECK(!has(frame, "Failed.") && !has(frame, "→ ISO."));
			step(term, "\x04", selectionPrompt);
			CHECK(readFile(source/"alpha.iso") == "keep existing ISO");

		}

		void checkConversionCache(TerminalSession& term) {

			step(term, "\x1b", "? for help:");
			const auto frame = step(term, "ls\n", selectionPrompt);
			CHECK(has(frame, "alpha.img") && has(frame, "beta.img"));
			step(term, "\x1b", "? for help:");
			expectStep(term, "!clr\n", continuePrompt, "BIN/IMG cache cleared.");
			step(term, "\n", "? for help:");
			expectStep(term, "ls\n", continuePrompt, "entries to display.");
			step(term, "\n", "? for help:");
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
		CHECK(has(frame,i==2?"Successful: 0/3":"Successful: 3/3"));
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
				auto end=term.waitFor("? for help: ",term.waitFor("IDX>DEVICE"));
				term.send("\x1b");
				end=term.waitFor("TEST-MAPPING-CANCELLED",end);
				end=term.waitFor("? for help: ",end);
				term.send("1>/dev/null;1>/dev/null\n");
				end=term.waitFor("used multiple times",end); end=term.waitFor("to try again...",end);
				term.send("\n"); end=term.waitFor("? for help: ",end);
				const std::string mapping="1>/dev/zero;2>/dev/null;1>/dev/full";
				term.send(mapping+"\n"); end=term.waitFor("Proceed? (y/n): ",end);
				const auto start=term.output.size(); term.resize(24,100);
				end=term.waitFor("Proceed? (y/n): ",start);
				term.send("n\n"); end=term.waitFor("? for help: ",end);
				// Declining retains the input for editing; resubmit it unchanged.
				end=term.waitFor(mapping,end); term.send("\n"); end=term.waitFor("Proceed? (y/n): ",end);
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
				end=term.waitFor("? for help: ",end);
				term.send("1>/dev/zero;2>/dev/null\n"); end=term.waitFor("Proceed? (y/n): ",end);
				term.send("y\n");
				term.waitFor("TEST-WRITE-UI-PASS",end); term.waitForExit();
				CHECK(!has(plain(term.output), "Verify write integrity? (y/n):"));
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

		CHECK(has(frame,"▄░░░░") && has(frame,"▇▇▄░░") && has(frame,"▇▇▇▇▄"));
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
				CHECK(has(frame,"Successful: 3/3 | Verified: 1/3") && has(frame,"PASS") && has(frame,"FAIL") && has(frame,"] CXL  ") && has(frame,"(avg)"));
				CHECK(has(frame,"▇▇▇▇▇"));
				CHECK(has(frame,"51.2 KB/s (avg)") && !has(frame,"100.0 MB/s (avg)"));
				start=term.output.size(); term.resize(20,100);
				frame=screenFrame(term,start,"INTERRUPTED"); CHECK(has(frame,"Successful: 3/3 | Verified: 1/3"));
				term.send("\n"); term.waitFor("TEST-PROGRESS-PASS",start); term.waitForExit();
			} catch (...) { printOutput(term.output); throw; }
			std::cout<<"PASS: live and final USB progress rows, glyphs, speeds, cancellation and terminal resizing\n";
			return 0;
		} catch(const std::exception& e) { std::cerr<<"FAIL: USB progress screen: "<<e.what()<<'\n'; return 1; }

	}
} // namespace isocmd_tests
