# Detect number of processors
NUM_PROCESSORS := $(shell nproc)
MAKEFLAGS = -j$(NUM_PROCESSORS)

# ---------- Compiler and common flags ----------
CXX = g++

# ---- Base flags (used by both normal and sanitizer builds) ----
CXXFLAGS_BASE = -std=c++20 -Wall -Wextra
LDFLAGS_BASE  = -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# ---- Normal build ----
CXXFLAGS_NORMAL = -O3 -flto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti
LDFLAGS_NORMAL  = -Wl,--gc-sections -Wl,--strip-all

# ---- Sanitizer build ----
ifeq ($(SANITIZE),1)
    CXXFLAGS_EXTRA = -O1 -g -fno-omit-frame-pointer -fsanitize=address
    LDFLAGS_EXTRA  = -fsanitize=address
    # Explicitly disable things that conflict with ASan
    CXXFLAGS_NORMAL = -O1 -g -fno-omit-frame-pointer
    LDFLAGS_NORMAL  =
endif

CXXFLAGS_COMMON = $(CXXFLAGS_BASE) $(CXXFLAGS_NORMAL) $(CXXFLAGS_EXTRA)
LDFLAGS_COMMON  = $(LDFLAGS_BASE) $(LDFLAGS_NORMAL) $(LDFLAGS_EXTRA)

# ---------- Shared library for both builds ----------
CHD_STATIC = ./deps/libchdr-static.a

# ---------- Dynamic build (default) ----------
CXXFLAGS_DYN = $(CXXFLAGS_COMMON) -I./deps/libchdr/include
LIBS_DYN = -lreadline -lmount $(CHD_STATIC) -llzma -lz -lzstd
LDFLAGS_DYN = -lreadline -lmount $(LDFLAGS_COMMON)

# ---------- Static build (use STATIC=1) ----------
LZMA_STATIC = /usr/lib/liblzma.a
ZLIB_STATIC = /usr/lib/libz.a
ZSTD_STATIC = /usr/lib/libzstd.a
READLINE_STATIC = /usr/lib/libreadline.a
NCURSES_STATIC = /usr/lib/libncurses.a
MOUNT_STATIC = /usr/lib/libmount.a
BLKID_STATIC = /usr/lib/libblkid.a
ECONF_STATIC = /usr/lib/libeconf.a
INTL_STATIC = /usr/lib/libintl.a

# Sanitizer + full static is tricky. Prefer dynamic for ASan builds.
ifeq ($(SANITIZE),1)
    # Keep it simple: don't strip, don't use LTO
    LDFLAGS_STAT = -static $(LDFLAGS_COMMON)
else
    LDFLAGS_STAT = -static $(LDFLAGS_COMMON) $(LDFLAGS_NORMAL)
endif

CXXFLAGS_STAT = $(CXXFLAGS_COMMON) -I./deps/libchdr/include
LIBS_STAT = $(CHD_STATIC) \
            $(LZMA_STATIC) $(ZLIB_STATIC) $(ZSTD_STATIC) \
            $(READLINE_STATIC) $(NCURSES_STATIC) $(MOUNT_STATIC) $(BLKID_STATIC) \
            $(ECONF_STATIC) $(INTL_STATIC) -pthread

# ---------- Select build type ----------
ifeq ($(STATIC),1)
    CXXFLAGS = $(CXXFLAGS_STAT)
    LIBS     = $(LIBS_STAT)
    LDFLAGS  = $(LDFLAGS_STAT)
else
    CXXFLAGS = $(CXXFLAGS_DYN)
    LIBS     = $(LIBS_DYN)
    LDFLAGS  = $(LDFLAGS_DYN)
endif

# ---------- Directories and source files ----------
SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
INSTALL_DIR = $(CURDIR)/bin

SRC_FILES = isocmd/main.cpp isocmd/history.cpp isocmd/verbose.cpp isocmd/isoDatabase.cpp isocmd/filtering.cpp isocmd/mount.cpp isocmd/umount.cpp isocmd/cpMvRm.cpp\
 isocmd/convert.cpp isocmd/ccd2iso_mdf2iso_nrg2iso.cpp isocmd/write2usb.cpp isocmd/stringManipulation.cpp isocmd/signalsAndTermios.cpp isocmd/select.cpp isocmd/sizeSpeedCalc.cpp\
 isocmd/search.cpp isocmd/readline.cpp isocmd/progressbar.cpp isocmd/processInput.cpp isocmd/pagination.cpp isocmd/naturalSort.cpp isocmd/cmdAutomation.cpp isocmd/themes.cpp isocmd/settingsEditor.cpp\
 isocmd/printList.cpp isocmd/displayCode.cpp isocmd/setupOptions.cpp isocmd/help.cpp isocmd/tokenize.cpp isocmd/menu.cpp isocmd/chOwnership.cpp isocmd/chd2iso.cpp isocmd/daa2iso.cpp

OBJ_FILES = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SRC_FILES))

all: isocmd

isocmd: $(OBJ_FILES)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) isocmd

install: isocmd
	mkdir -p $(INSTALL_DIR)
	install -m 755 isocmd $(INSTALL_DIR)

uninstall:
	rm -f $(INSTALL_DIR)/isocmd

.PHONY: clean install uninstall
