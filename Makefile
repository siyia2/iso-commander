CXX = g++
CXXFLAGS = -O2 -fopenmp
LIBS = -lreadline

# Determine the number of cores using nproc
CORES := $(shell nproc)

SRC_FILES = mounter_elite_plus.cpp conversion_tools.cpp sanitization_readline.cpp
OBJ_FILES = $(SRC_FILES:.cpp=.o)
EXECUTABLE = mounter_elite_plus

INSTALL_DIR = /usr/bin

.PHONY: all clean install

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJ_FILES)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_FILES) $(EXECUTABLE)

run: $(EXECUTABLE)
	OMP_NUM_THREADS=$(CORES) ./$(EXECUTABLE)

install: $(EXECUTABLE)
	install -m 755 $(EXECUTABLE) $(INSTALL_DIR)

uninstall:
	rm -f $(INSTALL_DIR)/$(EXECUTABLE)
