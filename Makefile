# Detect number of processors
NUM_PROCESSORS := $(shell nproc)
MAKEFLAGS = -j$(NUM_PROCESSORS)

# ---------- Compiler and common flags ----------
CXX = g++
CXXFLAGS_COMMON = -std=c++20 -O3 -Wall -Wextra -flto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti
LDFLAGS_COMMON = -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# ---------- Shared library for both builds ----------
CHD_STATIC = ./deps/libchdr-static.a   # now the combined, full library

# ---------- Dynamic build (default) ----------
CXXFLAGS_DYN = $(CXXFLAGS_COMMON) -I./deps/libchdr/include
LIBS_DYN = -lreadline -lmount $(CHD_STATIC) -llzma -lz -lzstd
LDFLAGS_DYN = -lreadline -lmount -flto -ffunction-sections -fdata-sections -fno-plt $(LDFLAGS_COMMON)

# ---------- Static build (use STATIC=1) ----------
# Paths to static system libraries (adjust for your distribution, here Alpine Linux)
LZMA_STATIC = /usr/lib/liblzma.a
ZLIB_STATIC = /usr/lib/libz.a
ZSTD_STATIC = /usr/lib/libzstd.a
READLINE_STATIC = /usr/lib/libreadline.a
NCURSES_STATIC = /usr/lib/libncurses.a
MOUNT_STATIC = /usr/lib/libmount.a
BLKID_STATIC = /usr/lib/libblkid.a
ECONF_STATIC = /usr/lib/libeconf.a
INTL_STATIC = /usr/lib/libintl.a

CXXFLAGS_STAT = $(CXXFLAGS_COMMON) -I./deps/libchdr/include   # <-- FIXED: added include path
LIBS_STAT = $(CHD_STATIC) \
            $(LZMA_STATIC) $(ZLIB_STATIC) $(ZSTD_STATIC) \
            $(READLINE_STATIC) $(NCURSES_STATIC) $(MOUNT_STATIC) $(BLKID_STATIC) \
            $(ECONF_STATIC) $(INTL_STATIC) -pthread
LDFLAGS_STAT = -static $(LDFLAGS_COMMON)

# ---------- Select build type ----------
ifeq ($(STATIC),1)
    CXXFLAGS = $(CXXFLAGS_STAT)
    LIBS = $(LIBS_STAT)
    LDFLAGS = $(LDFLAGS_STAT)
else
    CXXFLAGS = $(CXXFLAGS_DYN)
    LIBS = $(LIBS_DYN)
    LDFLAGS = $(LDFLAGS_DYN)
endif

# ---------- Directories and source files ----------
SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
INSTALL_DIR = $(CURDIR)/bin

SRC_FILES = isocmd/main.cpp isocmd/history.cpp isocmd/verbose.cpp isocmd/isoDatabase.cpp isocmd/filtering.cpp isocmd/mount.cpp isocmd/umount.cpp isocmd/cpMvRm.cpp\
 isocmd/convert.cpp isocmd/ccd2iso_mdf2iso_nrg2iso.cpp isocmd/write2usb.cpp isocmd/stringManipulation.cpp isocmd/signalsAndTermios.cpp isocmd/select.cpp\
 isocmd/search.cpp isocmd/readline.cpp isocmd/progressbar.cpp isocmd/processInput.cpp isocmd/pagination.cpp isocmd/naturalSort.cpp isocmd/cmdAutomation.cpp\
 isocmd/printList.cpp isocmd/displayCode.cpp isocmd/setupOptions.cpp isocmd/help.cpp isocmd/tokenize.cpp isocmd/menu.cpp isocmd/chOwnership.cpp isocmd/chd2iso.cpp

OBJ_FILES = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SRC_FILES))

# ---------- Targets ----------
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
