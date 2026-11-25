# ============================================================================
#  iso-commander Makefile
#
#  Layout:
#    modules/isocmd/<dir>/*.cppm   C++23 module interfaces (module isocmd.<dir>.<name>)
#    libstdc++ bits/std.cc        compiler-provided standard library module (import std;)
#    src/<module>/*.cpp            implementation, `import`s the modules above
#    deps/                         vendored third-party code (libchdr)
#    build/<variant>/              object + dependency files (generated)
#    isocmd                        final binary (generated, kept at the root)
#
#  Usage:
#    make                  optimized dynamic build
#    make STATIC=1         fully static build
#    make SANITIZE=1       AddressSanitizer build
#    make SANITIZE=2       ThreadSanitizer build
#    make FAST=1           quick development build: -O0, no LTO (separate build/fast tree).
#                          Much quicker edit-compile-link cycles; not for release use.
#
#  Requires GCC 15+ with libstdc++'s C++23 standard library module.
#  Native std is cached outside the ordinary build tree.
#    make clean cache             discard build output and standard-library cache
#    make cache                   discard cached standard-library artifacts only
#    make V=1                     show full compiler command lines
#    make install [BINDIR=/some/dir]
# ============================================================================

# ---------- Parallelism ----------
NUM_PROCESSORS := $(shell nproc)
ifeq ($(filter -j%, $(MAKEFLAGS)),)
    MAKEFLAGS += -j$(NUM_PROCESSORS)
endif

# ---------- Project ----------
TARGET   := isocmd
SRC_DIR  := src
DEPS_DIR := deps
BUILD_DIR ?= build
BINDIR   ?= bin

# ---------- Toolchain ----------
CXX ?= g++
GCC_VERSION := $(shell $(CXX) -dumpfullversion 2>/dev/null)
GCC_MAJOR := $(firstword $(subst ., ,$(GCC_VERSION)))
ifeq ($(shell [ "$(GCC_MAJOR)" -ge 15 ] 2>/dev/null && echo 1),)
    $(error GCC 15 or newer with libstdc++ standard library modules is required)
endif
GCC_NATIVE_STD := $(shell [ "$(GCC_MAJOR)" -ge 16 ] && echo 1 || echo 0)
# ---------- Verbosity ----------
ifeq ($(V),1)
    Q :=
else
    Q := @
endif

# ---------- Compiler and common flags ----------
CXXFLAGS_BASE = -std=c++23 -Wall -Wextra $(if $(filter 1,$(GCC_NATIVE_STD)),-fmodules,-fmodules-ts)
LDFLAGS_BASE  = -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# ---- Normal build ----
CXXFLAGS_NORMAL = -O3 -flto=auto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti
LDFLAGS_NORMAL  = -Wl,--gc-sections
LDFLAGS_STRIP   = -Wl,--strip-all
VARIANT         = release

# ---- Sanitizer builds ----
ifeq ($(SANITIZE),1)
    # AddressSanitizer
    CXXFLAGS_EXTRA  = -fsanitize=address
    LDFLAGS_EXTRA   = -fsanitize=address
    CXXFLAGS_NORMAL = -O1 -g -fno-omit-frame-pointer
    LDFLAGS_NORMAL  =
    LDFLAGS_STRIP   =
    VARIANT         = asan
else ifeq ($(SANITIZE),2)
    # ThreadSanitizer
    CXXFLAGS_EXTRA  = -fsanitize=thread
    LDFLAGS_EXTRA   = -fsanitize=thread
    CXXFLAGS_NORMAL = -O1 -g -fno-omit-frame-pointer
    LDFLAGS_NORMAL  =
    LDFLAGS_STRIP   =
    VARIANT         = tsan
else ifeq ($(FAST),1)
    # Development build: skip -O3 and whole-program LTO, which dominate the
    # link step and are re-run in full after every edit. Output is a normal,
    # working (but larger and slower) binary kept in its own object tree.
    CXXFLAGS_NORMAL = -O0 -fno-plt -fno-rtti
    LDFLAGS_NORMAL  =
    VARIANT         = fast
endif

# ---------- Third-party / system libraries ----------
CHD_STATIC = $(DEPS_DIR)/libchdr-static.a
CHD_INC    = $(DEPS_DIR)/libchdr/include

# Dynamic (default)
LIBS_DYN = -lreadline -lmount -lblkid $(CHD_STATIC)

# Static (STATIC=1)
LZMA_STATIC     = /usr/lib/liblzma.a
ZLIB_STATIC     = /usr/lib/libz.a
ZSTD_STATIC     = /usr/lib/libzstd.a
READLINE_STATIC = /usr/lib/libreadline.a
NCURSES_STATIC  = /usr/lib/libncurses.a
MOUNT_STATIC    = /usr/lib/libmount.a
BLKID_STATIC    = /usr/lib/libblkid.a
ECONF_STATIC    = /usr/lib/libeconf.a
INTL_STATIC     = /usr/lib/libintl.a
LIBS_STAT = $(CHD_STATIC) \
            $(LZMA_STATIC) $(ZLIB_STATIC) $(ZSTD_STATIC) \
            $(READLINE_STATIC) $(NCURSES_STATIC) $(MOUNT_STATIC) $(BLKID_STATIC) \
            $(ECONF_STATIC) $(INTL_STATIC) -pthread

# ---------- Select build type ----------
ifeq ($(STATIC),1)
    LIBS         = $(LIBS_STAT)
    LDFLAGS_LINK = -static
    VARIANT     := $(VARIANT)-static
else
    LIBS         = $(LIBS_DYN)
    LDFLAGS_LINK =
endif

# Assigned (not appended) so environment CXXFLAGS/LDFLAGS (e.g. from makepkg)
# do not alter the tuned flags above, matching the previous Makefile.
CPPFLAGS = -I$(CHD_INC) -MMD -MP
# The generated moddeps.mk owns the named-module dependency graph.
# GCC's synthetic *.c++-module / *.c++m targets have no Make rules here.
CPPFLAGS += -Mno-modules

CXXFLAGS = $(CXXFLAGS_BASE) $(CXXFLAGS_NORMAL) $(CXXFLAGS_EXTRA)
LDFLAGS  = $(LDFLAGS_LINK) $(LDFLAGS_BASE) $(LDFLAGS_NORMAL) $(LDFLAGS_EXTRA) $(LDFLAGS_STRIP)

# ---------- Sources (auto-discovered, one object tree per build variant) ----------
OBJ_DIR  := $(BUILD_DIR)/$(VARIANT)
SRCS     := $(sort $(shell find $(SRC_DIR) -name '*.cpp'))
MOD_DIR  := modules
MODSRCS  := $(sort $(shell find $(MOD_DIR) -name '*.cppm'))
OBJS     := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o) $(MODSRCS:$(MOD_DIR)/%.cppm=$(OBJ_DIR)/mod/%.o)
DEPFILES := $(OBJS:.o=.d)

# ---------- C++23 modules ----------
# modules/isocmd/a/b.cppm  ->  module isocmd.a.b  ->  $(GCM_DIR)/isocmd.a.b.gcm
# GCC finds each compiled module interface through a generated module mapper.
GCM_DIR := $(OBJ_DIR)/gcm
MAPPER  := $(OBJ_DIR)/modules.map
MOD_NAMES := $(foreach m,$(MODSRCS),$(subst /,.,$(patsubst $(MOD_DIR)/%.cppm,%,$(m))))

# Native standard library named module. Its installed source is found by GCC
# using the active compiler's own include path (no hard-coded GCC version).
# Keep the compiler-owned module outside BUILD_DIR so ordinary clean builds
# can reuse it. Key by compiler identity, compile flags and include environment;
# -MD below additionally tracks the actual installed headers and std.cc.
STD_CACHE_ROOT := .std-cache
STD_CACHE_KEY := $(shell { \
    printf '%s\n' '$(CXX)' '$(GCC_VERSION)' '$(CPPFLAGS)' '$(CXXFLAGS)' \
        '$(CPATH)' '$(CPLUS_INCLUDE_PATH)' '$(C_INCLUDE_PATH)' '$(GCC_EXEC_PREFIX)' '$(COMPILER_PATH)' '$(PATH)'; \
    $(CXX) -dumpmachine; \
    stat -Lc '%n %d %i %s %y %z' "$$(command -v $(firstword $(CXX)))" "$$($(CXX) -print-prog-name=cc1plus)"; \
    } | sha256sum | cut -d' ' -f1)
STD_CACHE_DIR := $(STD_CACHE_ROOT)/$(VARIANT)/$(STD_CACHE_KEY)
STD_MODULE_OBJ := $(STD_CACHE_DIR)/std.o
STD_MODULE_GCM := $(STD_CACHE_DIR)/std.gcm
STD_MODULE_DEP := $(STD_CACHE_DIR)/std.d
STD_MODULE_MAPPER := $(STD_CACHE_DIR)/std-module.map

# A changed compiler, build mode or flag set invalidates old objects and CMIs.
# The stamp is rewritten only when the configuration actually changes.
CONFIG_STAMP := $(OBJ_DIR)/compile.config
.PHONY: FORCE
FORCE:
$(CONFIG_STAMP): FORCE Makefile
	@mkdir -p $(@D)
	@{ printf '%s\n' '$(CXX)' '$(shell $(CXX) --version 2>/dev/null | head -n 1)' '$(CPPFLAGS)' '$(CXXFLAGS)' '$(LDFLAGS)' '$(LIBS)' '$(STD_CACHE_KEY)' '$(MOD_NAMES)'; } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

# The executable is shared by the release/static/sanitizer build trees. Relink
# when switching variants even if the newly selected objects are older.
LINK_STAMP := $(BUILD_DIR)/link.config
$(LINK_STAMP): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(OBJ_DIR)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@
MODFLAGS := -fmodule-mapper=$(MAPPER)

# Publish dependency files only after a successful compile. A failed compiler
# invocation may still emit a dependency file; never include that partial output.
DEPFLAGS = -MF $(@:.o=.d).tmp -MT $@

define publish_depfile
	$(Q)mv -f $(@:.o=.d).tmp $(@:.o=.d)
endef

# Import dependencies, scanned from the sources: every file waits for the .gcm
# of each module it imports, and every .gcm is a by-product of its .cppm object.
# Both implementation and interface edits can change the import graph.
# Refresh its timestamp even if the graph is unchanged, avoiding repeated scans.
# One awk process reads every source, avoiding per-file shell pipelines.
# Prioritize independent interfaces during std compilation, then rank the
# remaining work by estimated downstream critical path plus unique transitive
# fan-out. Source line counts approximate cost; the graph and link order are
# unchanged. Large independent implementation files retain high priority.
MODDEPS_MK := $(OBJ_DIR)/moddeps.mk
$(MODDEPS_MK): $(MODSRCS) $(SRCS) Makefile $(CONFIG_STAMP)
	@mkdir -p $(@D)
	@set -e; tmp="$$(mktemp)"; trap 'rm -f "$$tmp"' EXIT; \
	LC_ALL=C awk -v obj="$(OBJ_DIR)" -v gcm="$(GCM_DIR)" \
	    -v stdgcm="$(STD_MODULE_GCM)" -v src="$(SRC_DIR)/" -v mod="$(MOD_DIR)/" '\
	  function emit(    n,o,g,i,k,dep) { \
	    if (file == "") return; \
	    if (index(file, mod) == 1) { \
	      n = substr(file, length(mod) + 1); sub(/\.cppm$$/, "", n); \
	      o = obj "/mod/" n ".o"; gsub(/\//, ".", n); g = gcm "/" n ".gcm"; \
	      printf "%s: %s\n\t@:\n", g, o; \
	      printf "ifeq ($$(wildcard %s),)\n%s: FORCE\nendif\n", g, o; \
	    } else { \
	      n = substr(file, length(src) + 1); sub(/\.cpp$$/, "", n); \
	      o = obj "/" n ".o"; \
	    } \
	    printf "%s: %s\n", o, deps; \
	    objects[++count] = o; cost[o] = 1 + lines / 200; \
	    if (g != "") { module_gcm[o] = g; if (deps == "") root_modules[o] = 1; } \
	    n = split(deps, imports, /[[:space:]]+/); \
	    for (i = 1; i <= n; ++i) { \
	      dep = imports[i]; if (dep == "") continue; \
	      k = dep SUBSEP o; if (!edge[k]++) consumers[dep, ++degree[dep]] = o; \
	    } \
	  } \
	  function rank(o,    g,i,child,best,r) { \
	    if (o in ranks) return ranks[o]; \
	    if (visiting[o]) { print "Cyclic module imports: " o > "/dev/stderr"; exit 1; } \
	    visiting[o] = 1; best = 0; g = module_gcm[o]; \
	    if (g != "") for (i = 1; i <= degree[g]; ++i) { \
	      child = consumers[g,i]; r = rank(child); if (r > best) best = r; \
	    } \
	    delete visiting[o]; return ranks[o] = cost[o] + best; \
	  } \
	  function downstream(root,o,    g,i,child,total,k) { \
	    k = root SUBSEP o; if (seen[k]++) return 0; \
	    g = module_gcm[o]; total = 1; \
	    if (g != "") for (i = 1; i <= degree[g]; ++i) { \
	      child = consumers[g,i]; total += downstream(root,child); \
	    } \
	    return total; \
	  } \
	  BEGIN { \
	    for (i = 1; i < ARGC; ++i) { \
	      file = ARGV[i]; deps = ""; lines = 0; \
	      while ((status = (getline line < file)) > 0) { \
	        ++lines; \
	        if (line !~ /^[[:space:]]*(export[[:space:]]+)?import[[:space:]]+(std|isocmd[A-Za-z0-9_.]*);/) continue; \
	        name = line; sub(/^[[:space:]]*(export[[:space:]]+)?import[[:space:]]+/, "", name); \
	        sub(/;.*/, "", name); \
	        deps = deps (name == "std" ? stdgcm : gcm "/" name ".gcm") " "; \
	      } \
	      if (status < 0) exit 1; \
	      close(file); emit(); \
	    } \
	    for (i = 1; i <= count; ++i) { \
	      o = objects[i]; rank(o); \
	      reach[o] = downstream(o,o) - 1; \
	      priority[o] = ranks[o] + 0.15 * reach[o]; \
	    } \
	    printf "SCHEDULED_OBJS :="; \
	    for (i = 1; i <= count; ++i) { \
	      best = ""; \
	      for (j = 1; j <= count; ++j) { \
	        o = objects[j]; if (used[o]) continue; \
	        if (best == "" || priority[o] > priority[best] || \
	            (priority[o] == priority[best] && reach[o] > reach[best])) best = o; \
	      } \
	      used[best] = 1; printf " %s", best; \
	    } \
	    printf "\nROOT_MODULE_OBJS :="; \
	    for (i = 1; i <= count; ++i) if (root_modules[objects[i]]) printf " %s", objects[i]; \
	    printf "\n"; \
	  }' $(MODSRCS) $(SRCS) > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$$tmp" "$@"; then \
	  rm -f "$$tmp" && touch "$@"; \
	else \
	  mv -f "$$tmp" "$@"; \
	fi

$(MAPPER): Makefile $(CONFIG_STAMP) $(STD_MODULE_MAPPER)
	@mkdir -p $(@D) $(GCM_DIR)
	@set -e; tmp="$$(mktemp)"; trap 'rm -f "$$tmp"' EXIT; \
	{ $(foreach n,$(MOD_NAMES),echo "$(n) $(GCM_DIR)/$(n).gcm";) \
	  cat "$(STD_MODULE_MAPPER)"; \
	} > "$$tmp"; \
	if [ -f "$@" ] && cmp -s "$$tmp" "$@"; then \
	  rm -f "$$tmp" && touch "$@"; \
	else \
	  mv -f "$$tmp" "$@"; \
	fi

# Build the actual libstdc++ module once; its object is linked exactly once.
# -MD tracks installed standard-library sources and headers as well.
$(STD_MODULE_MAPPER): Makefile
	@mkdir -p $(@D)
	@{ printf '%s %s\n' std "$(STD_MODULE_GCM)"; } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

$(STD_MODULE_OBJ) $(STD_MODULE_GCM) &: Makefile $(STD_MODULE_MAPPER)
	@echo "  MOD     std (libstdc++)"
	@mkdir -p $(STD_CACHE_DIR)
	$(Q)rm -f $(STD_MODULE_GCM) $(STD_MODULE_OBJ)
	$(Q)set -e; trap 'rm -f $(STD_MODULE_GCM) $(STD_MODULE_OBJ) $(STD_MODULE_DEP).tmp' EXIT; \
	$(CXX) $(filter-out -MMD -MP,$(CPPFLAGS)) -MD -MP -MF $(STD_MODULE_DEP).tmp -MT $(STD_MODULE_OBJ) -MT $(STD_MODULE_GCM) -fmodule-mapper=$(STD_MODULE_MAPPER) $(CXXFLAGS) -fsearch-include-path -x c++ -c bits/std.cc -o $(STD_MODULE_OBJ); \
	mv -f $(STD_MODULE_DEP).tmp $(STD_MODULE_DEP); trap - EXIT
ifeq ($(wildcard $(STD_MODULE_DEP)),)
$(STD_MODULE_OBJ) $(STD_MODULE_GCM): FORCE
endif
-include $(STD_MODULE_DEP)

# ---------- Targets ----------
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all clean cache install uninstall help
# Read the generated priority order before expanding the link prerequisites.
# Do not regenerate the project dependency graph just to delete it.
ifeq ($(strip $(MAKECMDGOALS)),)
-include $(MODDEPS_MK)
else ifneq ($(strip $(filter-out clean cache help uninstall,$(MAKECMDGOALS))),)
-include $(MODDEPS_MK)
endif
# Preserve all current objects even on the initial parse with no graph yet.
BUILD_OBJS = $(filter $(OBJS),$(SCHEDULED_OBJS)) $(filter-out $(SCHEDULED_OBJS),$(OBJS))
# Launch the native std build first, then independent interfaces, then the
# ranked importer graph.
EARLY_OBJS := $(filter $(ROOT_MODULE_OBJS),$(BUILD_OBJS))
all: $(TARGET)

$(TARGET): $(STD_MODULE_OBJ) $(STD_MODULE_GCM) $(EARLY_OBJS) $(filter-out $(EARLY_OBJS),$(BUILD_OBJS)) $(LINK_STAMP)
	@echo "  LD      $@"
	$(Q)$(CXX) $(LDFLAGS) $(STD_MODULE_OBJ) $(OBJS) -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp Makefile $(CONFIG_STAMP) | $(MAPPER)
	@echo "  CXX     $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -c $< -o $@
	$(publish_depfile)

# Module interface units: GCC emits the .o and .gcm in one step.
$(OBJ_DIR)/mod/%.o: $(MOD_DIR)/%.cppm Makefile $(CONFIG_STAMP) | $(MAPPER)
	@echo "  MOD     $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@
	$(publish_depfile)

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

cache:
	rm -rf $(STD_CACHE_ROOT)

install: $(TARGET)
	mkdir -p $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

help:
	@echo "Targets:  all (default)  clean  cache   install  uninstall  help"
	@echo "Options:  STATIC=1  SANITIZE=1|2  FAST=1  V=1  BINDIR=<dir>  BUILD_DIR=<dir>"
	@echo "Compiler: $(CXX)"

# Recover dependency files left by older Makefiles after failed GCC builds.
# Ignore only files with synthetic module targets, and rebuild their objects
# once so normal header tracking is restored without a clean rebuild.
EXISTING_DEPFILES := $(wildcard $(DEPFILES))
BAD_DEPFILES := $(if $(EXISTING_DEPFILES),$(shell grep -lE '\.c\+\+(-module|m)([[:space:]:]|$$)' $(EXISTING_DEPFILES) || :))
ifneq ($(strip $(BAD_DEPFILES)),)
.PHONY: repair-depfile
$(BAD_DEPFILES:.d=.o): repair-depfile
endif

# Header dependency tracking (generated by -MMD)
-include $(filter-out $(BAD_DEPFILES),$(DEPFILES))
