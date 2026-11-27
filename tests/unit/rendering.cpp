// SPDX-License-Identifier: GPL-3.0-or-later

#include "../common/testHarness.h"

import isocmd.ui.display.themes;
import isocmd.settings.settings;
import isocmd.write2usb.ui.textWidth;
import isocmd.core.uiState;
import isocmd.core.sharedRefreshState;
import isocmd.filter.filter;
import isocmd.ui.display.printList;
import isocmd.ui.display.stringListView;
import isocmd.select.select;

namespace isocmd_tests {

	void themeAndWidth() {

		using namespace UI::Palette;
		for (const auto& base : ConfigValues::base) {
			defaultColor = base;
			updateDefaultColors();
			CHECK(PromptBoldAlt == "\001" + BoldReset + "\002");
			for (const auto& theme : ConfigValues::theme) {
				globalTheme = theme;
				const auto p = getPromptTheme();
				CHECK(p.reset == PromptBoldAlt);
				CHECK(p.primary.front() == '\001' && p.primary.back() == '\002');
				CHECK(getFilterTheme("", false).iso.empty());
				CHECK(!getFilterTheme("", true).iso.empty());
				for (const auto& s : ConfigValues::skin) { skin = s; CHECK(getskin() != Reset); }
			}
		}
		defaultColor = "invalid"; updateDefaultColors(); CHECK(BoldReset == White);
		skin = "invalid"; CHECK(getskin() == Reset);
		globalTheme = "original"; const auto* original = getActiveTheme();
		globalTheme = "invalid"; CHECK(getActiveTheme() == original);
		globalTheme = "original"; defaultColor = "white"; skin = "white"; updateDefaultColors(); getskin();
		CHECK(utf8Width("αβ日本語") == 5);
		CHECK(visibleWidth("\033[31mαβ日本語\033[0m") == 5);
		CHECK(clipToWidth("\033[31mαβ日本語\033[0m", 2) == "\033[31mαβ");
		CHECK(clipToWidth("abc", 0).empty());
		CHECK(clipToWidth("\033[31mabc", 0) == "\033[31m");
		CHECK(clipToWidth("αβ\033[0m", 2) == "αβ\033[0m");
		CHECK(truncateIsoName("abcdefghijk.ISO", 10) == "abcde….ISO");
		CHECK(truncateIsoName("αβγδεζηθ.iso", 9) == "αβγδ….iso");
		CHECK(truncateIsoName("abc.iso", 20) == "abc.iso");
		CHECK(truncateIsoName("abcdefghijk.iso", 0) == "abcd….iso");
		for (std::size_t w = 0; w < 40; ++w)
		    CHECK(visibleWidth(clipToWidth("\033[31mαβ日本語abcdefghijklmnop\033[0m", w)) <= w);

	}

	static void checkListPages(const auto& render, const auto& has, const std::vector<std::string>& files, std::size_t& page) {

		GlobalState::ITEMS_PER_PAGE=2;
		auto first=render(files);
		CHECK(has(first,"alpha.iso") && has(first,"beta.iso") && !has(first,"gamma.iso"));
		CHECK(has(first,"[PgDn] Next") && !has(first,"[PgUp] Prev") && has(first,"1-2 3"));
		page=99; auto last=render(files);
		CHECK(has(last,"gamma.iso") && !has(last,"alpha.iso") && has(last,"[PgUp] Prev") && !has(last,"[PgDn] Next"));
		GlobalState::ITEMS_PER_PAGE=0; auto all=render(files);
		CHECK(has(all,"alpha.iso") && has(all,"gamma.iso") && !has(all,"[PgUp]") && !has(all,"[PgDn]"));

	}

	static void checkFilteredList(const auto& render, const auto& has, const std::vector<std::string>& files, PrintListContext& context) {

		const std::vector<std::size_t> indices{1,2};
		filteringStack.push_back(FilteringState{{1,2},"test",true,false});
		auto filtered=render(StringListView(files,indices));
		CHECK(!has(filtered,"alpha.iso") && has(filtered,"beta.iso") && has(filtered,"2^") && has(filtered,"(1-2)^ 3^"));
		context.showPendingIndices=false; context.showSourceIndices=false;
		auto hidden=render(StringListView(files,indices));
		CHECK(!has(hidden,"Pending Indices") && !has(hidden,"2^") && !has(hidden,"3^"));
		auto empty=render(StringListView(files,std::vector<std::size_t>{}));
		CHECK(!has(empty,"alpha.iso") && !has(empty,"beta.iso"));

	}

	void listRendering() {

		struct Restore {
			std::size_t pageSize = GlobalState::ITEMS_PER_PAGE;
			std::vector<FilteringState> stack = std::move(filteringStack);

			~Restore() {

				GlobalState::ITEMS_PER_PAGE=pageSize; filteringStack=std::move(stack);

			}
		} restore;
		filteringStack.clear();
		const std::vector<std::string> files{"/sample/alpha.iso", "/sample/beta.iso", "/sample/gamma.iso"};
		std::vector<std::string> pending{"1-2", "3"}; bool hasPending=true;
		std::size_t page=0; auto state=std::make_shared<RefreshState>();
		PrintListContext context{pending, hasPending, page, state}; context.clearFirst=false;
		auto render = [&](const StringListView& view) {

			std::ostringstream out;

			struct Redirect { std::streambuf* old; ~Redirect(){

					std::cout.rdbuf(old);

				} } redirect{std::cout.rdbuf(out.rdbuf())};

			printList(view,"ISO_FILES","cp",context);
			return out.str();

		};
		auto has=[](const std::string& s,std::string_view text){ return s.find(text)!=std::string::npos; };
		checkListPages(render, has, files, page);
		checkFilteredList(render, has, files, context);

	}

	void paginationCommands() {

		std::size_t total=3,page=0;
		bool filtered=false, redraw=false, sort=false;
		std::vector<std::string> files;
		auto command=[&](const std::string& cmd) {

			redraw=false;
			return processPaginationHelpAndDisplay(cmd,total,page,filtered,redraw,"cp",sort,nullptr,files);

		};
		struct PageCase {
			const char* input;
			std::size_t total;
			std::size_t expectedPage;
			bool handled;
			bool expectedRedraw;
		};
		const PageCase cases[] = {
			{"PgUp", 3, 0, true, true},
			{"PgDn", 3, 1, true, true},
			{"PgDn", 3, 2, true, true},
			{"PgDn", 3, 2, true, true},
			{"g1", 3, 0, true, true},
			{"g3", 3, 2, true, true},
			{"g0", 3, 2, true, false},
			{"g4", 3, 2, true, false},
			{"g999999999999999999999999", 3, 2, true, false},
			{"1 2", 3, 2, false, false},
			{"PgDn", 1, 0, true, true},
			{"PgDn", 0, 0, true, true},
			{"g1", 0, 0, true, false},
		};
		for (const auto& test : cases) {
			total = test.total;
			try {
				CHECK(command(test.input) == test.handled);
				CHECK(page == test.expectedPage);
				CHECK(redraw == test.expectedRedraw);
			} catch (const std::exception& e) {
				throw std::runtime_error(std::string(test.input) + ": " + e.what());
			}
		}

	}

} // namespace isocmd_tests
