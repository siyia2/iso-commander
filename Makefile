# Detect number of processors
NUM_PROCESSORS := $(shell nproc)
MAKEFLAGS = -j$(NUM_PROCESSORS)

# ---------- Compiler and common flags ----------
CXX = g++

# ---- Sanitizer mode ----
ifeq ($(SANITIZE),1)
    # Optimize for debugging: low optimization, debug symbols, keep frame pointers
    CXXFLAGS_COMMON = -std=c++20 -O1 -g -fno-omit-frame-pointer -Wall -Wextra
    LDFLAGS_COMMON  = -fsanitize=address
else
    # Original production flags
    CXXFLAGS_COMMON = -std=c++20 -O3 -Wall -Wextra -flto -fmerge-all-constants \
                      -fdata-sections -ffunction-sections -fno-plt -fno-rtti
    LDFLAGS_COMMON  = -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now
endif

# ---------- Shared library for both builds ----------
CHD_STATIC = ./deps/libchdr-static.a

# ---------- Dynamic build (default) ----------
CXXFLAGS_DYN = $(CXXFLAGS_COMMON) -I./deps/libchdr/include
LIBS_DYN = -lreadline -lmount $(CHD_STATIC) -llzma -lz -lzstd

ifeq ($(SANITIZE),1)
    # Remove any LTO/opt-only flags from link line
    LDFLAGS_DYN = -lreadline -lmount $(LDFLAGS_COMMON)
else
    LDFLAGS_DYN = -lreadline -lmount -flto -ffunction-sections -fdata-sections -fno-plt $(LDFLAGS_COMMON)
endif

# ---------- Static build (use STATIC=1) ----------
# (Paths to static libraries remain unchanged)
ifeq ($(SANITIZE),1)
    # ASan + full static linkage often needs extra care; you may prefer dynamic build.
    # If you really need static, add -static-libasan and keep LDFLAGS_STAT simple.
    LDFLAGS_STAT = -static $(LDFLAGS_COMMON)
else
    LDFLAGS_STAT = -static $(LDFLAGS_COMMON)   # original: -static $(LDFLAGS_COMMON)
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
 isocmd/search.cpp isocmd/readline.cpp isocmd/progressbar.cpp isocmd/processInput.cpp isocmd/pagination.cpp isocmd/naturalSort.cpp isocmd/cmdAutomation.cpp isocmd/themes.cpp\
 isocmd/printList.cpp isocmd/displayCode.cpp isocmd/setupOptions.cpp isocmd/help.cpp isocmd/tokenize.cpp isocmd/menu.cpp isocmd/chOwnership.cpp isocmd/chd2iso.cpp isocmd/daa2iso.cpp

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
