
CXX ?= g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic -pthread
TARGET = task_scheduler

SRCS = main.cpp task_scheduler.cpp
OBJS = $(SRCS:.cpp=.o)
HEADERS = task_scheduler.hpp

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
