CXX = g++
CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -flto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti
LIBS = -lreadline -lmount
LDFLAGS = -lreadline -lmount -flto -ffunction-sections -fdata-sections -fno-plt -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# Flags for static builds
#CXX = g++
#CXXFLAGS = -std=c++20 -O3 -Wall -Wextra -flto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti
#LIBS = -static -L/usr/lib -lreadline -lmount -lncurses -lblkid -leconf -lintl -flto -ffunction-sections -fdata-sections -fno-plt
#LDFLAGS = -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# Use the number of available processors from nproc
NUM_PROCESSORS := $(shell nproc)

# Set the default number of jobs to the number of available processors
MAKEFLAGS = -j$(NUM_PROCESSORS)

SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
INSTALL_DIR = $(CURDIR)/bin
SRC_FILES = isocmd/main.cpp isocmd/history.cpp  isocmd/general.cpp  isocmd/verbose.cpp isocmd/cache.cpp isocmd/filtering.cpp isocmd/mount.cpp isocmd/umount.cpp isocmd/cp_mv_rm.cpp isocmd/conversions.cpp isocmd/ccd2iso_mdf2iso_nrg2iso.cpp isocmd/write2usb.cpp
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
