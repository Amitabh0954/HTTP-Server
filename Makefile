CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined
LDFLAGS := -fsanitize=address,undefined

SRCS := main.cpp Socket.cpp
OBJS := $(SRCS:.cpp=.o)
TARGET := server

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(LDFLAGS) -o $@ $^

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET)
