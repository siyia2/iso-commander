# ============================================================================
#  iso-commander Makefile
#
#  Layout:
#    include/isocmd/<module>/*.h   public/internal headers  (-Iinclude)
#    src/<module>/*.cpp            implementation, mirrors include/
#    deps/                         vendored third-party code (libchdr)
#    build/<variant>/              object + dependency files (generated)
#    isocmd                        final binary (generated, kept at the root)
#
#  Usage:
#    make                  optimized dynamic build
#    make STATIC=1         fully static build
#    make SANITIZE=1       AddressSanitizer build
#    make SANITIZE=2       ThreadSanitizer build
#    make PCH=0            disable the precompiled std header (GCC only; on by default)
#    make V=1              show full compiler command lines
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
INC_DIR  := include
DEPS_DIR := deps
BUILD_DIR ?= build
BINDIR   ?= bin

# ---------- Toolchain ----------
CXX ?= g++

# ---------- Verbosity ----------
ifeq ($(V),1)
    Q :=
else
    Q := @
endif

# ---------- Compiler and common flags ----------
CXXFLAGS_BASE = -std=c++20 -Wall -Wextra
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
CPPFLAGS = -I$(INC_DIR) -I$(CHD_INC) -MMD -MP
CXXFLAGS = $(CXXFLAGS_BASE) $(CXXFLAGS_NORMAL) $(CXXFLAGS_EXTRA)
LDFLAGS  = $(LDFLAGS_LINK) $(LDFLAGS_BASE) $(LDFLAGS_NORMAL) $(LDFLAGS_EXTRA) $(LDFLAGS_STRIP)

# ---------- Sources (auto-discovered, one object tree per build variant) ----------
OBJ_DIR  := $(BUILD_DIR)/$(VARIANT)
SRCS     := $(sort $(shell find $(SRC_DIR) -name '*.cpp'))
OBJS     := $(SRCS:$(SRC_DIR)/%.cpp=$(OBJ_DIR)/%.o)
DEPFILES := $(OBJS:.o=.d)

# ---------- Precompiled header (GCC only) ----------
# The std headers in include/isocmd/pch.h are parsed once per build variant and
# force-included into every translation unit. Flags are identical to normal
# compiles, as GCC requires. Disable with `make PCH=0` (or when using clang).
PCH ?= 1
PCH_SRC := $(INC_DIR)/isocmd/pch.h
PCH_DIR := $(OBJ_DIR)/pch
PCH_GCH := $(PCH_DIR)/pch.h.gch
ifneq ($(findstring clang,$(shell $(CXX) --version 2>/dev/null)),)
    PCH := 0
endif
ifeq ($(PCH),1)
    PCH_FLAGS := -I$(PCH_DIR) -include pch.h
    PCH_DEP   := $(PCH_GCH)
else
    PCH_FLAGS :=
    PCH_DEP   :=
endif

# ---------- Targets ----------
.PHONY: all clean install uninstall help
all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  LD      $@"
	$(Q)$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

# The header is copied next to its .gch so GCC finds it via -I$(PCH_DIR).
$(PCH_GCH): $(PCH_SRC)
	@echo "  PCH     $<"
	@mkdir -p $(PCH_DIR)
	@cp $< $(PCH_DIR)/pch.h
	$(Q)$(CXX) $(CPPFLAGS) $(CXXFLAGS) -x c++-header $(PCH_DIR)/pch.h -o $@

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp $(PCH_DEP)
	@echo "  CXX     $<"
	@mkdir -p $(@D)
	$(Q)$(CXX) $(CPPFLAGS) $(PCH_FLAGS) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(TARGET)

install: $(TARGET)
	mkdir -p $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)

uninstall:
	rm -f $(BINDIR)/$(TARGET)

help:
	@echo "Targets:  all (default)  clean  install  uninstall  help"
	@echo "Options:  STATIC=1  SANITIZE=1|2  PCH=0  V=1  BINDIR=<dir>  BUILD_DIR=<dir>"

# Header dependency tracking (generated by -MMD)
-include $(DEPFILES)
