# --- Standard Build Flags ---
CXX = g++
# Point to the BASE include directory (the one containing the libchdr/ subfolder)
CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -flto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti

LIBS = -lreadline -lmount
LDFLAGS = -lreadline -lmount -flto -ffunction-sections -fdata-sections -fno-plt -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# --- Static Build Flags ---
# When building static, you must manually list every sub-dependency
# Order: High-level lib -> Low-level system libs
#LIBS = -static -lreadline -lncursesw -lmount -lblkid -leconf -lintl -lz -llzma -lpthread
#STATIC_LDFLAGS = -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# To use the static flags later, you would swap LIBS for STATIC_LIBS

# Use the number of available processors from nproc
NUM_PROCESSORS := $(shell nproc)

# Set the default number of jobs to the number of available processors
MAKEFLAGS = -j$(NUM_PROCESSORS)

SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
INSTALL_DIR = $(CURDIR)/bin
SRC_FILES = isocmd/main.cpp isocmd/history.cpp  isocmd/verbose.cpp isocmd/isoDatabase.cpp isocmd/filtering.cpp isocmd/mount.cpp isocmd/umount.cpp isocmd/cpMvRm.cpp\
 isocmd/convert.cpp isocmd/ccd2iso_mdf2iso_nrg2iso.cpp isocmd/write2usb.cpp isocmd/stringManipulation.cpp isocmd/signalsAndTermios.cpp isocmd/select.cpp\
 isocmd/search.cpp isocmd/readline.cpp isocmd/progressbar.cpp isocmd/processInput.cpp isocmd/pagination.cpp isocmd/naturalSort.cpp isocmd/cmdAutomation.cpp\
 isocmd/printList.cpp isocmd/displayCode.cpp isocmd/setupOptions.cpp isocmd/help.cpp isocmd/tokenize.cpp isocmd/menu.cpp isocmd/chOwnership.cpp isocmd/convertChd.cpp isocmd/chd2iso.cpp
 
OBJ_FILES = $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(SRC_FILES))

all: isocmd

isocmd: $(OBJ_FILES)
	$(CXX) $(LDFLAGS) $^ -o $@ $(LIBS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -rf $(OBJ_DIR) isocmd

.PHONY: clean

install: isocmd
	mkdir bin
	install -m 755 isocmd $(INSTALL_DIR)

uninstall:
	rm -f $(INSTALL_DIR)/isocmd
