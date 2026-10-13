CXX = g++-15
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic -pthread

TARGET = work_stealing_pool
SRCS = main.cpp work_stealing_pool.cpp
OBJS = $(SRCS:.cpp=.o)
HEADERS = work_stealing_pool.hpp

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) $(OBJS) -o $(TARGET)

%.o: %.cpp $(HEADERS)
	$(CXX) $(CXXFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)
