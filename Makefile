# ============================================================================
#  iso-commander Makefile
#
#  Layout:
#    modules/isocmd/<dir>/*.cppm   C++23 module interfaces (module isocmd.<dir>.<name>)
#    libstdc++ bits/std.cc        compiler-provided standard library module (import std;)
#    src/<module>/*.cpp            implementation, `import`s the modules above
#    deps/                         vendored third-party code (libchdr)
#    tests/                        native C++ regression tests and shell runners
#    build/<variant>/              object + dependency files (generated)
#    isocmd                        dynamic executable (generated at the root)
#    isocmd_static                 static executable (generated at the root)
#
#  Usage:
#    make                  optimized dynamic build (isocmd)
#    make STATIC=1         fully static build (isocmd_static)
#    make SANITIZE=1       AddressSanitizer build
#    make SANITIZE=2       ThreadSanitizer build
#    make FAST=1           quick development build: -O0, no LTO (separate build/fast tree).
#                          Much quicker edit-compile-link cycles; not for release use.
#    make UNITY=0          compile each source file separately (default with FAST=1).
#    make UNITY=1          hybrid unity: combine small files within each directory
#                          (default regardless of processor count).
#    make UNITY=2          full directory-based unity compilation.
#    make -jN              limit parallel jobs without changing unity groups.
#
#  Requires GCC 15+ with libstdc++'s C++23 standard library module.
#  Native std is cached outside the ordinary build tree.
#  std cache entries survive unrelated Makefile edits; the key tracks the
#  actual std compilation configuration and installed header dependencies.
#    make clean                   discard build output, preserve standard-library cache
#    make clean cache             discard only the persistent standard-library cache
#    make clean all               discard build output and the persistent standard-library cache
#    make nocache                 build without the persistent std cache; rebuild std each time
#    make test-unit                   safe unit and CLI regression tests
#    make test-integration       unprivileged filesystem, cancellation, USB, import and PTY tests
#    make test-privileged        build as user, run loop-device, mount and CLI tests via sudo
#    make test-unit SANITIZE=1         run unit/CLI tests with AddressSanitizer
#    make test-unit SANITIZE=2         run unit/CLI tests with ThreadSanitizer
#    make V=1                     show full compiler command lines
#    make install [BINDIR=/some/dir]
#
#  Tests require no Python or external C++ framework; see tests/README.md.
# ============================================================================

# Reject sudo test builds before included dependency makefiles can be rebuilt.
# Only the privileged runner, never the compiler or cache recipes, needs root.
ifneq ($(filter test-unit test-integration test-privileged,$(MAKECMDGOALS)),)
ifeq ($(shell id -u),0)
    $(error Run test targets as a regular user; make test-privileged invokes sudo only for the runner)
endif
endif

# ---------- Parallelism ----------
NUM_PROCESSORS := $(shell nproc)
ifeq ($(filter -j%, $(MAKEFLAGS)),)
    MAKEFLAGS += -j$(NUM_PROCESSORS)
endif

# ---------- Project ----------
TARGET_DYN  := isocmd
TARGET_STAT := isocmd_static
SRC_DIR  := src
DEPS_DIR := deps
BUILD_DIR ?= build
BINDIR   ?= bin

STD_CACHE_ROOT := .std-cache

# Cleanup-only invocations never load or regenerate dependency files.
# With clean, cache and all are cleanup selectors, not build goals.
ifneq ($(filter cache,$(MAKECMDGOALS)),)
ifeq ($(filter clean,$(MAKECMDGOALS)),)
    $(error Use make clean cache to remove only the persistent standard-library cache)
endif
endif

ifneq ($(filter clean,$(MAKECMDGOALS)),)
ifneq ($(filter-out clean cache all,$(MAKECMDGOALS)),)
    $(error Combine clean only with cache or all)
endif
ifneq ($(filter cache,$(MAKECMDGOALS)),)
ifneq ($(filter all,$(MAKECMDGOALS)),)
    $(error Use either make clean cache or make clean all)
endif
endif
.DEFAULT_GOAL := clean
.PHONY: clean cache all

clean:
ifneq ($(filter all,$(MAKECMDGOALS)),)
	rm -rf $(BUILD_DIR) $(TARGET_DYN) $(TARGET_STAT) $(STD_CACHE_ROOT)
else ifneq ($(filter cache,$(MAKECMDGOALS)),)
	rm -rf $(STD_CACHE_ROOT)
else
	rm -rf $(BUILD_DIR) $(TARGET_DYN) $(TARGET_STAT)
endif

cache all: clean
	@:

else

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
    TARGET       := $(TARGET_STAT)
    LIBS         = $(LIBS_STAT)
    LDFLAGS_LINK = -static
    VARIANT     := $(VARIANT)-static
else
    TARGET       := $(TARGET_DYN)
    LIBS         = $(LIBS_DYN)
    LDFLAGS_LINK =
endif

# Keep uncached objects and CMIs separate from the normal build variants.
ifneq ($(filter nocache,$(MAKECMDGOALS)),)
    VARIANT := $(VARIANT)-nocache
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

# ---------- Combined compilation of implementation files ----------
# Sharing translation units reduces repeated import std and template overhead.
# Hybrid unity bounds group sizes to preserve independent compilation jobs.
# Grouping is deterministic and independent of processor count and -j.
# UNITY=0: per-file (default with FAST=1); 1: hybrid (default); 2: full directory.
UNITY ?= $(if $(filter 1,$(FAST)),0,1)
ifneq ($(words $(UNITY)),1)
    $(error UNITY must be 0, 1 or 2)
endif
ifeq ($(filter 0 1 2,$(UNITY)),)
    $(error UNITY must be 0, 1 or 2)
endif

# Hybrid limits: source lines are a cheap approximation of compilation cost.
UNITY_MAX_FILES := 4
UNITY_MAX_LINES := 2000
UNITY_STANDALONE_LINES := 1000

# Always compiled on their own:
#   app/main.cpp           the test targets replace it
#   converters/daa2iso.cpp defines many short macros that would leak into others
UNITY_EXCLUDE := $(SRC_DIR)/app/main.cpp $(SRC_DIR)/converters/daa2iso.cpp

ifneq ($(UNITY),0)
    UNITY_CANDIDATES := $(filter-out $(UNITY_EXCLUDE),$(SRCS))
    # Assign sorted sources to bounded groups within their immediate directory.
    # Large files are skipped rather than breaking the current small-file group.
    # Singleton groups remain normal objects. Name groups after their first member
    # and use distinct paths for hybrid/full to avoid mixing old objects.
    UNITY_ASSIGNMENTS := $(shell LC_ALL=C awk -v src="$(SRC_DIR)/" -v mode="$(UNITY)" \
        -v maxfiles="$(UNITY_MAX_FILES)" -v maxlines="$(UNITY_MAX_LINES)" \
        -v standalone="$(UNITY_STANDALONE_LINES)" '\
      BEGIN { \
        for (i = 1; i < ARGC; ++i) { \
          file = ARGV[i]; rel = substr(file, length(src) + 1); \
          dir = rel; if (dir !~ /\//) dir = "."; else sub(/\/[^\/]*$$/, "", dir); \
          lines = 0; \
          if (mode == 1) { \
            while ((status = (getline line < file)) > 0) ++lines; \
            if (status < 0) { print "Cannot read " file > "/dev/stderr"; exit 1; } \
            close(file); if (lines >= standalone || lines > maxlines) continue; \
          } \
          g = active[dir]; \
          if (g == "" || (mode == 1 && (count[g] >= maxfiles || total[g] + lines > maxlines))) { \
            sub(/\.cpp$$/, "", rel); g = (mode == 1 ? "hybrid/" : "full/") rel; \
            active[dir] = g; \
          } \
          ++count[g]; total[g] += lines; assigned[file] = g; \
        } \
        for (i = 1; i < ARGC; ++i) { \
          file = ARGV[i]; g = assigned[file]; \
          if (g != "" && count[g] >= 2) printf "%s=%s ", file, g; \
        } \
      }' $(UNITY_CANDIDATES))
    ifneq ($(.SHELLSTATUS),0)
        $(error Failed to generate unity groups)
    endif
endif
UNITY_GROUPS := $(sort $(foreach pair,$(UNITY_ASSIGNMENTS),$(word 2,$(subst =, ,$(pair)))))
group_srcs = $(patsubst %=$1,%,$(filter %=$1,$(UNITY_ASSIGNMENTS)))
UNITY_SRCS := $(foreach g,$(UNITY_GROUPS),$(call group_srcs,$g))
UNITY_OBJS := $(UNITY_GROUPS:%=$(OBJ_DIR)/unity/%.o)
# source=object pairs, used by the import dependency scan below.
UNITY_MAP  := $(foreach g,$(UNITY_GROUPS),$(foreach s,$(call group_srcs,$g),$s=$(OBJ_DIR)/unity/$g.o))

OBJS     := $(patsubst $(SRC_DIR)/%.cpp,$(OBJ_DIR)/%.o,$(filter-out $(UNITY_SRCS),$(SRCS))) \
            $(UNITY_OBJS) $(MODSRCS:$(MOD_DIR)/%.cppm=$(OBJ_DIR)/mod/%.o)
DEPFILES := $(OBJS:.o=.d)
TEST_SCAN_SRCS := $(sort $(shell find tests -name '*.cpp' 2>/dev/null))

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
# Bump the recipe revision if the std build procedure changes incompatibly.
# Unlike a Makefile timestamp, this does not invalidate std for unrelated edits.
STD_RECIPE_REV := 1
STD_COMPILE_FLAGS := $(filter-out -MMD -MP,$(CPPFLAGS)) $(CXXFLAGS)
STD_CACHE_KEY := $(shell { \
    printf '%s\n' '$(STD_RECIPE_REV)' '$(CXX)' '$(GCC_VERSION)' '$(STD_COMPILE_FLAGS)' \
        '$(CPATH)' '$(CPLUS_INCLUDE_PATH)' '$(C_INCLUDE_PATH)' '$(GCC_EXEC_PREFIX)' '$(COMPILER_PATH)' '$(PATH)'; \
    $(CXX) -dumpmachine; \
    stat -Lc '%n %d %i %s %y %z' "$$(command -v $(firstword $(CXX)))" "$$($(CXX) -print-prog-name=cc1plus)"; \
    } | sha256sum | cut -d' ' -f1)
ifneq ($(filter nocache,$(MAKECMDGOALS)),)
    STD_CACHE_DIR := $(OBJ_DIR)/std/$(STD_CACHE_KEY)
else
    STD_CACHE_DIR := $(STD_CACHE_ROOT)/$(VARIANT)/$(STD_CACHE_KEY)
endif
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

# Source membership and unity grouping, kept apart from CONFIG_STAMP so
# adding/removing a source does not invalidate every object. Also track test
# membership for graph regeneration and relinking after source removal.
UNITY_STAMP := $(OBJ_DIR)/unity.config
$(UNITY_STAMP): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(UNITY_MAP)' '$(SRCS)' '$(TEST_SCAN_SRCS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

# Each linkage type has its own executable and link stamp. Relink only
# when switching variants that share that executable (e.g. release and fast).
LINK_STAMP := $(BUILD_DIR)/link-$(TARGET).config
$(LINK_STAMP): FORCE
	@mkdir -p $(@D)
	@printf '%s\n' '$(OBJ_DIR)' '$(OBJS)' '$(UNITY_MAP)' > $@.tmp
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
$(MODDEPS_MK): $(MODSRCS) $(SRCS) $(TEST_SCAN_SRCS) Makefile $(CONFIG_STAMP) $(UNITY_STAMP)
	@mkdir -p $(@D)
	@set -e; tmp="$$(mktemp)"; trap 'rm -f "$$tmp"' EXIT; \
	LC_ALL=C awk -v obj="$(OBJ_DIR)" -v gcm="$(GCM_DIR)" \
	    -v stdgcm="$(STD_MODULE_GCM)" -v src="$(SRC_DIR)/" -v mod="$(MOD_DIR)/" \
	    -v units="$(UNITY_MAP)" '\
	  function emit(    n,o,g,i,k,dep) { \
	    if (file == "") return; \
	    if (index(file, mod) == 1) { \
	      n = substr(file, length(mod) + 1); sub(/\.cppm$$/, "", n); \
	      o = obj "/mod/" n ".o"; gsub(/\//, ".", n); g = gcm "/" n ".gcm"; \
	      printf "%s: %s\n\t@:\n", g, o; \
	      printf "ifeq ($$(wildcard %s),)\n%s: FORCE\nendif\n", g, o; \
	    } else if (index(file, "tests/") == 1) { \
	      n = file; sub(/\.cpp$$/, "", n); \
	      printf "%s/%s.o: %s\n", obj, n, deps; \
	      return; \
	    } else { \
	      n = substr(file, length(src) + 1); sub(/\.cpp$$/, "", n); \
	      o = (file in unit) ? unit[file] : obj "/" n ".o"; \
	      if (n == "app/main") printf "%s/tests/menuApp.o: %s\n", obj, deps; \
	    } \
	    printf "%s: %s\n", o, deps; \
	    if (!(o in cost)) objects[++count] = o; \
	    cost[o] += 1 + lines / 200; \
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
	    n = split(units, pairs, /[[:space:]]+/); \
	    for (i = 1; i <= n; ++i) if (split(pairs[i], kv, "=") == 2) unit[kv[1]] = kv[2]; \
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
	  }' $(MODSRCS) $(SRCS) $(TEST_SCAN_SRCS) > "$$tmp"; \
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

# Build libstdc++ once per cached configuration, or on every nocache build.
# Its object is linked exactly once.
# -MD tracks installed standard-library sources and headers as well.
$(STD_MODULE_MAPPER):
	@mkdir -p $(@D)
	@{ printf '%s %s\n' std "$(STD_MODULE_GCM)"; } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

$(STD_MODULE_OBJ) $(STD_MODULE_GCM) &: $(STD_MODULE_MAPPER)
	@echo "  MOD     std (libstdc++)"
	@mkdir -p $(STD_CACHE_DIR)
	$(Q)rm -f $(STD_MODULE_GCM) $(STD_MODULE_OBJ)
	$(Q)set -e; trap 'rm -f $(STD_MODULE_GCM) $(STD_MODULE_OBJ) $(STD_MODULE_DEP).tmp' EXIT; \
	$(CXX) $(STD_COMPILE_FLAGS) -MD -MP -MF $(STD_MODULE_DEP).tmp -MT $(STD_MODULE_OBJ) -MT $(STD_MODULE_GCM) -fmodule-mapper=$(STD_MODULE_MAPPER) -fsearch-include-path -x c++ -c bits/std.cc -o $(STD_MODULE_OBJ); \
	mv -f $(STD_MODULE_DEP).tmp $(STD_MODULE_DEP); trap - EXIT
ifneq ($(filter nocache,$(MAKECMDGOALS)),)
$(STD_MODULE_OBJ) $(STD_MODULE_GCM): FORCE
else ifeq ($(wildcard $(STD_MODULE_DEP)),)
$(STD_MODULE_OBJ) $(STD_MODULE_GCM): FORCE
endif
-include $(STD_MODULE_DEP)

# ---------- Targets ----------
.DEFAULT_GOAL := all
.DELETE_ON_ERROR:
.PHONY: all nocache install uninstall help test-unit test-integration test-privileged
# Read the generated priority order before expanding the link prerequisites.
# Do not regenerate the project dependency graph just to delete it.
ifeq ($(strip $(MAKECMDGOALS)),)
-include $(MODDEPS_MK)
else ifneq ($(strip $(filter-out help uninstall,$(MAKECMDGOALS))),)
-include $(MODDEPS_MK)
endif
# Preserve all current objects even on the initial parse with no graph yet.
BUILD_OBJS = $(filter $(OBJS),$(SCHEDULED_OBJS)) $(filter-out $(SCHEDULED_OBJS),$(OBJS))
# Launch the native std build first, then independent interfaces, then the
# ranked importer graph.
EARLY_OBJS := $(filter $(ROOT_MODULE_OBJS),$(BUILD_OBJS))
all: $(TARGET)
nocache: all

$(TARGET): $(STD_MODULE_OBJ) $(STD_MODULE_GCM) $(EARLY_OBJS) $(filter-out $(EARLY_OBJS),$(BUILD_OBJS)) $(LINK_STAMP)
	@echo "  LD      $@"
	$(Q)$(CXX) $(LDFLAGS) $(STD_MODULE_OBJ) $(OBJS) -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp Makefile $(CONFIG_STAMP) | $(MAPPER)
	@echo "  CXX     $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -c $< -o $@
	$(publish_depfile)

# Generated sources that include each group. GCC rejects a textual system header
# that first appears after `import std;`, so every member's #include <...> lines
# are repeated at the top, ahead of any import (include guards make the later
# copies no-ops). Rewritten only when that text changes, so adding or removing
# a file or an include is picked up automatically.
# -MMD records the included files, so editing any member rebuilds its group.
$(UNITY_OBJS:.o=.cpp): $(OBJ_DIR)/unity/%.cpp: FORCE
	@mkdir -p $(@D)
	@{ awk '/^#include[ \t]*</ && !seen[$$0]++' $(call group_srcs,$*); \
	   printf '#include "%s"\n' $(call group_srcs,$*); } > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv -f $@.tmp $@

$(UNITY_OBJS): $(OBJ_DIR)/unity/%.o: $(OBJ_DIR)/unity/%.cpp Makefile $(CONFIG_STAMP) | $(MAPPER)
	@echo "  CXX     $(firstword $(call group_srcs,$*)) (combined, $(words $(call group_srcs,$*)) files)"
	$(Q)$(CXX) $(CPPFLAGS) -iquote . $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -c $< -o $@
	$(publish_depfile)

# Direct membership dependencies also cover the first build or a missing .d.
$(foreach g,$(UNITY_GROUPS),$(eval $(OBJ_DIR)/unity/$g.o: $(call group_srcs,$g)))

# Module interface units: GCC emits the .o and .gcm in one step.
$(OBJ_DIR)/mod/%.o: $(MOD_DIR)/%.cppm Makefile $(CONFIG_STAMP) | $(MAPPER)
	@echo "  MOD     $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -x c++ -c $< -o $@
	$(publish_depfile)

# ---------- Regression tests ----------
# Reuse production objects and the same module mapper/flags; never link the
# application entry point into the unit-test executable. Tests stay outside src/.
TEST_UNIT_SRCS := tests/main.cpp $(sort $(wildcard tests/unit/*.cpp))
TEST_INTEGRATION_SRCS := $(sort $(wildcard tests/integration/*.cpp))
TEST_PRIVILEGED_SRCS := $(sort $(wildcard tests/privileged/*.cpp))
TEST_UNIT_OBJS := $(TEST_UNIT_SRCS:%.cpp=$(OBJ_DIR)/%.o)
TEST_INTEGRATION_OBJS := $(TEST_INTEGRATION_SRCS:%.cpp=$(OBJ_DIR)/%.o)
TEST_PRIVILEGED_OBJS := $(TEST_PRIVILEGED_SRCS:%.cpp=$(OBJ_DIR)/%.o)
TEST_FAULT_OBJ := $(OBJ_DIR)/tests/common/ioFaults.o
TEST_FAULT_WRAP := -Wl,--wrap=read,--wrap=write,--wrap=pwrite,--wrap=fsync,--wrap=sync_file_range,--wrap=posix_fallocate,--wrap=ftruncate,--wrap=close
TEST_OBJS := $(TEST_UNIT_OBJS) $(TEST_INTEGRATION_OBJS) $(TEST_PRIVILEGED_OBJS) $(TEST_FAULT_OBJ)
TEST_BIN := $(OBJ_DIR)/tests/isocmd-tests
TEST_INTEGRATION_BIN := $(OBJ_DIR)/tests/isocmd-integration-tests
TEST_MENU_OBJ := $(OBJ_DIR)/tests/menuApp.o
TEST_MENU_BIN := $(OBJ_DIR)/tests/isocmd-menu-test
TEST_PRIVILEGED_BIN := $(OBJ_DIR)/tests/isocmd-privileged-tests
TEST_LIB_OBJS := $(filter-out $(OBJ_DIR)/app/main.o,$(OBJS))
# All test sources import std through testHarness.h (directly or via ioFaults.h).
# Project module prerequisites are generated per source in moddeps.mk.

$(OBJ_DIR)/tests/%.o: tests/%.cpp tests/common/testHarness.h Makefile $(CONFIG_STAMP) $(STD_MODULE_GCM) | $(MAPPER)
	@echo "  TEST CXX $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -c $< -o $@
	$(publish_depfile)

$(TEST_BIN): $(TEST_UNIT_OBJS) $(TEST_LIB_OBJS) $(STD_MODULE_OBJ) $(UNITY_STAMP)
$(TEST_INTEGRATION_BIN): $(TEST_INTEGRATION_OBJS) $(TEST_FAULT_OBJ) $(TEST_LIB_OBJS) $(STD_MODULE_OBJ) $(UNITY_STAMP)
$(TEST_PRIVILEGED_BIN): $(TEST_PRIVILEGED_OBJS) $(TEST_FAULT_OBJ) $(TEST_LIB_OBJS) $(STD_MODULE_OBJ) $(UNITY_STAMP)
# The unity stamp also forces relinking when the production object list changes.
# forkpty is in libc on modern glibc; -lutil also supports older glibc.
# Only the integration executable needs it.
$(TEST_BIN) $(TEST_INTEGRATION_BIN) $(TEST_PRIVILEGED_BIN):
	@echo "  TEST LD  $@"
	$(Q)$(CXX) $(LDFLAGS) $(STD_MODULE_OBJ) $(TEST_LIB_OBJS) $(filter $(TEST_OBJS),$^) -o $@ $(LIBS) $(if $(filter $(TEST_INTEGRATION_BIN),$@),-lutil) $(if $(filter $(TEST_INTEGRATION_BIN) $(TEST_PRIVILEGED_BIN),$@),$(TEST_FAULT_WRAP))

test-unit: $(TARGET) $(TEST_BIN)
	$(Q)sh tests/run.sh "$(TEST_BIN)" "$(TARGET)"

$(TEST_MENU_OBJ): src/app/main.cpp Makefile $(CONFIG_STAMP) $(STD_MODULE_GCM) | $(MAPPER)
	@echo "  TEST CXX $< (private menu lock)"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(DEPFLAGS) $(MODFLAGS) $(CXXFLAGS) -DISOCMD_TEST_PRIVATE_LOCK -c $< -o $@
	$(publish_depfile)

$(TEST_MENU_BIN): $(TEST_MENU_OBJ) $(TEST_LIB_OBJS) $(STD_MODULE_OBJ) $(UNITY_STAMP)
	@echo "  TEST LD  $@"
	$(Q)$(CXX) $(LDFLAGS) $(STD_MODULE_OBJ) $(TEST_LIB_OBJS) $(TEST_MENU_OBJ) -o $@ $(LIBS)

test-integration: $(TEST_INTEGRATION_BIN) $(TEST_MENU_BIN)
	$(Q)sh tests/integration/run.sh "$(TEST_INTEGRATION_BIN)" "$(TEST_MENU_BIN)"

# Elevate only after all compilation and dependency generation have finished.
test-privileged: $(TEST_PRIVILEGED_BIN) $(TARGET)
	$(Q)sudo sh tests/privileged/run.sh "$(TEST_PRIVILEGED_BIN)" "$(TARGET)"

-include $(wildcard $(TEST_OBJS:.o=.d) $(TEST_MENU_OBJ:.o=.d))

install: $(TARGET)
	mkdir -p $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

help:
	@echo "Usage: make [target] [options]"
	@echo ""
	@echo "Build:"
	@echo "  make                       Build isocmd (default)"
	@echo "  make all                   Same as make"
	@echo "  make nocache               Rebuild std without the persistent cache"
	@echo "  make -jN                   Limit parallel jobs to N"
	@echo ""
	@echo "Testing:"
	@echo "  make test-unit             Unit and CLI regression tests"
	@echo "  make test-integration      Filesystem, cancellation, USB, import and PTY tests"
	@echo "  make test-privileged       Build as user, run loop-device, mount and CLI tests via sudo"
	@echo ""
	@echo "  Run all test targets without sudo."
	@echo "  Use SANITIZE=1 (ASan) or SANITIZE=2 (TSan) with any test suite."
	@echo "  See tests/README.md for requirements and safety."
	@echo ""
	@echo "Cleanup:"
	@echo "  make clean                 Remove build artifacts, preserve std cache"
	@echo "  make clean cache           Remove only the persistent std cache"
	@echo "  make clean all             Remove build artifacts and std cache"
	@echo ""
	@echo "Installation:"
	@echo "  make install               Install the application"
	@echo "  make uninstall             Uninstall the application"
	@echo ""
	@echo "Options:"
	@echo "  STATIC=1                   Build a static executable"
	@echo "  SANITIZE=1                 Enable AddressSanitizer (ASan)"
	@echo "  SANITIZE=2                 Enable ThreadSanitizer (TSan)"
	@echo "  FAST=1                     Enable fast development build (-O0, no LTO)"
	@echo "  UNITY=0|1|2                Per file (0), hybrid (1, default), or full directory (2)"
	@echo "                             FAST=1 defaults to UNITY=0"
	@echo "  V=1                        Enable verbose build output"
	@echo "  BINDIR=<dir>               Set installation directory"
	@echo "  BUILD_DIR=<dir>            Set build directory"
	@echo ""
	@echo "Output:"
	@echo "  isocmd                     Default executable"
	@echo "  isocmd_static              Static executable (STATIC=1)"
	@echo ""
	@echo "Help:"
	@echo "  make help                  Show this help"
	@echo ""
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

endif # cleanup dispatcher
