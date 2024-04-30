CXX = g++
CXXFLAGS = -O2 -Wall -Werror
LIBS = -lreadline
LDFLAGS = -lreadline -lmount

# Use the number of available processors from nproc
NUM_PROCESSORS := $(shell nproc 2>/dev/null)

# Set the default number of jobs to the number of available processors
MAKEFLAGS = -j$(NUM_PROCESSORS)

SRC_DIR = $(CURDIR)/src
OBJ_DIR = $(CURDIR)/obj
SRC_FILES = iso_commander.cpp conversion_tools.cpp sanitization_extraction_readline.cpp cp_mv_rm.cpp
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
	install -m 755 isocmd $(INSTALL_DIR)

uninstall:
	rm -f $(INSTALL_DIR)/isocmd
