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

		// Readline can emit several preview frames for one input write. Require the
		// complete edited query after the Filter prompt in the same latest redraw.
		std::string queryFrame(TerminalSession& term,std::size_t start,std::string_view query) {

			for (;;) {
				const auto prompt=term.waitFor("Filter: ",start);
				const auto end=term.waitFor(query,prompt);
				const auto frame=listFrame(term,start,end);
				if (has(frame,std::string("Filter: ")+std::string(query))) return frame;
				start=prompt;
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
				terminal.waitFor("TEST-TERMINAL-PASS");
				terminal.waitForExit();
			} catch (...) {
				printOutput(terminal.output);
				throw;
			}
			std::cout << "PASS: PTY resize, input preservation and Readline restoration\n";
			return 0;
		} catch (const std::exception& e) {
			std::cerr << "FAIL: PTY resize: " << e.what() << '\n';
			return 1;
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
				constexpr std::string_view selectPrompt="? for help: ";
				auto sendWait=[&](std::string_view input,std::string_view prompt) {

					const auto start=term.output.size(); term.send(input);
					return listFrame(term,start,term.waitFor(prompt,start));

				};
				auto page=[&](std::string_view input,std::string_view title,std::string_view first,std::string_view absent) {

					const auto frame=sendWait(input,selectPrompt);
					CHECK(has(frame,title) && has(frame,first) && !has(frame,absent));
					return frame;

				};
				term.waitFor("Enter choice [1-5]: ",term.waitFor("Main Menu"));
				sendWait("1\n","Enter choice [1-6]: ");
				page("5\n","Page 1/4","groupalpha1.iso","groupalpha3.iso");
				page("\x1b[5~","Page 1/4","groupalpha1.iso","groupalpha3.iso");
				page("\x1b[6~","Page 2/4","groupalpha3.iso","groupalpha1.iso");
				page("g4\n","Page 4/4","groupbeta1.iso","groupalpha1.iso");
				page("\x1b[6~","Page 4/4","groupbeta1.iso","groupalpha1.iso");
				page("g1\n","Page 1/4","groupalpha1.iso","groupbeta1.iso");
				// Stage a selection before preview; cancellation must retain it.
				CHECK(has(sendWait("1;\n",selectPrompt),"Pending Indices"));
				sendWait("/","Filter: ");
				auto begin=term.output.size(); term.send("groupalpha");
				auto frame=queryFrame(term,begin,"groupalpha");
				CHECK(has(frame,"groupalpha1.iso") && !has(frame,"groupbeta1.iso"));
				auto preview=[&](std::string_view key,std::string_view query) {

					const auto start=term.output.size(); term.send(key);
					return queryFrame(term,start,query);

				};
				frame=preview("\x1b[6~","groupalpha");
				CHECK(has(frame,"groupalpha3.iso") && !has(frame,"groupalpha1.iso"));
				frame=preview("\x1b[6~","groupalpha"); CHECK(has(frame,"groupalpha5.iso"));
				frame=preview("\x1b[6~","groupalpha"); CHECK(has(frame,"groupalpha5.iso"));
				frame=preview("\x1b[5~","groupalpha"); CHECK(has(frame,"groupalpha3.iso"));
				begin=term.output.size(); term.resize(18,95);
				frame=queryFrame(term,begin,"groupalpha"); CHECK(has(frame,"groupalpha3.iso"));
				// Changing query resets scroll; widening must use original source.
				frame=preview("\x15groupbeta","groupbeta");
				CHECK(has(frame,"groupbeta1.iso") && !has(frame,"groupalpha1.iso"));
				frame=preview("\x15zzzz-no-fixture","zzzz-no-fixture");
				CHECK(has(frame,"No filter results"));
				// No-match Enter must leave us in the filter prompt.
				sendWait("\n","Filter: ");
				CHECK(has(sendWait("\x1b",selectPrompt),"Pending Indices"));
				// Commit a non-overlapping filter, then replace it from the source.
				sendWait("/","Filter: "); frame=preview("groupbeta","groupbeta");
				CHECK(has(frame,"groupbeta1.iso"));
				frame=sendWait("\n",selectPrompt); CHECK(has(frame,"groupbeta1.iso") && !has(frame,"groupalpha1.iso"));
				sendWait("/","Filter: "); frame=preview("groupalpha","groupalpha");
				CHECK(has(frame,"groupalpha1.iso"));
				frame=sendWait("\n",selectPrompt); CHECK(has(frame,"groupalpha1.iso") && !has(frame,"groupbeta1.iso"));
				CHECK(has(frame,"Pending Indices"));
				page("\x1b[6~","Page 2/3","groupalpha3.iso","groupalpha1.iso");
				sendWait("\x1b",selectPrompt); // clear committed filter
				// Execute the original staged path, regardless of filter history.
				sendWait("P","into, ? for help:");
				begin=term.output.size(); term.send(dest.string()+"\n");
				auto end=term.waitFor("verbose output? (y/n): ",begin);
				CHECK(has(plain(std::string_view(term.output).substr(begin,end-begin)),"COMPLETED"));
				begin=term.output.size(); term.resize(20,110);
				end=term.waitFor("verbose output? (y/n): ",begin);
				CHECK(has(plain(std::string_view(term.output).substr(begin,end-begin)),"COMPLETED"));
				sendWait("n\n",selectPrompt);
				CHECK(readFile(dest/"groupalpha1.iso")==readFile(source/"groupalpha1.iso"));
				// Return to menu; exercise delete rejection followed by confirmation.
				sendWait("\x1b","Enter choice [1-6]: ");
				sendWait("3\n",selectPrompt);
				sendWait("1\n","Proceed? (y/n): ");
				sendWait("n\n",selectPrompt); CHECK(fs::exists(source/"groupalpha1.iso"));
				sendWait("1\n","Proceed? (y/n): ");
				begin=term.output.size(); term.send("y\n"); term.waitFor("verbose output? (y/n): ",begin);
				sendWait("n\n",selectPrompt); CHECK(!fs::exists(source/"groupalpha1.iso"));
				CHECK(fs::exists(source/"groupalpha2.iso"));
				// The source directory sorts before the destination; the deleted
				// first row is now replaced by alpha2. Move it to the fixture dir.
				sendWait("\x1b","Enter choice [1-6]: ");
				frame=sendWait("4\n",selectPrompt); CHECK(has(frame,"groupalpha2.iso"));
				sendWait("1\n","into, ? for help:");
				begin=term.output.size(); term.send(dest.string()+"\n");
				term.waitFor("verbose output? (y/n): ",begin);
				sendWait("n\n",selectPrompt);
				CHECK(!fs::exists(source/"groupalpha2.iso"));
				CHECK(readFile(dest/"groupalpha2.iso")=="fixture payload: groupalpha2.iso");
				CHECK(fs::exists(source/"groupalpha3.iso"));
				sendWait("\x1b","Enter choice [1-6]: "); sendWait("\x1b","Enter choice [1-5]: ");
				// Observe the exit action's clear-screen sequence before waiting
				// for shutdown, distinguishing lost input from a cleanup hang.
				begin=term.output.size(); term.send("5\n");
				term.waitFor("\033[2J\033[3J\033[H",begin);
				term.waitForExit();
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
			auto end = term.waitFor(marker, start);
			// The value prompt has an ANSI sequence between Skip and the colon.
			if (marker == valuePrompt) end = term.waitFor(": ", end);
			return plain(std::string_view(term.output).substr(start, end-start));

		}

		void expectStep(TerminalSession& term, std::string_view input,
		                std::string_view marker, std::string_view expected) {

			CHECK(has(step(term, input, marker), expected));

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

				expectStep(term, "4\n", selectionPrompt, "Settings Editor");
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
				step(term, "\x1b", mainPrompt);

			});
			// New process must load the persisted value, not just the old cache.
			runMenuWorkflow(binary, [&](TerminalSession& term) {

				step(term, "4\n", selectionPrompt);
				expectStep(term, "6\n", valuePrompt, "Current: 3");
				step(term, "\x1b", selectionPrompt);
				step(term, "r", "Reset all settings to default? (y/n): ");
				expectStep(term, "y\n", continuePrompt, "Defaults applied");
				CHECK(has(readFile(config), "pagination = 25\n"));
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

				expectStep(term, "2\n", mainPrompt, "Convert2ISO");
				step(term, "1\n", "? for help:");
				expectStep(term, "ls\n", continuePrompt, "entries to display.");
				step(term, "\n", "? for help:");
				auto frame = step(term, source.string()+"\n", continuePrompt);
				CHECK(has(frame, "3 {.bin/.img} files found"));
				frame = step(term, "\n", selectionPrompt);
				CHECK(has(frame, "alpha.img") && has(frame, "beta.img") && has(frame, "broken.img"));
				expectStep(term, "1;\n", selectionPrompt, "Pending Indices");
				frame = step(term, "clr\n", selectionPrompt);
				CHECK(!has(frame, "Pending Indices"));
				expectStep(term, "1;\n", selectionPrompt, "Pending Indices");
				step(term, "/", "Filter: ");
				const auto start = term.output.size(); term.send("beta");
				frame = queryFrame(term, start, "beta");
				CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));
				frame = step(term, "\n", selectionPrompt);
				CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));
				// Execute the staged source index while another image is visible.
				expectStep(term, "P", "verbose output? (y/n): ", "COMPLETED");
				frame = step(term, "n\n", selectionPrompt);
				CHECK(has(frame, "beta.img") && !has(frame, "alpha.img"));
				CHECK(readFile(source/"alpha.iso") == alpha);
				CHECK(!fs::exists(source/"beta.iso") && !fs::exists(source/"broken.iso"));
				expectStep(term, "1\n", "verbose output? (y/n): ", "COMPLETED");
				step(term, "n\n", selectionPrompt);
				CHECK(readFile(source/"beta.iso") == beta);
				step(term, "\x1b", selectionPrompt);
				expectStep(term, "3\n", "verbose output? (y/n): ", "FAILED");
				step(term, "n\n", selectionPrompt);
				CHECK(!fs::exists(source/"broken.iso"));
				CHECK(fileLines(db) == (std::vector<std::string>{(source/"alpha.iso").string(), (source/"beta.iso").string()}));
				// Existing output is a skip and must never be overwritten.
				writeFile(source/"alpha.iso", "keep existing ISO");
				expectStep(term, "1\n", "verbose output? (y/n): ", "COMPLETED");
				step(term, "n\n", selectionPrompt);
				CHECK(readFile(source/"alpha.iso") == "keep existing ISO");
				step(term, "\x1b", "? for help:");
				frame = step(term, "ls\n", selectionPrompt);
				CHECK(has(frame, "alpha.img") && has(frame, "beta.img"));
				step(term, "\x1b", "? for help:");
				expectStep(term, "!clr\n", continuePrompt, "BIN/IMG cache cleared.");
				step(term, "\n", "? for help:");
				expectStep(term, "ls\n", continuePrompt, "entries to display.");
				step(term, "\n", "? for help:");
				step(term, "\x1b", mainPrompt); // Convert2ISO submenu
				step(term, "\x1b", mainPrompt); // main menu

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

	static void checkWriteResult(const std::string& frame, int i) {

		const bool successful=i==0 || i==3;
		CHECK(has(frame,successful?"COMPLETED":i==1?"PARTIAL":"FAILED"));
		CHECK(has(frame,i==2?"Successful: 0/3":"Successful: 3/3"));
		if (i==3) CHECK(!has(frame,"Verified:"));
		else CHECK(has(frame,i==0?"Verified: 3/3":i==1?"Verified: 2/3":"Verified: 0/3"));
		checkWriteElapsed(frame, i);
		CHECK(has(frame,"iso2.iso") && has(frame,"iso10.iso"));
		if (i==0) CHECK(has(frame,"PASS"));
		if (i==3) CHECK(has(frame,"DONE") && !has(frame,"PASS"));
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
				term.send("y\n");
				end=term.waitFor("Verify write integrity? (y/n): ",end);
				const auto verifyResize=term.output.size(); term.resize(24,110);
				end=term.waitFor("Verify write integrity? (y/n): ",verifyResize);
				const auto afterConfirm=term.output.size(); term.send("y\n");
				for (int i=0;i<4;++i) {
					const auto startResult=i==0?afterConfirm:end;
					end=term.waitFor("TEST-WRITE-RESULT-"+std::to_string(i),startResult);
					const auto frame=plain(std::string_view(term.output).substr(startResult,end-startResult));
					checkWriteResult(frame, i);
					term.send("\n");
				}
				end=term.waitFor("TEST-VERIFY-DEFAULT",end);
				end=term.waitFor("? for help: ",end);
				term.send("1>/dev/zero;2>/dev/null\n"); end=term.waitFor("Proceed? (y/n): ",end);
				term.send("y\n"); end=term.waitFor("Verify write integrity? (y/n): ",end);
				term.send("\n");
				term.waitFor("TEST-WRITE-UI-PASS",end); term.waitForExit();
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
				start=term.output.size(); term.resize(20,100);
				frame=screenFrame(term,start,"INTERRUPTED"); CHECK(has(frame,"Successful: 3/3 | Verified: 1/3"));
				term.send("\n"); term.waitFor("TEST-PROGRESS-PASS",start); term.waitForExit();
			} catch (...) { printOutput(term.output); throw; }
			std::cout<<"PASS: live and final USB progress rows, glyphs, speeds, cancellation and terminal resizing\n";
			return 0;
		} catch(const std::exception& e) { std::cerr<<"FAIL: USB progress screen: "<<e.what()<<'\n'; return 1; }

	}
} // namespace isocmd_tests
