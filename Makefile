CXX = g++
CXXFLAGS = -O2 -fopenmp
LIBS = -lreadline

SRC_FILES = mounter_elite_plus.cpp conversion_tools.cpp sanitization_readline.cpp
OBJ_FILES = $(SRC_FILES:.cpp=.o)
EXECUTABLE = mounter_elite_plus

.PHONY: all clean

all: $(EXECUTABLE)

$(EXECUTABLE): $(OBJ_FILES)
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LIBS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ_FILES) $(EXECUTABLE)
