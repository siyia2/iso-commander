CXX = g++
CXXFLAGS = -O2 -Wall -Werror -fopenmp

# Use the number of available processors from nproc
NUM_PROCESSORS := $(shell nproc 2>/dev/null)

# Set the default number of jobs to the number of available processors
MAKEFLAGS = -j$(NUM_PROCESSORS)

SRC_FILES = bulk_rename++.cpp case_modes.cpp
OBJ_FILES = $(SRC_FILES:.cpp=.o)
EXECUTABLE = bulk_rename++

INSTALL_DIR = ~/.local/bin

.PHONY: all clean install

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJ_FILES)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_FILES) $(EXECUTABLE)

run: $(EXECUTABLE)
	./$(EXECUTABLE)

install: $(EXECUTABLE)
	install -m 755 $(EXECUTABLE) $(INSTALL_DIR)

uninstall:
	rm -f $(INSTALL_DIR)/$(EXECUTABLE)
