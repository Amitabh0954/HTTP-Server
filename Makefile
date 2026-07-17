CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -g -fsanitize=address,undefined -pthread
LDFLAGS := -fsanitize=address,undefined -pthread

SRCS := main.cpp Socket.cpp HttpRequest.cpp HttpResponse.cpp Server.cpp ThreadPool.cpp
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
