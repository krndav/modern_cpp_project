CXX ?= g++
CPPFLAGS := -Iinclude
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDLIBS := -pthread
TARGET := http_server
SOURCES := src/main.cpp src/work_stealing_pool.cpp
OBJECTS := $(SOURCES:.cpp=.o)

.PHONY: all run test clean
all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CXX) $(OBJECTS) $(LDLIBS) -o $@

src/%.o: src/%.cpp include/work_stealing_pool.hpp
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

test:
	./tests/smoke_test.sh

clean:
	rm -f $(OBJECTS) $(TARGET)
