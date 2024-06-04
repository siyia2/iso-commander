CXX = g++
CXXFLAGS = -O2 -Wall -Werror
LIBS = -lreadline
LDFLAGS = -lreadline -lmount

# Use the number of available processors from nproc
NUM_PROCESSORS := $(shell nproc)

# Set the default number of jobs to the number of available processors
MAKEFLAGS = -j$(NUM_PROCESSORS)

SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
INSTALL_DIR = $(CURDIR)/bin
SRC_FILES = isocmd/main_general.cpp isocmd/cache.cpp conversion_tools/conversion_tools.cpp cp_mv_rm/cp_mv_rm.cpp isocmd/filtering.cpp isocmd/mount.cpp isocmd/umount.cpp
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
