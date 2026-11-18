CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Iinclude
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=build/%.o)

all: build/tests

build/%.o: src/%.cpp $(wildcard include/simlab/*.hpp)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/tests: tests/test_main.cpp $(OBJ)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $^ -o $@

test: build/tests
	./build/tests

clean:
	rm -rf build

.PHONY: all test clean
