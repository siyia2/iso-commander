CXX = g++
CXXFLAGS = -O3 -Wall -Wextra -flto -fmerge-all-constants -fdata-sections -ffunction-sections -fno-plt -fno-rtti
LIBS = -lreadline -lmount
LDFLAGS = -lreadline -lmount -flto -ffunction-sections -fdata-sections -fno-plt -Wl,--gc-sections -Wl,--strip-all -Wl,--as-needed -Wl,-z,relro -Wl,-z,now

# Use the number of available processors from nproc
NUM_PROCESSORS := $(shell nproc)

# Set the default number of jobs to the number of available processors
MAKEFLAGS = -j$(NUM_PROCESSORS)

SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
INSTALL_DIR = $(CURDIR)/bin
SRC_FILES = isocmd/main_general.cpp isocmd/cache.cpp isocmd/filtering.cpp isocmd/mount.cpp isocmd/umount.cpp isocmd/cp_mv_rm.cpp isocmd/conversion_tools.cpp
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
